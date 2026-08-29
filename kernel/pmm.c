/* pmm.c -- см. include/pmm.h за общим описанием модели. */

#include "pmm.h"
#include "bootinfo.h"
#include "loader.h"
#include "memlayout.h"
#include "string.h"
#include "kprintf.h"
#include "spinlock.h"

/* SMP: битовая карта кадров (g_bitmap), счётчики ссылок (g_refcount) и
 * g_scan_hint -- общие структуры данных на всю машину, а не на одно
 * ядро (физическая память едина независимо от того, сколько
 * процессоров её используют). До SMP (см. include/smp.h) единственным
 * пользователем PMM было единственное ядро, так что гонок не
 * возникало в принципе. Теперь два ядра могут одновременно вызвать
 * pmm_alloc_frame()/pmm_free_frame() (например, оба одновременно
 * растят кучу ядра, kernel/kheap.c, или оба обрабатывают #PF в разных
 * процессах) -- без лока это классическая гонка на битовой карте:
 * оба могут найти один и тот же "свободный" кадр и оба пометить его
   занятым, думая, что получили эксклюзивный доступ. */
static spinlock_t g_pmm_lock = SPINLOCK_INIT;

/* Отслеживаем кадры в пределах нижних 4 GiB -- ровно того диапазона,
   который identity-map'ит boot.s. 4 GiB / 4 KiB = 1 048 576 кадров,
   т.е. 131 072 байта (128 KiB) под битовую карту -- это статический
   массив в .bss, физически он не занимает места в самом ISO/бинарнике
   ядра, только в оперативной памяти после загрузки. */
#define PMM_MAX_FRAMES (0x100000000ull / PMM_FRAME_SIZE)

static u8 g_bitmap[PMM_MAX_FRAMES / 8];

/* Счётчик ссылок на кадр, для copy-on-write (см. include/pmm.h). Кадр
   без ссылок либо свободен, либо ещё никогда не выделялся -- в обоих
   случаях 0 здесь ничего не значит сам по себе, единственный источник
   истины "свободен ли кадр" -- по-прежнему g_bitmap. Один байт на кадр
   (максимум 255 совладельцев) с большим запасом хватает для честного
   copy-on-write в учебной ОС: даже десяток вложенных fork() без единой
   записи в общие страницы -- уже экзотика. */
static u8 g_refcount[PMM_MAX_FRAMES];

static u32 g_total_frames = 0;
static u32 g_free_frames  = 0;
static u32 g_scan_hint    = 0; /* индекс, с которого начинать поиск свободного кадра */

extern u8 kernel_end; /* см. boot/linker.ld */

static inline u32 frame_index(const void *addr) {
    return (u32)(((u64)(uintptr_t)addr) / PMM_FRAME_SIZE);
}

static inline bool_t bit_test(u32 idx) {
    return (g_bitmap[idx / 8] >> (idx % 8)) & 1u;
}

static inline void bit_set(u32 idx) {
    g_bitmap[idx / 8] |= (u8)(1u << (idx % 8));
}

static inline void bit_clear(u32 idx) {
    g_bitmap[idx / 8] &= (u8)~(1u << (idx % 8));
}

/* Помечает один кадр занятым, если он ещё не был занят, и обновляет
   счётчик свободных кадров. Кадры за пределами g_total_frames (т.е.
   физически несуществующая память) уже считаются занятыми при
   инициализации и повторно не учитываются. */
static void mark_used(u32 idx) {
    if (idx >= PMM_MAX_FRAMES) return;
    if (!bit_test(idx)) {
        bit_set(idx);
        if (g_free_frames > 0) g_free_frames--;
    }
}

static void mark_free(u32 idx) {
    if (idx >= g_total_frames) return; /* нельзя освободить несуществующую память */
    if (bit_test(idx)) {
        bit_clear(idx);
        g_free_frames++;
    }
}

void pmm_reserve_region(void *addr, u32 len) {
    if (len == 0) return;
    u32 start = frame_index(addr);
    u32 end   = frame_index((u8 *)addr + len + PMM_FRAME_SIZE - 1); /* округление вверх */
    if (end > PMM_MAX_FRAMES) end = PMM_MAX_FRAMES;
    for (u32 i = start; i < end; i++) mark_used(i);
}

