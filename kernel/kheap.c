/* kheap.c -- см. include/kheap.h за общим описанием модели. */

#include "kheap.h"
#include "pmm.h"
#include "string.h"
#include "kprintf.h"
#include "spinlock.h"

/* SMP: список блоков кучи (g_first_block и вся цепочка next/prev) --
 * общая структура на всю машину, а не на одно ядро (одна куча ядра на
 * всю систему, как и раньше). До SMP (см. include/smp.h) вызвать
 * kmalloc()/kfree() могло только единственное существовавшее ядро;
 * теперь два ядра могут одновременно дёрнуть kmalloc() (например, оба
 * создают task_t в task_create(), см. kernel/task.c) -- без лока это
 * классическая гонка на связном списке (оба могут одновременно
 * разрезать/слить один и тот же блок, повредив g_first_block). */
static spinlock_t g_heap_lock = SPINLOCK_INIT;

/* Куча ядра живёт по фиксированному адресу, начиная с 44 MiB -- сразу
   ПОСЛЕ окна процесса (PROC_CODE_BASE..PROC_SHM_BASE+PROC_SHM_SIZE =
   [0x02000000, 0x02C00000) = [32 MiB, 44 MiB), см. include/memlayout.h
   и комментарий в kernel/pmm.c про PROC_FIRST_PD_SLOT..PROC_LAST_PD_SLOT).
   ВАЖНО: раньше здесь стояло 0x00900000 (9 MiB) -- это заходило ВНУТРЬ
   окна процесса (9..18 MiB из тогдашних 6..18 MiB), а не после него. Формально
   pmm_reserve_region() в pmm_init() резервирует под окно процесса ровно
   тот же физический диапазон, так что кадры не выдавались бы дважды --
   но сама куча ядра как ВИРТУАЛЬНЫЙ диапазон адресов всё равно
   попадала в те самые слоты PD0 (PROC_FIRST_PD_SLOT..PROC_LAST_PD_SLOT
   в include/memlayout.h), которые as_create() (kernel/paging.c)
   заменяет на приватные per-process таблицы страниц у КАЖДОГО
   запущенного sandboxed-процесса. В итоге любое обращение ядра к
   объекту, выделенному через kmalloc() в перекрывающейся части кучи
   (например, к task_t текущей задачи -- см. kernel/task.c), пока в CR3
   загружены таблицы страниц процесса, тут же давало #PF (адрес
   попадает в приватное окно, а не в общий identity map): обработчик
   страничного отказа (kernel/isr.c: page_fault_handler()) сам первым
   делом читает task_current()->as -- т.е. тоже обращается к куче -- и
   тоже падал в #PF на той же странице, что превращало обычный #PF в
   #DF, а затем и в тройной отказ (перезагрузку всей машины) при
   ЛЮБОМ запуске sandboxed-приложения (обычный 'run', ring 3), а не
   только у каких-то конкретных "тяжёлых" программ.

   Фаза 4.4 (см. CONTAINER_ARCHITECTURE.md) наступила на ОЧЕНЬ похожие
   грабли ещё раз, но уже не с кучей ядра, а со СТЕКОМ BSP (boot.s:
   stack_bottom/stack_top): окно процесса раньше начиналось с 6 MiB
   (PROC_CODE_BASE = 0x00600000), а .bss ядра к этому моменту разрослось
   (g_bitmap, g_refcount, file_data и т.д.) настолько, что kernel_end
   оказывался БОЛЬШЕ 6 MiB -- то есть стек BSP (несколькими сотнями KiB
   ниже kernel_end) тоже попадал ВНУТРЬ окна процесса. Symptom был другой
   (не #PF/#DF/тройной отказ, а тихий переход по случайному адресу внутри
   task_t сразу после context_switch() -- см. подробный разбор в
   include/memlayout.h) именно потому, что это была не куча (её
   отдельные kmalloc()-блоки), а САМ СТЕК, на котором в этот момент
   продолжал исполняться код ядра -- но причина и способ найти её были
   ровно те же: окно процесса выросло/сдвинулось не поспевая за ростом
   .bss ядра. Решение то же самое, только для ОБОИХ регионов сразу --
   поднять окно процесса (include/memlayout.h) значительно выше, с
   большим запасом на будущий рост, а не только-только за текущий
   kernel_end. */
#define HEAP_START_ADDR 0x02C00000ull
#define HEAP_MAX_SIZE   (16u * 1024u * 1024u) /* верхняя граница -- 16 MiB */
#define HEAP_MIN_SIZE   (64u * 1024u)         /* минимум, если памяти совсем мало */
#define MIN_SPLIT_REMAINDER 32u /* не делим блок, если остаток был бы меньше этого */

