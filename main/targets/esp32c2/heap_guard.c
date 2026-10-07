/* The RF dump shares the last 128 KiB SRAM bank. Keep all dynamic
 * allocations (including DMA buffers and task stacks) below that bank.
 * Filter IDF's already-trimmed regions, so the ROM reservation can vary
 * between chip revisions without overlapping a static reservation. */
#include "heap_memory_layout.h"
extern size_t __real_soc_get_available_memory_regions(soc_memory_region_t *regions);
size_t __wrap_soc_get_available_memory_regions(soc_memory_region_t *regions) {
    size_t count=__real_soc_get_available_memory_regions(regions), kept=0;
    for(size_t i=0;i<count;i++) {
        soc_memory_region_t r=regions[i];
        if(r.start>=0x3fcc0000)continue;
        if(r.size>(size_t)(0x3fcc0000-r.start))r.size=0x3fcc0000-r.start;
        if(r.size)regions[kept++]=r;
    }
    return kept;
}
