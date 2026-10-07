"""Guard the unrolled RF-bank fill against misaligned tails and wraparound."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class RingSentinels(unittest.TestCase):
    @unittest.skipUnless(shutil.which('cc'), 'Host C compiler unavailable')
    def test_wrapped_regions_and_untouched_neighbors(self):
        source = (ROOT / 'main/common/ring_capture.c').read_text()
        source = source[source.index('RING_HOT static void fill_sentinels('):
                        source.index('/* First written pair')]
        source = source.replace('__asm__ volatile("fence rw,rw" ::: "memory");', '')
        harness = r'''
#include <stdint.h>
#include <assert.h>
#define RING_HOT
#define RING_PAIRS 16384u
#define RING_MASK (RING_PAIRS-1)
#define SENTINEL 0xa5c33c5au
static uint32_t banks[2][RING_PAIRS+2];
static uint32_t *bank_ptr(unsigned b){return &banks[b][1];}
'''
        check = r'''
int main(void){
 const unsigned starts[]={0,1,2,3,4,7,8,9,123,1023,1024,16375,16380,16381,16382,16383,16384,32767};
 const unsigned counts[]={0,1,2,3,4,5,7,8,9,15,16,17,63,64,65,1024,2048,16384};
 for(unsigned b=0;b<2;b++)
 for(unsigned a=0;a<sizeof(starts)/sizeof(*starts);a++)
 for(unsigned c=0;c<sizeof(counts)/sizeof(*counts);c++){
  for(unsigned k=0;k<2;k++)for(unsigned j=0;j<RING_PAIRS+2;j++)banks[k][j]=0x12345678u;
  fill_sentinels(b,starts[a],counts[c]);
  for(unsigned k=0;k<2;k++){
   assert(banks[k][0]==0x12345678u&&banks[k][RING_PAIRS+1]==0x12345678u);
   for(unsigned j=0;j<RING_PAIRS;j++){
    unsigned distance=(j-starts[a])&RING_MASK;
    uint32_t expected=k==b&&distance<counts[c]?SENTINEL:0x12345678u;
    assert(bank_ptr(k)[j]==expected);
   }
  }
 }
 return 0;
}
'''
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / 'sentinels.c'
            path.write_text(harness + source + check)
            exe = Path(tmp) / 'sentinels'
            subprocess.run(['cc', '-std=gnu11', '-O2', '-fsanitize=undefined',
                            '-fno-sanitize-recover=all', str(path), '-o', str(exe)], check=True)
            subprocess.run([str(exe)], check=True, timeout=10)