typedef struct block_header {
    u32 size;       /* размер полезной области после заголовка, в байтах --
                        начиная с этой правки ВКЛЮЧАЕТ канарейку (см. ниже),
                        то есть больше не равен тому, что реально можно
                        отдавать вызывающему -- см. req_size. */
    u32 req_size;   /* ВРЕМЕННО (см. общий комментарий в kernel/kheap.c про
                        охоту за повреждением кучи): то, что реально
                        запросил вызывающий kmalloc(size) -- ДО выравнивания
                        по 8 и ДО добавления канарейки. Канарейка пишется
                        сразу после этих req_size байт. */
    u32 free;
    u32 _pad;       /* выравнивание next/prev на 8 байт -- не обязательно,
                        но чище, чем полагаться на то, что компилятор сам
                        досчитает padding одинаково везде, где решится
                        занести sizeof(block_header_t) в арифметику указателей. */
    struct block_header *next;
    struct block_header *prev;
} block_header_t;

#define HDR_SIZE ((u32)sizeof(block_header_t))

/* ---- Канарейка после каждого выделения (диагностический инструмент) ----
 * Появилась при охоте за багом Фазы 4.4 (см. CONTAINER_ARCHITECTURE.md):
 * свежесозданный task_t "падал" сразу после первого же context_switch()
 * на него -- выглядело как повреждение кучи (адрес возврата внутри
 * task_t), но канарейка ниже, будучи расставлена по всем подозрительным
 * точкам (kernel/task.c, kernel/process.c -- см. git-историю), НИ РАЗУ
 * не сработала. Это и оказалось ключевой уликой: раз данные целы, а
 * переход всё равно идёт не туда -- дело не в куче вообще, а в том, что
 * стек BSP (boot.s) физически пересекался с приватным окном процесса
 * (см. подробный разбор у HEAP_START_ADDR ниже и в include/memlayout.h) --
 * баг найден и исправлен переносом окна процесса, а канарейка оставлена
 * здесь на будущее: недорогая (16 байт на выделение, проверка только на
 * kfree() сама по себе) и уже доказавшая свою пользу при отладке.
 *
 * После РЕАЛЬНО запрошенных вызывающим req_size байт (а не после
 * округлённого до 8 размера -- округление само по себе не должно ничего
 * трогать, но лучше не полагаться и на это) дописывается
 * KHEAP_CANARY_SIZE байт известного паттерна. kfree() проверяет свою
 * канарейку автоматически; kheap_check_all(tag) -- ручной инструмент:
 * проходит по ВСЕМ занятым блокам сразу (не дожидаясь их kfree()) и
 * печатает через kprintf() адрес и tag первого блока с испорченной
 * канарейкой -- полезно расставить по паре подозрительных мест кода и
 * сузить обхватом окно, где могло произойти повреждение, как это и было
 * сделано в Фазе 4.4. */
#define KHEAP_CANARY_SIZE 16u
static const u8 KHEAP_CANARY_BYTE = 0xC5;

static inline u8 *canary_ptr(block_header_t *b) {
    return (u8 *)b + HDR_SIZE + b->req_size;
}

static void write_canary(block_header_t *b) {
    memset(canary_ptr(b), KHEAP_CANARY_BYTE, KHEAP_CANARY_SIZE);
}

/* TRUE, если канарейка блока b цела. */
static bool_t canary_ok(block_header_t *b) {
    u8 *c = canary_ptr(b);
    for (u32 i = 0; i < KHEAP_CANARY_SIZE; i++) {
        if (c[i] != KHEAP_CANARY_BYTE) return FALSE;
    }
    return TRUE;
}

static block_header_t *g_first_block = 0;
static u32 g_heap_total_bytes = 0; /* сумма size всех блоков (без заголовков) на момент инициализации */
static u32 g_heap_used_bytes  = 0; /* сумма size блоков, которые сейчас не свободны */

static inline u32 align_up(u32 v, u32 a) {
    return (v + (a - 1)) & ~(a - 1);
}

void kheap_init(void) {
    u32 total_ram = pmm_total_memory_bytes();
    u32 heap_size = HEAP_MAX_SIZE;

    /* Не отдаём под кучу ядра больше половины обнаруженной памяти --
       на маленьких конфигурациях (например, -m 32M) это защищает от
       ситуации, когда куче некуда расти, а остальной системе не
       остаётся физических кадров вообще. */
    if (heap_size > total_ram / 2) heap_size = total_ram / 2;
    if (heap_size < HEAP_MIN_SIZE) heap_size = HEAP_MIN_SIZE;

    pmm_reserve_region((void *)(uintptr_t)HEAP_START_ADDR, heap_size);

    g_first_block = (block_header_t *)(uintptr_t)HEAP_START_ADDR;
    g_first_block->size = heap_size - HDR_SIZE;
    g_first_block->free = TRUE;
    g_first_block->next = 0;
    g_first_block->prev = 0;

    g_heap_total_bytes = g_first_block->size;
    g_heap_used_bytes = 0;

    kprintf("kheap: %u KB reserved for kernel heap at 0x%x.\n",
            heap_size / 1024, (u32)HEAP_START_ADDR);
}

