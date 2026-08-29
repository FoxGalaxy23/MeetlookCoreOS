/* percpu.c -- см. include/percpu.h за общим описанием модели. */

#include "percpu.h"
#include "string.h"

#define IA32_GS_BASE 0xC0000101u

static percpu_t g_percpu[SMP_MAX_CPUS];
static volatile u32 g_online_count = 0;

static inline void wrmsr(u32 msr, u64 value) {
    u32 lo = (u32)(value & 0xFFFFFFFFu);
    u32 hi = (u32)(value >> 32);
    __asm__ volatile ("wrmsr" : : "c"(msr), "a"(lo), "d"(hi) : "memory");
}

void percpu_init_early(u32 cpu_index, u32 lapic_id) {
    percpu_t *p = &g_percpu[cpu_index];
    memset(p, 0, sizeof(*p));
    p->self = p;
    p->cpu_index = cpu_index;
    p->lapic_id = lapic_id;
    p->online = TRUE;
    p->current_cr3 = 0; /* 0 -- заведомо невалидный физический CR3 (PML4 не бывает по адресу 0,
                            он всегда внутри зарезервированного PMM региона [0,1MiB), см. pmm.c) --
                            поэтому первый же paging_switch() на этом ядре гарантированно
                            перезагрузит CR3, а не решит, что он "уже тот, что нужен". */

    wrmsr(IA32_GS_BASE, (u64)(uintptr_t)p);

    /* g_online_count растёт монотонно и только этим самым ядром про
       самого себя -- инкремент не нуждается в отдельном локе, т.к. два
       разных ядра никогда не инкрементируют его "за" одно и то же
       cpu_index одновременно (bring-up в kernel/smp.c строго
       последовательный, см. smp_init()), но __atomic всё равно
       используем, чтобы избежать гонки с percpu_online_count(),
       читающим это же значение с других ядер безо всякого лока. */
    __atomic_fetch_add(&g_online_count, 1u, __ATOMIC_SEQ_CST);
}

u32 percpu_online_count(void) {
    return __atomic_load_n(&g_online_count, __ATOMIC_SEQ_CST);
}

percpu_t *percpu_get(u32 index) {
    if (index >= SMP_MAX_CPUS) return 0;
    if (!g_percpu[index].online) return 0;
    return &g_percpu[index];
}
