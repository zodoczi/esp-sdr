"""Keep copied FFT work alive across RF bank retirement without mixing frames."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT=Path(__file__).resolve().parents[1]

class RingRetirement(unittest.TestCase):
    @unittest.skipUnless(shutil.which('cc'), 'Host C compiler unavailable')
    def test_retirement_at_each_processing_phase(self):
        src=(ROOT/'main/common/ring_capture.c').read_text()
        unit=src[src.index('RING_HOT static void unit_done('):src.index('/* Bank b is about to')]
        work=src[src.index('RING_HOT static bool work_slice('):src.index('RING_HOT static void fail(')]
        harness=r'''
#include <stdbool.h>
#include <stdint.h>
#include <assert.h>
#include <string.h>
#define RING_HOT
#define CHUNK 128
#define UNPACK_CHUNK 512
#define S3_FFT_STAGE(a,b,c) ((void)0)
enum { BLK_IDLE, BLK_UNPACK, BLK_FFT, BLK_ACCUM };
typedef struct {bool pending;unsigned first,count,next_block,blocks;uint64_t index;uint8_t gain;} work_t;
static struct {unsigned stride,units_per_frame;bool max_hold;} cfg={1,1,false};
static struct {unsigned abandoned,ffts,work_max;} result;
static struct {
 typeof(cfg)*cfg;typeof(result)*res;work_t work[3];unsigned fifo[3],fifo_len;
 unsigned frame_units,frame_ffts,frame_pairs,frame_flags,frame_gain;uint64_t frame_index;
 unsigned phase,blk_bank,blk_at,pos,fft_stage;bool emitting;
}st;
static unsigned txq_head,txq_tail;
static unsigned spec_n=2048,spec_log2=11,closed,commits;
static int16_t fft_buf[4096];
static void frame_close(void){assert(st.phase==BLK_IDLE);closed++;st.frame_units=st.frame_ffts=0;}
static void emit_chunk(void){}
static void spec_remove_dc(void){}
static void spec_accumulate(bool max,unsigned a,unsigned b){assert(a<b&&b<=2048);commits+=b-a;}
static const uint32_t *bank_ptr(unsigned b){return 0;}
static void spec_unpack(const uint32_t*p,unsigned a,unsigned b,unsigned c){}
static unsigned esp_cpu_get_cycle_count(void){return 0;}
'''
        check=r'''
static void setup(unsigned phase){
 memset(&st,0,sizeof(st));memset(&result,0,sizeof(result));closed=commits=0;
 st.cfg=&cfg;st.res=&result;st.phase=phase;st.blk_bank=0;st.fifo_len=1;
 st.work[0]=(work_t){.pending=true,.count=12000,.next_block=1,.blocks=1,.index=123,.gain=42};
}
int main(void){
 for(unsigned phase=BLK_FFT;phase<=BLK_ACCUM;phase++)for(unsigned stage=0;stage<spec_log2;stage++){
  setup(phase);st.fft_stage=stage;unit_done();assert(!st.work[0].pending&&st.fifo_len==0);
  assert(st.phase==phase&&closed==0&&result.abandoned==0);
  assert(st.frame_index==123&&st.frame_pairs==12000);
  while(st.phase!=BLK_IDLE)assert(work_slice());
  assert(closed==1&&commits==2048&&result.ffts==1);
 }
 setup(BLK_UNPACK);unit_done();assert(st.phase==BLK_IDLE&&result.abandoned==1&&commits==0);
 setup(BLK_ACCUM);st.fifo_len=2;st.fifo[1]=1;st.work[1]=(work_t){.pending=true,.count=12000,.blocks=0};
 unit_done();unit_done();assert(closed==0&&st.frame_pairs==24000);
 while(st.phase!=BLK_IDLE)assert(work_slice());assert(closed==1&&commits==2048);
 return 0;
}
'''
        with tempfile.TemporaryDirectory() as tmp:
            path=Path(tmp)/'retirement.c';path.write_text(harness+unit+work+check)
            exe=Path(tmp)/'retirement'
            subprocess.run(['cc','-std=gnu11',str(path),'-o',str(exe)],check=True)
            subprocess.run([str(exe)],check=True,timeout=5)