void pmm_init(const boot_info_t *bi) {
    /* По умолчанию всё считаем занятым/несуществующим, затем открываем
       ровно тот диапазон, который реально есть в системе. Так кадры
       выше обнаруженного объёма RAM (и, в частности, весь диапазон
       вплоть до 4 GiB, где может быть MMIO/PCI BAR устройств вроде
       AHCI) никогда не будут выданы аллокатором. */
    memset(g_bitmap, 0xFF, sizeof(g_bitmap));

    u64 total_bytes = bootinfo_usable_ram_extent(bi);
    if (total_bytes == 0) {
        /* Загрузчик не предоставил информацию о памяти -- берём
           консервативный запасной вариант, чтобы система хотя бы
           не пыталась выделять память там, где её точно нет. */
        total_bytes = 32ull * 1024ull * 1024ull;
        kprintf("pmm: boot info has no usable memory regions, assuming %u MB RAM.\n",
                (u32)(total_bytes / (1024ull * 1024ull)));
    }

    u64 total_frames64 = total_bytes / PMM_FRAME_SIZE;
    if (total_frames64 > PMM_MAX_FRAMES) total_frames64 = PMM_MAX_FRAMES;
    g_total_frames = (u32)total_frames64;

    /* Открываем обнаруженный диапазон -- помечаем как свободный. */
    for (u32 i = 0; i < g_total_frames; i++) bit_clear(i);
    g_free_frames = g_total_frames;

    /* Резервируем известные занятые области:
       1) первый 1 MiB -- реальный режим, BIOS data area, VGA text
          buffer (0xB8000), таблицы страниц и стек ядра из boot.s
          лежат уже выше, но этот регион всё равно нельзя трогать;
       2) сам образ ядра, от 1 MiB (см. boot/linker.ld: ". = 1M") до
          kernel_end -- включает .text/.rodata/.data/.bss, в том числе
          таблицы страниц, стек ядра и саму эту битовую карту;
       3) буфер загрузчика пользовательских бинарников -- фиксированный
          адрес APP_LOAD_ADDR, куда loader.c копирует запускаемый файл
          (см. include/loader.h);
       4) отдельный стек для ring3-приложений -- USER_STACK_ADDR, сразу
          за APP_LOAD_ADDR (см. include/usermode.h): обычной куче
          (kheap.c) нельзя выдавать отсюда память, иначе снятие/установка
          бита USER для этой 2 MiB страницы (kernel/paging.c) на время
          запуска sandboxed-приложения задевала бы и чужие данные. */
    pmm_reserve_region((void *)0x0, 0x100000u);
    pmm_reserve_region((void *)0x100000u,
                        (u32)((uintptr_t)&kernel_end - 0x100000u));
    pmm_reserve_region((void *)APP_LOAD_ADDR, APP_MAX_SIZE);
    /* Виртуальное окно процесса (код -- он же APP_LOAD_ADDR/APP_MAX_SIZE
       выше, стек, куча -- см. include/memlayout.h) целиком зарезервировано
       здесь как ФИЗИЧЕСКИЙ диапазон, хотя сами страницы кода/стека/кучи
       процесса на самом деле берут кадры из обычного общего пула (см.
       kernel/paging.c: as_alloc_and_map()) и НИКОГДА не используют кадры
       именно из этого диапазона -- он тут лишь потому, что у каждого
       процесса приватный PD0 "дырявит" identity map ровно в этих пяти
       2 MiB слотах (PROC_FIRST_PD_SLOT..PROC_LAST_PD_SLOT), и если бы
       обычная куча ядра (kheap.c) когда-нибудь получила отсюда кадр под
       свои нужды, эта память стала бы невидимой ядру всякий раз, когда в
       CR3 оказывается чьё-то (любое) адресное пространство процесса. */
    pmm_reserve_region((void *)PROC_STACK_BASE, PROC_STACK_SIZE);
    pmm_reserve_region((void *)PROC_HEAP_BASE, PROC_HEAP_SIZE);
    /* 5) окно разделяемой памяти IPC (см. include/ipc.h, PROC_SHM_BASE в
       include/memlayout.h) -- та же логика, что и для стека/кучи выше:
       кадры самих shm-сегментов приходят из обычного общего пула
       (pmm_alloc_frame(), см. kernel/ipc.c: shm_open_or_create()), просто
       отображаются на этот виртуальный диапазон приватной PT процесса --
       сам физический диапазон должен остаться недостижим для kheap.c. */
    pmm_reserve_region((void *)PROC_SHM_BASE, PROC_SHM_SIZE);

    memset(g_refcount, 0, sizeof(g_refcount));

    g_scan_hint = 0;
}

