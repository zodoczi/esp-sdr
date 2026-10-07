"""Numeric regression checks for the sliced FFT and wire power conversion."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]

class SpectrumMath(unittest.TestCase):
    @unittest.skipUnless(shutil.which('cc'), 'Host C compiler unavailable')
    def test_sliced_fft_complex_tones_and_power_quantization(self):
        source = r'''
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <math.h>
#include <assert.h>
#include <stdlib.h>
#define SPEC_MAGIC 0x31435053u
static unsigned spec_n=256, spec_log2=8;
static int16_t fft_buf[4096], coefficients[2048];
static int16_t *dsps_fft_w_table_sc16=coefficients;
static uint16_t bin_of[2048];
static uint8_t frame_out[2080];
typedef struct __attribute__((packed)) {
 uint32_t magic,frame;uint64_t pair_index;uint32_t pairs;
 uint16_t ffts;uint8_t flags,gain;uint16_t drops;uint8_t nfft_log2,db_step;
} spec_header_t;
static struct {unsigned abandoned,frames,drops,ffts,work_max;} result;
static struct {bool max_hold;unsigned units_per_frame;} config={.units_per_frame=1};
static float accum[2048];
static unsigned txq_head,txq_tail;
static struct {typeof(result)*res;typeof(config)*cfg;struct{bool pending;}work[3];bool dropped;uint64_t last_ok;}st={.res=&result,.cfg=&config};
typedef typeof(result) ring_result_t;
static uint32_t bank[2048];
static const uint32_t *bank_ptr(unsigned b){return bank;}
static unsigned copied;
static void spec_unpack(const uint32_t*p,unsigned f,unsigned a,unsigned b){
 for(unsigned j=a;j<b;j++){fft_buf[2*j]=(int16_t)p[(f+j)%256];fft_buf[2*j+1]=0;copied++;}
}
static void spec_remove_dc(void){}
static unsigned esp_cpu_get_cycle_count(void){static unsigned ticks;return ++ticks;}
static unsigned esp_timer_get_time(void){return 0;}
static unsigned esp_rom_crc32_le(unsigned c,const void*p,unsigned n){return 0;}
static bool txq_push(const void*p,unsigned n){return true;}
#define scalar_telemetry(cycles,complete) ((void)0)
#include "ring_scalar.h"
#include "spectrum_fft.h"
static unsigned polls;
static void poll(void *ctx){assert(ctx==&polls);polls++;}
static unsigned rev(unsigned x,unsigned bits){unsigned r=0;while(bits--){r=r*2+(x&1);x>>=1;}return r;}
int main(void){
 for(unsigned j=0;j<1024;j++){
  unsigned k=rev(j,10);
  coefficients[2*j]=(int16_t)(32767*cos(2*M_PI*k/2048));
  coefficients[2*j+1]=(int16_t)(32767*sin(2*M_PI*k/2048));
 }
 for(unsigned j=0;j<256;j++)bin_of[j]=rev(j,8);
 const int tones[]={1,17,63,-23,-120};
 for(unsigned t=0;t<sizeof(tones)/sizeof(*tones);t++){
  for(unsigned j=0;j<256;j++){
   fft_buf[2*j]=(int16_t)lrint(12000*cos(2*M_PI*tones[t]*j/256));
   fft_buf[2*j+1]=(int16_t)lrint(12000*sin(2*M_PI*tones[t]*j/256));
  }
  memset(&scalar,0,sizeof(scalar));scalar.phase=1;scalar.half=128;scalar.groups=1;
  unsigned slices=0;while(scalar_work())assert(++slices<1000);
  unsigned peak=0;for(unsigned j=1;j<256;j++)if(frame_out[28+j]>frame_out[28+peak])peak=j;
  assert(peak==(unsigned)((tones[t]+256)%256));
  assert(abs((int)frame_out[28+peak]-(int)lrint(20*log10(12000.0*12000)))<=1);
  for(unsigned j=0;j<256;j++)if(j!=peak)assert(frame_out[28+j]+80<frame_out[28+peak]);
 }

 /* Two completed FFTs merge in linear power, including max-hold and a
  * final partial frame. No per-bin encoding occurs before the deadline. */
 for(unsigned maximum=0;maximum<2;maximum++){
  memset(&scalar,0,sizeof(scalar));memset(accum,0,sizeof(accum));
  memset(&result,0,sizeof(result));config.units_per_frame=2;config.max_hold=maximum;
  for(unsigned pass=0;pass<2;pass++){
   for(unsigned j=0;j<256;j++){fft_buf[2*j]=pass?20:10;fft_buf[2*j+1]=0;}
   txq_head=pass?0:1;
   scalar.phase=SCALAR_BINS;scalar.emit=0;scalar.index=pass*24576;
   while(scalar_work()){}
   assert(result.frames==pass);
  }
  spec_header_t *h=(void *)frame_out;
  assert(h->ffts==2 && h->pair_index==0 && h->pairs==24832);
  assert(result.ffts==2 && !scalar.merged);
  assert(frame_out[28]==(maximum?spectrum_power_code(400):spectrum_mean_power_code(250)));
  for(unsigned j=0;j<256;j++)assert(accum[j]==0);
  txq_head=1;
  scalar.phase=SCALAR_BINS;scalar.emit=0;scalar.index=50000;
  while(scalar_work()){}
  assert(result.frames==1 && scalar.merged==1);
  scalar_flush();while(scalar_work()){}
  assert(result.frames==2 && ((spec_header_t *)frame_out)->ffts==1);
 }
 txq_head=txq_tail=0;config.units_per_frame=1;config.max_hold=false;

 /* The live bank is released only after the entire window is private. */
 for(unsigned j=0;j<256;j++)bank[j]=j;
 memset(&scalar,0,sizeof(scalar));copied=0;
 scalar_accept(0,240,1234);assert(st.work[0].pending);
 scalar_work();assert(copied==256&&!st.work[0].pending&&scalar.phase==1);
 assert(scalar.cost[3]>0);
 for(unsigned j=0;j<256;j++)assert(fft_buf[2*j]==(j+240)%256);
 memset(bank,0,sizeof(bank)); /* reclaiming RF SRAM cannot alter the copy */
 for(unsigned j=0;j<256;j++)assert(fft_buf[2*j]==(j+240)%256);

 /* Compare every FFT output against the previous wide-arithmetic kernel,
  * including full-scale inputs that overflow signed 32-bit sums. */
 uint32_t rng=0x12345678u;
 int16_t reference[4096];
 for(spec_n=256;spec_n<=2048;spec_n*=2){
  for(unsigned trial=0;trial<66;trial++){
   for(unsigned j=0;j<2*spec_n;j++){
    rng=rng*1664525u+1013904223u;
    fft_buf[j]=reference[j]=trial==0?INT16_MIN:trial==1?INT16_MAX:(int16_t)(rng>>16);
   }
   for(unsigned half=spec_n/2,groups=1;half;half/=2,groups*=2){
    for(unsigned group=0;group<groups;group++)for(unsigned offset=0;offset<half;offset++){
     unsigned a=group*half*2+offset,b=a+half;
     int64_t wr=coefficients[2*group],wi=coefficients[2*group+1];
     int64_t ar=reference[2*a],ai=reference[2*a+1],br=reference[2*b],bi=reference[2*b+1];
     int64_t tr=wr*br+wi*bi,ti=wr*bi-wi*br;
     reference[2*a]=(int16_t)((ar*32767+tr+32767)>>16);
     reference[2*a+1]=(int16_t)((ai*32767+ti+32767)>>16);
     reference[2*b]=(int16_t)((ar*32767-tr+32767)>>16);
     reference[2*b+1]=(int16_t)((ai*32767-ti+32767)>>16);
    }
   }
   int16_t sliced[4096];memcpy(sliced,fft_buf,2*spec_n*sizeof(*fft_buf));polls=0;
   spectrum_fft_poll(sliced,spec_n,coefficients,poll,&polls);
   assert(polls && !memcmp(sliced,reference,2*spec_n*sizeof(*fft_buf)));
   memset(&scalar,0,sizeof(scalar));scalar.phase=1;scalar.half=spec_n/2;scalar.groups=1;
   while(scalar.phase==1)scalar_work();
   assert(!memcmp(fft_buf,reference,2*spec_n*sizeof(*fft_buf)));
  }
 }
 assert(spectrum_power_code(0)==0);
 for(uint64_t p=1;p<=UINT32_MAX;p=p*103/100+1){
  int expected=(int)lrint(20*log10((double)p));if(expected>255)expected=255;
  assert(abs((int)spectrum_power_code(p)-expected)<=1);
  for(unsigned count=1;count<=8;count++){
   float mean=(float)p/count;
   int mean_expected=(int)lrint(20*log10((double)mean));
   if(mean_expected<0)mean_expected=0;if(mean_expected>255)mean_expected=255;
   assert(abs((int)spectrum_mean_power_code(mean)-mean_expected)<=1);
  }
 }
 assert(spectrum_mean_power_code(0)==0);
 assert(spectrum_mean_power_code(INFINITY)==255);
 assert(spectrum_mean_power_code(-INFINITY)==0);
 assert(spectrum_mean_power_code(NAN)==0);
 assert(spectrum_mean_power_code(-1)==0);
 assert(spectrum_mean_power_code(0.99f)==0);
 assert(spectrum_complex_power(INT16_MIN,INT16_MIN)==2147483648u);
 assert(spectrum_complex_power(INT16_MAX,INT16_MAX)==2147352578u);
 return 0;
}
'''
        with tempfile.TemporaryDirectory() as tmp:
            src=Path(tmp)/'numeric.c';src.write_text(source)
            exe=Path(tmp)/'numeric'
            subprocess.run(['cc','-std=gnu11','-O2','-fsanitize=undefined','-fno-sanitize-recover=all','-I'+str(ROOT/'main/common'),str(src),'-lm','-o',str(exe)],check=True)
            subprocess.run([str(exe)],check=True)

    @unittest.skipUnless(shutil.which('cc'), 'Host C compiler unavailable')
    def test_dc_tracker_preserves_changes_and_bounds_hann_correction(self):
        source = r'''
#include "spectrum_dc.h"
#include <assert.h>
#include <string.h>
unsigned spectrum_dc_mode=1;
int main(void) {
 for(unsigned n=256;n<=2048;n*=2) {
  spectrum_dc_t dc={0};int16_t x[4096]={0};
  x[0]=1000;x[1]=-800;x[n]=-500;x[n+1]=400;x[2*n-2]=-500;x[2*n-1]=400;
  spectrum_dc_apply(&dc,x,n);
  for(unsigned j=0;j<2*n;j++)assert(x[j]==0);
  x[0]=2000;x[1]=-1600;x[8]=1234;x[9]=-2345;
  spectrum_dc_apply(&dc,x,n);
  assert(x[0]>900 && x[1]<-700);assert(x[8]==1234 && x[9]==-2345);
  for(unsigned k=0;k<1024;k++){memset(x,0,sizeof(x));x[0]=2000;x[1]=-1600;spectrum_dc_apply(&dc,x,n);}
  assert(x[0]>=-1 && x[0]<=1);assert(x[1]>=-1 && x[1]<=1);
  spectrum_dc_mode=0;memset(x,0,sizeof(x));x[0]=32767;x[1]=-32768;x[n]=32767;x[n+1]=-32768;
  spectrum_dc_apply(&dc,x,n);assert(x[0]==0 && x[1]==0);assert(x[n]==32767 && x[n+1]==-32768);
  spectrum_dc_mode=1;
 }
}
'''
        with tempfile.TemporaryDirectory() as tmp:
            src=Path(tmp)/'dc.c';src.write_text(source)
            exe=Path(tmp)/'dc'
            subprocess.run(['cc','-std=c11','-O2','-fsanitize=undefined','-I'+str(ROOT/'main/common'),str(src),'-o',str(exe)],check=True)
            subprocess.run([str(exe)],check=True)
