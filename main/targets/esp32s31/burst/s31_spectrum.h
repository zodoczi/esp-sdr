/* S31: continuous RF bank rotation on core 0, SIMD FFT worker on core 1.
 * Included by receiver.c so radio setup remains owned by that backend. */
#include <stdatomic.h>
#include <math.h>
#include "dsps_fft2r.h"
#include "spectrum_dc.h"
#include "spectrum_stats.h"
#include "ring_io.h"
#include "esp_heap_caps.h"

#define S31_NMAX 2048u
#define S31_RING 16384u
#define S31_MASK (S31_RING-1u)
#define S31_UNIT 12288u
#define S31_GUARD 1024u
#define S31_END_GUARD 2048u
#define S31_LATE 800u
#define S31_QUEUE 8u
#define S31_TX_BYTES 16384u
#define S31_MARK 0xa5c33c5au
#define LOAD(p) atomic_load_explicit(&(p),memory_order_acquire)
#define STORE(p,v) atomic_store_explicit(&(p),(v),memory_order_release)
extern int s31_fft2r_sc16_rnd(int16_t *,int,int16_t *);
static const unsigned s31_rates[]={80000000,40000000,20000000,10000000,8000000,4000000};
static const unsigned s31_dividers[]={0,1,3,5,7,9};
typedef struct {unsigned bank,seq,first,count,phase,gain;uint64_t index;} s31_job_t;
typedef struct {unsigned n,logn,rate,stride,upf,ms;bool maximum,stats;} s31_config_t;
static struct {
    _Atomic unsigned run,done,end,posted,taken,reader,bank_seq[2],txhead,txtail;
    _Atomic unsigned busy0,busy1,late,skipped0;
    s31_job_t jobs[S31_QUEUE];
    s31_config_t cfg;
    uint8_t tx[S31_TX_BYTES];
    unsigned ffts,frames,drops,skipped,work_max;
    unsigned heap_free,heap_largest;
} s31;
static TaskHandle_t s31_worker_task;
/* SIMD also loads constants from its stack; keep it in HP SRAM, not RTC RAM. */
static StaticTask_t s31_worker_tcb;
static StackType_t s31_worker_stack[4096/sizeof(StackType_t)] __attribute__((aligned(16)));
static spectrum_workspace_t s31_work;
static int16_t s31_win2[2*S31_NMAX] __attribute__((aligned(16)));
extern void s31_unpack_iq10_win(const uint32_t *,int16_t *,const int16_t *,unsigned);
static uint16_t s31_bin[S31_NMAX];
static int16_t s31_log_e[256],s31_log_m[128];