static s32 find_free_run(u32 count) {
    if (count == 0 || count > g_total_frames) return -1;
    u32 run_start = 0;
    u32 run_len = 0;
    for (u32 i = g_scan_hint; i < g_total_frames + g_scan_hint; i++) {
        u32 idx = i % g_total_frames;
        if (!bit_test(idx)) {
            if (run_len == 0) run_start = idx;
            run_len++;
            if (run_len == count) return (s32)run_start;
        } else {
            run_len = 0;
        }
        /* На стыке "конец массива -> начало" подряд идущий участок
           разрывается, даже если физически кадры не идут подряд по
           этому индексу -- это нормально для линейного скана. */
        if (idx == g_total_frames - 1) run_len = 0;
    }
    return -1;
}

void *pmm_alloc_frames(u32 count) {
    u64 flags = spin_lock_irqsave(&g_pmm_lock);

    s32 start = find_free_run(count);
    if (start < 0) {
        spin_unlock_irqrestore(&g_pmm_lock, flags);
        return 0;
    }
    for (u32 i = 0; i < count; i++) {
        mark_used((u32)start + i);
        g_refcount[(u32)start + i] = 1;
    }
    g_scan_hint = (u32)start + count;
    if (g_scan_hint >= g_total_frames) g_scan_hint = 0;

    spin_unlock_irqrestore(&g_pmm_lock, flags);
    return (void *)(uintptr_t)((u64)(u32)start * PMM_FRAME_SIZE);
}

void *pmm_alloc_frame(void) {
    return pmm_alloc_frames(1);
}

void pmm_free_frames(void *addr, u32 count) {
    u64 flags = spin_lock_irqsave(&g_pmm_lock);
    u32 start = frame_index(addr);
    for (u32 i = 0; i < count; i++) {
        g_refcount[start + i] = 0;
        mark_free(start + i);
    }
    spin_unlock_irqrestore(&g_pmm_lock, flags);
}

void pmm_free_frame(void *addr) {
    pmm_free_frames(addr, 1);
}

void pmm_frame_addref(void *addr) {
    u64 flags = spin_lock_irqsave(&g_pmm_lock);
    u32 idx = frame_index(addr);
    if (idx < g_total_frames && g_refcount[idx] < 0xFF) g_refcount[idx]++;
    spin_unlock_irqrestore(&g_pmm_lock, flags);
}

void pmm_frame_release(void *addr) {
    u64 flags = spin_lock_irqsave(&g_pmm_lock);
    u32 idx = frame_index(addr);
    if (idx >= g_total_frames) {
        spin_unlock_irqrestore(&g_pmm_lock, flags);
        return;
    }
    if (g_refcount[idx] > 1) {
        g_refcount[idx]--;
        spin_unlock_irqrestore(&g_pmm_lock, flags);
        return;
    }
    /* Последняя (или единственная -- обычный не-COW кадр) ссылка:
       по-настоящему освобождаем кадр аллокатору. */
    g_refcount[idx] = 0;
    mark_free(idx);
    spin_unlock_irqrestore(&g_pmm_lock, flags);
}

u32 pmm_total_frames(void)      { return g_total_frames; }
u32 pmm_free_frame_count(void)  { return g_free_frames; }
u32 pmm_used_frame_count(void)  { return g_total_frames - g_free_frames; }
u32 pmm_total_memory_bytes(void) { return g_total_frames * PMM_FRAME_SIZE; }
u32 pmm_free_memory_bytes(void)  { return g_free_frames * PMM_FRAME_SIZE; }
