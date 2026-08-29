/* bootinfo.c -- реализация bootinfo.h. Ничего не знает про Multiboot,
 * FDT или что-либо ещё специфичное для конкретного загрузчика -- см.
 * общий комментарий в include/bootinfo.h. */

#include "bootinfo.h"

void bootinfo_add_region(boot_info_t *bi, u64 base, u64 length, u32 type) {
    if (bi->region_count >= BOOTINFO_MAX_MEM_REGIONS) return;
    boot_mem_region_t *r = &bi->regions[bi->region_count];
    r->base = base;
    r->length = length;
    r->type = type;
    bi->region_count++;
}

u64 bootinfo_usable_ram_extent(const boot_info_t *bi) {
    u64 max_extent = 0;
    for (u32 i = 0; i < bi->region_count; i++) {
        if (bi->regions[i].type != BOOTINFO_MEM_USABLE) continue;
        u64 end = bi->regions[i].base + bi->regions[i].length;
        if (end > max_extent) max_extent = end;
    }
    return max_extent;
}
