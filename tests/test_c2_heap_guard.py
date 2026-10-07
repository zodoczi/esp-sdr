"""Keep task stacks and DMA allocations outside the C2 capture SRAM bank."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


class C2HeapGuard(unittest.TestCase):
    @unittest.skipUnless(shutil.which('cc'), 'Host C compiler unavailable')
    def test_trims_regions_without_assuming_a_rom_reservation_boundary(self):
        root = Path(__file__).resolve().parents[1]
        with tempfile.TemporaryDirectory() as tmp:
            tmp = Path(tmp)
            (tmp/'heap_memory_layout.h').write_text('''
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
typedef struct { intptr_t start; size_t size,type; intptr_t iram_address; bool startup_stack; } soc_memory_region_t;
''')
            (tmp/'test.c').write_text('''
#include "heap_memory_layout.h"
#include <assert.h>
#include <string.h>
static soc_memory_region_t input[4];
size_t __real_soc_get_available_memory_regions(soc_memory_region_t *r) {
    memcpy(r,input,sizeof(input));return 4;
}
extern size_t __wrap_soc_get_available_memory_regions(soc_memory_region_t *r);
int main(void) {
    for(unsigned end=0x3fcdf000;end<=0x3fce0000;end+=4) {
        input[0]=(soc_memory_region_t){0x3fca1234,0x1234,2,0x40381234,false};
        input[1]=(soc_memory_region_t){0x3fcb8000,0x10000,3,0x40398000,false};
        input[2]=(soc_memory_region_t){0x3fcc8000,end-0x3fcc8000,4,0x403a8000,false};
        input[3]=(soc_memory_region_t){end,0x3fce0000-end,5,0,true};
        soc_memory_region_t r[4];
        assert(__wrap_soc_get_available_memory_regions(r)==2);
        assert(!memcmp(&r[0],&input[0],sizeof(r[0])));
        assert(r[1].start==0x3fcb8000 && r[1].size==0x8000);
        assert(r[1].type==3 && r[1].iram_address==0x40398000 && !r[1].startup_stack);
    }
}
''')
            exe=tmp/'test'
            subprocess.run(['cc','-std=c11','-I'+str(tmp),str(tmp/'test.c'),str(root/'main/targets/esp32c2/heap_guard.c'),'-o',str(exe)],check=True)
            subprocess.run([str(exe)],check=True)