static inline volatile uint32_t *s31_bank(unsigned b){return (void*)(0x2f040000u+b*0x20000u);}
static void IRAM_ATTR s31_select(unsigned bank){
    REG_WRITE(HP_SYSTEM_TCM_DATA_DUMP_CTRL_REG,bank?0xff000000u:0x00ff0000u);
    __asm__ volatile("fence rw,rw" ::: "memory");
}
static void IRAM_ATTR s31_mark(unsigned b,unsigned at,unsigned n){
    volatile uint32_t *p=s31_bank(b);
    while(n){
        unsigned count=n<S31_RING-at?n:S31_RING-at;
        volatile uint32_t *q=p+at,*end=q+count;
        while(q!=end)*q++=S31_MARK;
        n-=count;at=0;
    }
    __asm__ volatile("fence rw,rw" ::: "memory");
}
static unsigned IRAM_ATTR s31_boundary(unsigned b,unsigned at,unsigned n,bool end){
    volatile uint32_t *p=s31_bank(b);unsigned lo=0,hi=n-1;
    if((p[at&S31_MASK]==S31_MARK)==end || (p[(at+hi)&S31_MASK]==S31_MARK)!=end)return UINT32_MAX;
    while(lo<hi){unsigned mid=(lo+hi)/2;if((p[(at+mid)&S31_MASK]==S31_MARK)==end)hi=mid;else lo=mid+1;}
    return (at+lo)&S31_MASK;
}
static bool s31_send(const void *data,unsigned size){
    unsigned head=LOAD(s31.txhead),tail=LOAD(s31.txtail);
    if(size>S31_TX_BYTES-(head-tail))return false;
    const uint8_t *p=data;
    for(unsigned j=0;j<size;j++)s31.tx[(head+j)%S31_TX_BYTES]=p[j];
    STORE(s31.txhead,head+size);return true;
}
static void IRAM_ATTR s31_pump(void){
    unsigned tail=LOAD(s31.txtail),used=LOAD(s31.txhead)-tail;if(!used)return;
    unsigned offset=tail%S31_TX_BYTES,n=S31_TX_BYTES-offset;
    if(n>used)n=used;
    if(n>64)n=64;
    int sent=ring_write(s31.tx+offset,n);if(sent>0)STORE(s31.txtail,tail+sent);
}
static void s31_put16(unsigned at,uint16_t v){memcpy(s31_work.frame+at,&v,2);}
static void s31_put32(unsigned at,uint32_t v){memcpy(s31_work.frame+at,&v,4);}
static void s31_emit(unsigned ffts,unsigned pairs,uint64_t index,unsigned gain,bool gaps){
    if(!ffts){s31.drops++;return;}
    unsigned n=s31.cfg.n;
    memcpy(s31_work.frame,"SPC1",4);s31_put32(4,s31.frames+s31.drops);memcpy(s31_work.frame+8,&index,8);
    s31_put32(16,pairs);s31_put16(20,ffts>65535?65535:ffts);
    s31_work.frame[22]=(s31.cfg.maximum?1:0)|(gaps?2:0)|(s31.drops?4:0);s31_work.frame[23]=gain;
    s31_put16(24,s31.drops>65535?65535:s31.drops);s31_work.frame[26]=s31.cfg.logn;s31_work.frame[27]=2;
    int offset=s31.cfg.maximum?0:lrintf(16*6.0206f*log2f((float)ffts));
    for(unsigned j=0;j<n;j++){
        union{float f;uint32_t u;}v={.f=s31_work.power[j]};
        int code=(s31_log_e[(v.u>>23)&255]+s31_log_m[(v.u>>16)&127]-offset)>>4;
        s31_work.frame[28+s31_bin[j]]=code<0?0:code>255?255:code;s31_work.power[j]=0;
    }
    s31_put32(28+n,esp_rom_crc32_le(0,s31_work.frame,28+n));
    if(s31_send(s31_work.frame,n+32))s31.frames++;else s31.drops++;
}
/* Integer squaring needs only one integer-to-float conversion per bin.
 * IQ16 squares sum to at most 2^31, so the unsigned sum cannot overflow.
 * All powers are finite; a direct comparison avoids libm NaN checks. */