void *kmalloc(u32 size) {
    if (size == 0 || g_first_block == 0) return 0;
    u32 asize = align_up(size, 8) + KHEAP_CANARY_SIZE;

    u64 flags = spin_lock_irqsave(&g_heap_lock);

    for (block_header_t *b = g_first_block; b != 0; b = b->next) {
        if (!b->free || b->size < asize) continue;

        /* Если после выделения останется достаточно места на ещё один
           заголовок + разумный минимум полезных байт -- отрезаем хвост
           в отдельный свободный блок, а не отдаём его целиком. */
        if (b->size >= asize + HDR_SIZE + MIN_SPLIT_REMAINDER) {
            block_header_t *tail = (block_header_t *)((u8 *)b + HDR_SIZE + asize);
            tail->size = b->size - asize - HDR_SIZE;
            tail->free = TRUE;
            tail->next = b->next;
            tail->prev = b;
            if (b->next) b->next->prev = tail;
            b->next = tail;
            b->size = asize;
        }

        b->free = FALSE;
        b->req_size = size;
        write_canary(b);
        g_heap_used_bytes += b->size;
        spin_unlock_irqrestore(&g_heap_lock, flags);
        return (void *)((u8 *)b + HDR_SIZE);
    }

    spin_unlock_irqrestore(&g_heap_lock, flags);
    return 0; /* куча исчерпана или фрагментирована -- свободного блока нужного размера нет */
}

void *kcalloc(u32 nmemb, u32 size) {
    u32 total = nmemb * size;
    void *p = kmalloc(total);
    if (p) memset(p, 0, total);
    return p;
}

static void try_merge_with_next(block_header_t *b) {
    if (b->next && b->next->free) {
        b->size += HDR_SIZE + b->next->size;
        b->next = b->next->next;
        if (b->next) b->next->prev = b;
    }
}

void kfree(void *ptr) {
    if (!ptr || g_first_block == 0) return;
    block_header_t *b = (block_header_t *)((u8 *)ptr - HDR_SIZE);

    u64 flags = spin_lock_irqsave(&g_heap_lock);

    /* Простая защита от двойного free / мусорного указателя не из этой
       кучи: если блок уже помечен свободным, просто игнорируем. */
    if (b->free) {
        spin_unlock_irqrestore(&g_heap_lock, flags);
        return;
    }

    /* ВРЕМЕННО (см. общий комментарий у KHEAP_CANARY_SIZE выше): ловит
       переполнение хотя бы для блоков, которые вообще доживают до
       kfree() -- в дополнение к kheap_check_all(), которая проверяет
       ЖИВЫЕ блоки, не дожидаясь их освобождения. */
    if (!canary_ok(b)) {
        kprintf("kheap: CANARY CORRUPTED at kfree(%x), req_size=%u, block=%x\n",
                (u32)(uintptr_t)ptr, b->req_size, (u32)(uintptr_t)b);
    }

    b->free = TRUE;
    g_heap_used_bytes -= b->size;

    try_merge_with_next(b);
    if (b->prev && b->prev->free) {
        try_merge_with_next(b->prev);
    }

    spin_unlock_irqrestore(&g_heap_lock, flags);
}

void *krealloc(void *ptr, u32 new_size) {
    if (!ptr) return kmalloc(new_size);
    if (new_size == 0) { kfree(ptr); return 0; }

    block_header_t *b = (block_header_t *)((u8 *)ptr - HDR_SIZE);
    if (b->req_size >= new_size) return ptr; /* уже достаточно места, не ужимаем агрессивно */

    void *newp = kmalloc(new_size);
    if (!newp) return 0;
    memcpy(newp, ptr, b->req_size);
    kfree(ptr);
    return newp;
}

u32 kheap_total_bytes(void) { return g_heap_total_bytes; }
u32 kheap_used_bytes(void)  { return g_heap_used_bytes; }
u32 kheap_free_bytes(void)  { return g_heap_total_bytes - g_heap_used_bytes; }

bool_t kheap_check_all(const char *tag) {
    bool_t ok = TRUE;
    u64 flags = spin_lock_irqsave(&g_heap_lock);
    for (block_header_t *b = g_first_block; b != 0; b = b->next) {
        if (b->free) continue;
        if (!canary_ok(b)) {
            kprintf("kheap: CANARY CORRUPTED, tag=%s block=%x payload=%x req_size=%u\n",
                    tag, (u32)(uintptr_t)b, (u32)(uintptr_t)((u8 *)b + HDR_SIZE), b->req_size);
            u8 *c = canary_ptr(b);
            kprintf("kheap:   canary bytes: %x %x %x %x %x %x %x %x\n",
                    c[0], c[1], c[2], c[3], c[4], c[5], c[6], c[7]);
            ok = FALSE;
        }
    }
    spin_unlock_irqrestore(&g_heap_lock, flags);
    return ok;
}