static void __attribute__((noinline)) s31_accumulate(const int16_t *restrict x,float *restrict power,unsigned n,bool maximum){
    if(maximum){
        #pragma GCC unroll 4
        for(unsigned j=0;j<n;j++){
            int32_t re=x[2*j],im=x[2*j+1];float p=(float)((uint32_t)(re*re)+(uint32_t)(im*im));
            float old=power[j];
            __asm__("fmax.s %0,%1,%2":"=f"(power[j]):"f"(old),"f"(p));
        }
    }else{
        #pragma GCC unroll 4
        for(unsigned j=0;j<n;j++){
            int32_t re=x[2*j],im=x[2*j+1];power[j]+=(float)((uint32_t)(re*re)+(uint32_t)(im*im));
        }
    }
}
static void s31_worker(void *arg){
    (void)arg;unsigned seen=0;
    for(;;){
        ulTaskNotifyTake(pdTRUE,portMAX_DELAY);
        unsigned run=LOAD(s31.run);if(run==seen)continue;seen=run;
        const s31_config_t cfg=s31.cfg;unsigned taken=0,units=0,ffts=0,pairs=0,gain=0;uint64_t index=0;
        bool gaps=false;spectrum_dc_t dc={0};
        unsigned stat_t=esp_cpu_get_cycle_count(),stat_b0=0,stat_b1=0,stat_ffts=0;
        for(;;){
            if(taken==LOAD(s31.posted)){if(LOAD(s31.end))break;taskYIELD();continue;}
            s31_job_t job=s31.jobs[taken%S31_QUEUE];unsigned t0=esp_cpu_get_cycle_count();
            if(!units){index=job.index;gain=job.gain;}
            unsigned blocks=job.count>>cfg.logn;
            for(unsigned block=job.phase;block<blocks;block+=cfg.stride){
                unsigned start=esp_cpu_get_cycle_count(),at=job.first+block*cfg.n;bool valid=true;
                for(unsigned from=0;from<cfg.n;from+=128){
                    /* Publish the read before testing ownership. Paired full
                     * fences prevent a store/load race with bank revocation. */
                    STORE(s31.reader,job.bank+1);atomic_thread_fence(memory_order_seq_cst);
                    if(LOAD(s31.bank_seq[job.bank])!=job.seq){valid=false;STORE(s31.reader,0);break;}
                    unsigned to=from+128,j=from;
                    while(j<to){
                        unsigned first=(at+j)&S31_MASK;
                        unsigned available=first+4<S31_RING?S31_RING-first-4:0;
                        unsigned count=to-j<available?to-j:available,bulk=count&~7u;
                        if(bulk){
                            s31_unpack_iq10_win((const uint32_t *)s31_bank(job.bank)+first,s31_work.fft+2*j,s31_win2+2*j,bulk/8);
                            j+=bulk;
                        }else{
                            unsigned end=j+8<to?j+8:to;
                            for(;j<end;j++){
                                uint32_t w=s31_bank(job.bank)[(at+j)&S31_MASK];
                                int32_t i=(int32_t)(w<<22)>>22,q=(int32_t)(w<<12)>>22;
                                s31_work.fft[2*j]=(i*s31_work.window[j])>>9;s31_work.fft[2*j+1]=(q*s31_work.window[j])>>9;
                            }
                        }
                    }
                    STORE(s31.reader,0);
                }
                if(!valid){s31.skipped+=(blocks-block+cfg.stride-1)/cfg.stride;gaps=true;break;}
                if(block+cfg.stride>=blocks){unsigned sequence=job.seq;atomic_compare_exchange_strong(&s31.bank_seq[job.bank],&sequence,0);}
                s31_fft2r_sc16_rnd(s31_work.fft,cfg.n,dsps_fft_w_table_sc16);
                spectrum_dc_apply(&dc,s31_work.fft,cfg.n);
                s31_accumulate(s31_work.fft,s31_work.power,cfg.n,cfg.maximum);
                ffts++;s31.ffts++;
                unsigned dt=esp_cpu_get_cycle_count()-start;if(dt>s31.work_max)s31.work_max=dt;
            }
            unsigned sequence=job.seq;atomic_compare_exchange_strong(&s31.bank_seq[job.bank],&sequence,0);
            if(units && index+pairs!=job.index)gaps=true;
            pairs=(unsigned)(job.index+job.count-index);units++;
            if(ffts && (LOAD(s31.txhead)==LOAD(s31.txtail) || ffts>=60000)){
                s31_emit(ffts,pairs,index,gain,gaps);ffts=units=pairs=0;gaps=false;
            }
            atomic_fetch_add_explicit(&s31.busy1,esp_cpu_get_cycle_count()-t0,memory_order_relaxed);
            if(cfg.stats){
                unsigned now=esp_cpu_get_cycle_count(),dt=now-stat_t;
                if(dt>=CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ*250000u){
                    unsigned b0=LOAD(s31.busy0),b1=LOAD(s31.busy1),df=s31.ffts-stat_ffts;
                    unsigned c0=(b0-stat_b0)/(dt/1000),c1=(b1-stat_b1)/(dt/1000);
                    uint64_t coverage=(uint64_t)df*cfg.n*CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ*1000000000ull/((uint64_t)dt*s31_rates[cfg.rate]);
                    spectrum_stats_frame_t f={.magic=0x31535053,.core0=c0>1000?1000:c0,.core1=c1>1000?1000:c1,
                        .coverage=coverage>1000?1000:coverage,.mode=1,.heap_free=s31.heap_free,.heap_largest=s31.heap_largest,
                        .abandoned=s31.skipped+LOAD(s31.skipped0),.drops=s31.drops,.late_max=LOAD(s31.late),
                        .queue=(LOAD(s31.txhead)-LOAD(s31.txtail))*1000/S31_TX_BYTES,
                        .ffts_per_s=(uint64_t)df*CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ*1000000ull/dt};
                    uint8_t bytes[40];memcpy(bytes,&f,36);uint32_t crc=esp_rom_crc32_le(0,bytes,36);memcpy(bytes+36,&crc,4);s31_send(bytes,40);
                    stat_t=now;stat_b0=b0;stat_b1=b1;stat_ffts=s31.ffts;
                }
            }
            STORE(s31.taken,++taken);
        }
        if(units)s31_emit(ffts,pairs,index,gain,gaps);
        STORE(s31.done,seen);
    }
}
static bool s31_spectrum_init(void){
    if(s31_worker_task)return true;
    if(!spectrum_fft_init())return false;
    s31_work=spectrum_workspace();
    for(unsigned e=0;e<256;e++)s31_log_e[e]=lrintf(16*6.0206f*((float)e-127))+8;
    for(unsigned m=0;m<128;m++)s31_log_m[m]=lrintf(16*6.0206f*log2f(1+((float)m+.5f)/128));
    s31_worker_task=xTaskCreateStaticPinnedToCore(s31_worker,"s31_fft",sizeof(s31_worker_stack),NULL,20,s31_worker_stack,&s31_worker_tcb,1);
    return s31_worker_task!=NULL;
}
static void IRAM_ATTR s31_run(unsigned *status,unsigned *detail,unsigned *units,uint64_t *pairs,unsigned *stopped){
    const s31_config_t cfg=s31.cfg;unsigned start_probe[2]={S31_RING-512,0},end_probe[2]={0,0};
    s31_mark(0,0,S31_RING);s31_mark(1,0,S31_RING);
    unsigned ctrl=(reset_ctrl&~0x803fffffu)|S31_RING|BIT(17),expected=0,bank=0,seq=0,phase=0;
    unsigned tail=0,taken=0;int64_t last_output=esp_timer_get_time(),last_work=last_output,start_time=last_output;
    unsigned irq=portSET_INTERRUPT_MASK_FROM_ISR();
    REG_WRITE(DUMP_MODE,(REG_READ(DUMP_MODE)&~0x01fe0000u)|(s31_dividers[cfg.rate]<<21)|(3u<<17));
    REG_WRITE(DUMP_CTRL,ctrl|BIT(18));REG_WRITE(DUMP_CTRL,ctrl);s31_select(0);REG_WRITE(DUMP_CTRL,ctrl|BIT(31));
    for(unsigned unit=0;;unit++){
        unsigned next=bank^1;
        unsigned ptr,written,unit_start=esp_cpu_get_cycle_count();bool stop=false,prepared=false;
        for(;;){
            ptr=REG_READ(DUMP_MODE)&S31_MASK;written=(ptr-expected)&S31_MASK;
            if(written>=S31_UNIT)break;
            const unsigned cpp=CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ*1000000u/s31_rates[cfg.rate];
            /* Wait for the worker when possible, then revoke with enough time
             * for sentinel preparation before the next RF bank switch. */
            if(!prepared && (!LOAD(s31.bank_seq[next]) || written+16000/cpp+512>=S31_UNIT)){
                unsigned prep=esp_cpu_get_cycle_count();STORE(s31.bank_seq[next],0);atomic_thread_fence(memory_order_seq_cst);
                while(LOAD(s31.reader)==next+1){if(esp_cpu_get_cycle_count()-prep>320000){*status=3;*detail=1;break;}}
                if(*status)break;
                start_probe[next]=(expected+S31_UNIT)&S31_MASK;end_probe[next]=(expected+2*S31_UNIT)&S31_MASK;
                s31_mark(next,start_probe[next],S31_GUARD);s31_mark(next,end_probe[next],S31_END_GUARD);
                prepared=true;atomic_fetch_add_explicit(&s31.busy0,esp_cpu_get_cycle_count()-prep,memory_order_relaxed);continue;
            }
            unsigned dt=esp_cpu_get_cycle_count()-unit_start;
            if(dt>2*S31_RING*(CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ*1000000u/s31_rates[cfg.rate])){*status=3;*detail=dt;break;}
            /* USB and timer calls stay out of the final polling margin. */
            if(written+2048>=S31_UNIT)continue;
            unsigned tx=esp_cpu_get_cycle_count();s31_pump();atomic_fetch_add_explicit(&s31.busy0,esp_cpu_get_cycle_count()-tx,memory_order_relaxed);
            if(written>S31_UNIT/2){
                int64_t now=esp_timer_get_time();
                unsigned sent=LOAD(s31.txtail),done=LOAD(s31.taken);
                /* A long averaging interval is not a stalled USB reader. */
                if(sent!=tail || sent==LOAD(s31.txhead))last_output=now;
                if(done!=taken)last_work=now;
                tail=sent;taken=done;
                if(cfg.ms && now-start_time>=(int64_t)cfg.ms*1000)stop=true;
                if(ring_input_available() || now-last_output>2000000 || now-last_work>2000000){stop=true;*stopped=1;}
            }
        }
        if(*status)break;
        if(!prepared){*status=3;*detail=2;break;}
        unsigned late=written-S31_UNIT;if(late>LOAD(s31.late))STORE(s31.late,late);
        if(late>S31_LATE){*status=2;*detail=late;break;}
        unsigned t1=esp_cpu_get_cycle_count();
        if(stop){REG_WRITE(DUMP_CTRL,ctrl);REG_WRITE(HP_SYSTEM_TCM_DATA_DUMP_CTRL_REG,0);}
        else s31_select(next);
        __asm__ volatile("fence rw,rw" ::: "memory");
        unsigned settle=esp_cpu_get_cycle_count();while(esp_cpu_get_cycle_count()-settle<16*(CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ*1000000u/s31_rates[cfg.rate])){}
        unsigned first=s31_boundary(bank,start_probe[bank],unit?S31_GUARD:1024,false);
        unsigned end=s31_boundary(bank,unit?end_probe[bank]:ptr,unit?S31_END_GUARD:1024,true);
        if(first!=expected){*status=4;*detail=first;break;}
        if(end==UINT32_MAX){*status=5;*detail=ptr;break;}
        unsigned count=(end-first)&S31_MASK;if(count<S31_UNIT || count>S31_UNIT+S31_LATE+128){*status=6;*detail=count;break;}
        unsigned blocks=count>>cfg.logn,todo=phase<blocks?(blocks-phase+cfg.stride-1)/cfg.stride:0;
        unsigned posted=LOAD(s31.posted);
        if(posted-LOAD(s31.taken)<S31_QUEUE){
            s31.jobs[posted%S31_QUEUE]=(s31_job_t){.bank=bank,.seq=++seq,.first=first,.count=count,.phase=phase,.index=*pairs,.gain=s31_bank(bank)[first]>>20};
            STORE(s31.bank_seq[bank],seq);STORE(s31.posted,posted+1);
        }else atomic_fetch_add_explicit(&s31.skipped0,todo,memory_order_relaxed);
        phase+=todo*cfg.stride;phase-=blocks;
        (*units)++;*pairs+=count;expected=end;bank=next;
        atomic_fetch_add_explicit(&s31.busy0,esp_cpu_get_cycle_count()-t1,memory_order_relaxed);
        if(stop)break;
    }
    REG_CLR_BIT(DUMP_CTRL,BIT(31));REG_WRITE(HP_SYSTEM_TCM_DATA_DUMP_CTRL_REG,0);__asm__ volatile("fence rw,rw" ::: "memory");
    STORE(s31.end,1);
    while(ring_input_available()){uint8_t c;if(ring_read_byte(&c)!=1||c=='\n')break;}
    portCLEAR_INTERRUPT_MASK_FROM_ISR(irq);
    int64_t deadline=esp_timer_get_time()+2000000;
    while(LOAD(s31.done)!=LOAD(s31.run) && esp_timer_get_time()<deadline){s31_pump();vTaskDelay(1);}
    /* A failed DSP worker cannot be reused or left owning live state. */
    if(LOAD(s31.done)!=LOAD(s31.run)){esp_restart();}
    deadline=esp_timer_get_time()+500000;
    while(LOAD(s31.txhead)!=LOAD(s31.txtail)&&esp_timer_get_time()<deadline){s31_pump();vTaskDelay(1);}
}
static void s31_profiles(void){
    reply("SPECINFO {\"continuous\":true,\"transports\":[\"USB\"],\"profiles\":[");
    bool comma=false;
    /* Bound default USB traffic to roughly 250 kB/s at the fastest rates. */
    for(unsigned r=0;r<6;r++)for(unsigned n=256;n<=S31_NMAX;n*=2){
        unsigned stride=r==0?8:r==1?4:r==2?2:1,upf=r==0?n/32:r==1?n/64:n/128;
        if(!upf)upf=1;
        reply("%s[%u,%u,%u,%u,%u,1]",comma?",":"",s31_rates[r],r,n,stride,upf);comma=true;
    }
    reply("]}\n");
}
static bool s31_spectrum_command(const char *line){
    if(burst_serial_port()!=BURST_SERIAL_USB)return false;
    if(!strcmp(line,"SPECINFO?")){if(!s31_spectrum_init())return false;s31_profiles();return true;}
    if(strncmp(line,"SPEC ",5))return false;
    s31_config_t cfg={0};unsigned det,stats=0;char extra;
    int fields=sscanf(line,"SPEC %u %u %u %u %u %u %u %c",&cfg.ms,&cfg.stride,&cfg.upf,&det,&cfg.rate,&cfg.n,&stats,&extra);
    if((fields!=6&&fields!=7)||stats>1||det>1||cfg.ms>86400000u||!cfg.stride||cfg.stride>64||!cfg.upf||cfg.upf>1000||cfg.rate>5||cfg.n<256||cfg.n>S31_NMAX||(cfg.n&(cfg.n-1))){reply("ERR spec_args\n");return true;}
    cfg.stride=1; /* Process until the bank deadline, not a host-selected stride. */
    if(!s31_spectrum_init()){reply("ERR spectrum_memory\n");return true;}
    s31_work=spectrum_workspace();
    cfg.maximum=det;cfg.stats=stats;while((1u<<cfg.logn)<cfg.n)cfg.logn++;
    for(unsigned j=0;j<cfg.n;j++){
        s31_work.window[j]=lrintf(16383.5f*(1-cosf(2*M_PI*j/cfg.n)));
        s31_win2[2*j]=s31_win2[2*j+1]=s31_work.window[j];
        unsigned b=0;for(unsigned k=0;k<cfg.logn;k++)b=b*2+((j>>k)&1);s31_bin[j]=b;
    }
    memset(s31_work.power,0,S31_NMAX*sizeof(float));s31.cfg=cfg;s31.ffts=s31.frames=s31.drops=s31.skipped=s31.work_max=0;
    STORE(s31.end,0);STORE(s31.posted,0);STORE(s31.taken,0);STORE(s31.reader,0);STORE(s31.bank_seq[0],0);STORE(s31.bank_seq[1],0);
    STORE(s31.txhead,0);STORE(s31.txtail,0);STORE(s31.busy0,0);STORE(s31.busy1,0);STORE(s31.late,0);STORE(s31.skipped0,0);
    s31.heap_free=heap_caps_get_free_size(MALLOC_CAP_INTERNAL);s31.heap_largest=heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
    prepare_rx();filter_apply();
    reply("SPEC %u %u %u %u\n",cfg.n,s31_rates[cfg.rate],S31_UNIT,frequency_mhz);
    STORE(s31.run,LOAD(s31.run)+1);xTaskNotifyGive(s31_worker_task);
    unsigned status=0,detail=0,units=0,stopped=0;uint64_t pairs=0;int64_t start=esp_timer_get_time();
    s31_run(&status,&detail,&units,&pairs,&stopped);filter_restore();
    reply("SPECEND %u %u %u %llu %lld %u %u %u %u %u %u %u\n",status,detail,units,(unsigned long long)pairs,
        (long long)(esp_timer_get_time()-start),LOAD(s31.late),s31.work_max,s31.frames,s31.drops,s31.skipped+LOAD(s31.skipped0),s31.ffts,stopped);
    return true;
}
#undef LOAD
#undef STORE
