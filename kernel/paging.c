/* paging.c -- см. include/paging.h за общей идеей.
 *
 * Раскладка одного адресного пространства:
 *
 *   PML4 (1 кадр) -- используется только PML4[0] (весь identity map
 *                     нижних 4 GiB лежит в первых 512 GiB виртуального
 *                     пространства, т.е. в одной записи PML4).
 *     -> PDPT (1 кадр, приватный)
 *          [0] -> PD0 (1 кадр, приватный) -- нижний 1 GiB
 *          [1..3] -> ОБЩИЕ PD ядра (те же самые, что и в ядерном
 *                    шаблоне -- 2..4 GiB, включая "PCI hole" на 3 GiB,
 *                    см. boot/boot.s) -- никогда не создаются заново и
 *                    не освобождаются per-process, т.к. не приватны.
 *
 *   PD0 -- 512 записей по 2 MiB. Все, КРОМЕ индексов
 *   PROC_FIRST_PD_SLOT..PROC_LAST_PD_SLOT (include/memlayout.h),
 *   скопированы по значению из ядерного шаблона (обычные huge pages,
 *   PS=1, как их построил boot.s) -- то есть ядро/низкая память видны
 *   без изменений при любом CR3. Индексы 3..7 (код/стек/куча процесса)
 *   в свежесозданном адресном пространстве -- 0 (not present); при
 *   первом реальном отображении в этом слоте лениво создаётся приватная
 *   PT (обычные 4 KiB PTE, не PS).
 *
 * Кадры данных (собственно код/стек/куча процесса) НЕ хранятся в этом
 * "дырявом" физическом диапазоне -- они самые обычные кадры из общего
 * пула pmm_alloc_frame(), просто отображённые через приватную PT на
 * фиксированный виртуальный адрес процесса (PROC_CODE_BASE и т.д.).
 * Сам физический диапазон [PROC_CODE_BASE, PROC_HEAP_BASE+PROC_HEAP_SIZE)
 * зарезервирован в PMM и не используется вообще ничем (см. kernel/pmm.c)
 * -- иначе кадр ядерных данных мог бы случайно оказаться там, куда
 * per-process PD0 "дырявит" отображение, и стать невидимым, когда в CR3
 * окажется чьё-то чужое адресное пространство. */

#include "paging.h"
#include "pmm.h"
#include "kheap.h"
#include "kprintf.h"
#include "string.h"
#include "memlayout.h"
#include "percpu.h"

#define PAGE_SIZE       4096ull
#define SLOT_SIZE       0x200000ull   /* 2 MiB, размер одного PDE-слота */
#define PT_ENTRIES      512u

struct address_space {
    u64 pml4_phys;
    u64 pdpt_phys;
    u64 pd0_phys;
};

static u64 g_kernel_pml4 = 0;   /* PML4, построенный boot.s -- "ядерное" CR3 */
static u64 g_kernel_pdpt = 0;   /* его единственный PDPT (PML4[0]) */
static u64 g_kernel_pd0  = 0;   /* его PD0 (PDPT[0]) -- шаблон для as_create() */

/* TRUE, если этот CPU поддерживает NX (CPUID) И мы успешно включили
   EFER.NXE (см. enable_nx_if_supported() ниже) -- единственное условие,
   при котором где-либо в этом файле разрешено выставлять PTE_NX. Пока
   FALSE, весь W^X-код молча деградирует к старому поведению (страницы
   исполняемы всегда, как до этой правки) -- НЕ к отказу загрузки: на
   гипотетическом CPU без NX аппаратный W^X просто недостижим, отказывать
   в загрузке системы из-за этого было бы явно хуже, чем просто не
   получить эту конкретную защиту. */
static bool_t g_nx_supported = FALSE;

/* CPUID.80000001H:EDX[20] -- поддержка NX/XD (AMD называет его NX, Intel
   -- XD, бит один и тот же). Сначала проверяем через CPUID.80000000H,
   что расширенный лист 0x80000001 вообще существует -- на CPU настолько
   старом, что не существует, читать 0x80000001 напрямую можно было бы
   получить неопределённый (не обязательно нулевой) результат. Практически
   для любого CPU, способного вообще войти в long mode (что уже сделал
   boot.s к моменту вызова этой функции), лист 0x80000001 присутствует
   всегда -- проверка здесь скорее защита от экзотики, чем реальный
   сценарий. */
static bool_t cpu_supports_nx(void) {
    u32 eax, ebx, ecx, edx;

    __asm__ volatile ("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx) : "a"(0x80000000u) : );
    (void)ebx; (void)ecx; (void)edx;
    if (eax < 0x80000001u) return FALSE;

    __asm__ volatile ("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx) : "a"(0x80000001u) : );
    (void)eax; (void)ebx; (void)ecx;
    return (edx & (1u << 20)) != 0;
}

/* Включает EFER.NXE (MSR 0xC0000080, бит 11) -- без этого бит 63 (PTE_NX)
   во ВСЕХ структурах трансляции адресов -- reserved, и попытка его
   выставить привела бы не к ожидаемому "страница не исполняется", а к
   #PF с флагом RSVD при первом же обращении к такой странице (Intel SDM
   Vol.3 §4.13.4) -- то есть система бы падала ровно на том самом
   бинарнике, который W^X должен защищать, а не атаковать. Именно поэтому
   весь остальной код paging.c выставляет PTE_NX ТОЛЬКО под условием
   g_nx_supported, которое этот вызов и устанавливает. Вызывается один
   раз из paging_init(), до первого as_create(). */
static void enable_nx_if_supported(void) {
    if (!cpu_supports_nx()) return;

    u32 lo, hi;
    __asm__ volatile ("rdmsr" : "=a"(lo), "=d"(hi) : "c"(0xC0000080u));
    lo |= (1u << 11); /* EFER.NXE */
    __asm__ volatile ("wrmsr" : : "c"(0xC0000080u), "a"(lo), "d"(hi) : "memory");

    g_nx_supported = TRUE;
}

static inline u64 read_cr3(void) {
    u64 v;
    __asm__ volatile ("mov %%cr3, %0" : "=r"(v));
    return v;
}

u64 paging_read_cr2(void) {
    u64 v;
    __asm__ volatile ("mov %%cr2, %0" : "=r"(v));
    return v;
}

static inline void invlpg(u64 addr) {
    __asm__ volatile ("invlpg (%0)" : : "r"(addr) : "memory");
}

/* Вся физическая память identity-mapped из ядра -- физический адрес
   таблицы одновременно является указателем, годным для чтения/записи
   ядром, НЕЗАВИСИМО от того, чьё CR3 сейчас загружено (см. комментарий
   в начале файла: общие для всех записи PDPT[1..3]/большая часть PD0
   гарантируют, что этот же самый физический диапазон виден всегда). */
static inline u64 *table_at(u64 phys) {
    return (u64 *)(uintptr_t)(phys & ~0xFFFull);
}

void paging_init(void) {
    enable_nx_if_supported();

    g_kernel_pml4 = read_cr3() & ~0xFFFull;
    g_kernel_pdpt = table_at(g_kernel_pml4)[0] & ~0xFFFull;
    g_kernel_pd0  = table_at(g_kernel_pdpt)[0] & ~0xFFFull;
    /* Кэш "уже загруженного CR3" для BSP (this_cpu() уже валиден к этому
       моменту -- percpu_init_early(0, ...) вызывается в kernel_main() ещё
       раньше, см. kernel/kernel.c) -- инициализируем сразу в g_kernel_pml4,
       раз BSP и так уже работает именно с этими таблицами (см.
       read_cr3() выше), чтобы paging_switch(0) не делал лишнюю запись в
       CR3 прямо на первом же вызове. */
    this_cpu()->current_cr3 = g_kernel_pml4;
}

/* Физический адрес PML4, построенного boot.s -- "ядерное" (общее для
   всех процессов и для всех ядер SMP) адресное пространство. Нужен
   AP-трамплину (см. boot/ap_trampoline.s, kernel/smp.c: smp_init()),
   чтобы каждый поднимаемый AP загрузил СЕБЕ в CR3 ровно эти же таблицы
   -- см. общий комментарий там же про то, почему отдельных таблиц на
   AP строить не нужно (identity map нижних 4 GiB общий на всю машину). */
u64 paging_kernel_cr3(void) {
    return g_kernel_pml4;
}

void paging_switch(address_space_t *as) {
    /* SMP: CR3 -- честно локальный регистр КАЖДОГО процессора (Intel SDM
       Vol.3 §4.3) -- кэш "что уже загружено" обязан быть per-CPU (см.
       this_cpu()->current_cr3, include/percpu.h), а не одной общей
       переменной на все ядра: иначе ядро B решило бы "CR3 уже тот, что
       нужен" на основании того, что когда-то его загрузило ядро A --
       у СЕБЯ, а не у ядра B, и B продолжило бы работать с ЧУЖИМИ
       таблицами страниц, ошибочно считая, что уже переключилось. */
    u64 target = as ? as->pml4_phys : g_kernel_pml4;
    percpu_t *cpu = this_cpu();
    if (target != cpu->current_cr3) {
        __asm__ volatile ("mov %0, %%cr3" : : "r"(target) : "memory");
        cpu->current_cr3 = target;
    }
}

/* Индекс PD0-слота (0..511), которому принадлежит виртуальный адрес --
   не проверяет, что это один из разрешённых процессу слотов, это дело
   вызывающего кода. */
static inline u32 pd_slot_of(u64 vaddr) {
    return (u32)((vaddr >> 21) & 0x1FF);
}

static bool_t slot_is_process_window(u32 slot) {
    return slot >= PROC_FIRST_PD_SLOT && slot <= PROC_LAST_PD_SLOT;
}

/* Возвращает указатель на приватную PT для данного PD0-слота адресного
   пространства as, создавая её при необходимости (create == TRUE) --
   4 KiB кадр, сразу обнулённый (все 512 PTE not-present). Возвращает 0,
   если слот не входит в диапазон процессных окон, либо (create==FALSE)
   если PT для него ещё не создана, либо (create==TRUE) при нехватке
   памяти. */
static u64 *get_or_create_pt(address_space_t *as, u32 slot, bool_t create) {
    if (!slot_is_process_window(slot)) return 0;

    u64 *pd0 = table_at(as->pd0_phys);
    if (pd0[slot] & PTE_PRESENT) {
        return table_at(pd0[slot]);
    }
    if (!create) return 0;

    void *pt = pmm_alloc_frame();
    if (!pt) return 0;
    memset(pt, 0, PAGE_SIZE);

    pd0[slot] = (u64)(uintptr_t)pt | PTE_PRESENT | PTE_WRITABLE | PTE_USER;
    return (u64 *)pt;
}

address_space_t *as_create(void) {
    address_space_t *as = (address_space_t *)kmalloc(sizeof(address_space_t));
    if (!as) return 0;

    void *pml4 = pmm_alloc_frame();
    void *pdpt = pmm_alloc_frame();
    void *pd0  = pmm_alloc_frame();
    if (!pml4 || !pdpt || !pd0) {
        if (pml4) pmm_free_frame(pml4);
        if (pdpt) pmm_free_frame(pdpt);
        if (pd0)  pmm_free_frame(pd0);
        kfree(as);
        return 0;
    }

    memset(pml4, 0, PAGE_SIZE);

    /* PD0: копия ядерного шаблона (все обычные 2 MiB отображения низкой
       памяти/образа ядра), кроме процессных слотов -- те лениво получат
       свою приватную PT при первом реальном отображении. */
    memcpy(pd0, table_at(g_kernel_pd0), PAGE_SIZE);
    u64 *pd0e = (u64 *)pd0;
    for (u32 slot = PROC_FIRST_PD_SLOT; slot <= PROC_LAST_PD_SLOT; slot++) {
        pd0e[slot] = 0;
    }

    /* PDPT: [0] -- наш приватный PD0, [1..3] -- общие PD ядра (2..4 GiB),
       см. boot/boot.s -- те же самые физические таблицы, что и у ядра. */
    u64 *kpdpt = table_at(g_kernel_pdpt);
    u64 *pdpte = (u64 *)pdpt;
    pdpte[0] = (u64)(uintptr_t)pd0 | PTE_PRESENT | PTE_WRITABLE | PTE_USER;
    pdpte[1] = kpdpt[1];
    pdpte[2] = kpdpt[2];
    pdpte[3] = kpdpt[3];
    for (u32 i = 4; i < PT_ENTRIES; i++) pdpte[i] = 0;

    ((u64 *)pml4)[0] = (u64)(uintptr_t)pdpt | PTE_PRESENT | PTE_WRITABLE | PTE_USER;

    as->pml4_phys = (u64)(uintptr_t)pml4;
    as->pdpt_phys = (u64)(uintptr_t)pdpt;
    as->pd0_phys  = (u64)(uintptr_t)pd0;
    return as;
}

void as_destroy(address_space_t *as) {
    if (!as) return;

    u64 *pd0 = table_at(as->pd0_phys);
    for (u32 slot = PROC_FIRST_PD_SLOT; slot <= PROC_LAST_PD_SLOT; slot++) {
        if (!(pd0[slot] & PTE_PRESENT)) continue;
        u64 *pt = table_at(pd0[slot]);
        for (u32 i = 0; i < PT_ENTRIES; i++) {
            if (pt[i] & PTE_PRESENT) {
                pmm_frame_release((void *)(uintptr_t)(pt[i] & PTE_ADDR_MASK));
            }
        }
        pmm_free_frame(pt); /* сама PT никогда не была общей -- своя у каждого as */
    }

    pmm_free_frame(table_at(as->pd0_phys));
    pmm_free_frame(table_at(as->pdpt_phys));
    pmm_free_frame(table_at(as->pml4_phys));
    kfree(as);
}

bool_t as_map_frame(address_space_t *as, u64 vaddr, u64 paddr, bool_t writable, bool_t executable, bool_t cow) {
    u32 slot = pd_slot_of(vaddr);
    u64 *pt = get_or_create_pt(as, slot, TRUE);
    if (!pt) return FALSE;

    u32 idx = (u32)((vaddr >> 12) & 0x1FF);
    u64 entry = (paddr & PTE_ADDR_MASK) | PTE_PRESENT | PTE_USER;
    if (writable) entry |= PTE_WRITABLE;
    if (cow)      entry |= PTE_COW;
    if (!executable && g_nx_supported) entry |= PTE_NX;
    pt[idx] = entry;

    invlpg(vaddr & ~0xFFFull); /* безопасно, даже если as сейчас не в CR3 -- просто лишний flush */
    return TRUE;
}

void *as_alloc_and_map(address_space_t *as, u64 vaddr, bool_t writable, bool_t executable) {
    u32 slot = pd_slot_of(vaddr);
    if (!slot_is_process_window(slot)) return 0;

    /* Проверяем, не отображена ли страница уже. Если да -- возвращаем
       её физический кадр (адрес ядра), не выделяя новый, чтобы
       избежать утечек памяти при перекрывающихся сегментах. */
    u64 *pt = get_or_create_pt(as, slot, FALSE);
    if (pt) {
        u32 idx = (u32)((vaddr >> 12) & 0x1FF);
        if (pt[idx] & PTE_PRESENT) {
            return table_at(pt[idx] & ~0xFFFull);
        }
    }

    void *frame = pmm_alloc_frame();
    if (!frame) return 0;
    memset(frame, 0, PAGE_SIZE);

    if (!as_map_frame(as, vaddr, (u64)(uintptr_t)frame, writable, executable, FALSE)) {
        pmm_free_frame(frame);
        return 0;
    }
    return frame;
}

address_space_t *as_clone_cow(address_space_t *src) {
    address_space_t *dst = as_create();
    if (!dst) return 0;

    u32 shm_slot = pd_slot_of(PROC_SHM_BASE);

    for (u32 slot = PROC_FIRST_PD_SLOT; slot <= PROC_LAST_PD_SLOT; slot++) {
        u64 *src_pd = table_at(src->pd0_phys);
        if (!(src_pd[slot] & PTE_PRESENT)) continue; /* в этом окне у родителя ещё ничего нет */

        u64 *src_pt = table_at(src_pd[slot]);
        for (u32 i = 0; i < PT_ENTRIES; i++) {
            if (!(src_pt[i] & PTE_PRESENT)) continue;

            u64 paddr = src_pt[i] & PTE_ADDR_MASK;
            u64 vaddr = ((u64)slot << 21) | ((u64)i << 12);
            bool_t executable = !(src_pt[i] & PTE_NX); /* если NX не поддержан, бит всегда 0 -> "исполняем", как раньше */

            pmm_frame_addref((void *)(uintptr_t)paddr);

            if (slot == shm_slot) {
                /* Разделяемая память (см. include/ipc.h) -- единственное
                   окно, для которого copy-on-write не подходит по смыслу
                   самого примитива: она обязана остаться ДЕЙСТВИТЕЛЬНО
                   общей и после fork(), а не стать приватной копией у
                   того из двоих, кто первым в неё запишет. Поэтому здесь
                   просто повторяем отображение как у родителя (те же
                   права, включая writable), без PTE_COW -- прямо как
                   shm_attach() делал бы для ещё одного присоединения к
                   тому же самому сегменту (см. kernel/ipc.c). Учёт
                   attach_count самого сегмента синхронизирует со счётчиком
                   ipc_task_fork_copy() -- см. её вызов из process_fork().
                   executable здесь сознательно ИГНОРИРУЕТСЯ и всегда FALSE
                   (см. W^X-комментарий в начале файла, include/paging.h)
                   -- ни одному процессу не нужно исполнять код прямо из
                   разделяемой памяти, а вот эксплойту, который туда что-то
                   записал, это было бы очень удобно. */
                bool_t writable = (src_pt[i] & PTE_WRITABLE) != 0;
                as_map_frame(dst, vaddr, paddr, writable, FALSE, FALSE);
                continue;
            }

            if (!(src_pt[i] & PTE_WRITABLE)) {
                /* Страница и так уже read-only (типичный случай -- код/
                   rodata: elf_load() грузит их без PF_W, см.
                   kernel/loader.c) -- COW ей попросту не нужен: раз ни
                   один из двоих процессов и так не может её изменить,
                   значит и расхождения между родителем и потомком быть не
                   может, а значит можно смело оставить кадр честно общим
                   НАВСЕГДА, просто добавив вторую ссылку (что мы уже
                   сделали строкой выше). До этой правки такие страницы
                   тоже помечались COW заодно со всеми остальными -- в
                   этом и был сам баг: запись в них (например, через
                   намеренно испорченный указатель) проходила через общий
                   путь "разрешить COW" ниже в
                   paging_handle_page_fault(), которая безусловно снимает
                   PTE_COW и ставит PTE_WRITABLE -- то есть fork() сам по
                   себе тихо открывал дорогу к перезаписи read-only кода.
                   Теперь такая страница просто никогда не получает
                   PTE_COW, и попытка записи в нём остаётся настоящим,
                   честным segfault-ом и после fork(), ровно как и до
                   него. */
                as_map_frame(dst, vaddr, paddr, FALSE, executable, FALSE);
                continue;
            }

            /* Родитель отныне тоже read-only+COW -- ровно тот же кадр
               общий на двоих, писать в него напрямую больше нельзя ни
               одному из них, пока кто-то из двоих не спровоцирует
               настоящее копирование обращением на запись. NX (или его
               отсутствие) переносим как есть -- writable-страница может
               быть как обычными данными (NX), так и, в принципе, W+X
               сегментом (если ELF почему-то так попросил, см. комментарий
               в include/paging.h) -- COW не место решать это заново. */
            src_pt[i] = (src_pt[i] & ~PTE_WRITABLE) | PTE_COW;
            invlpg(vaddr);

            as_map_frame(dst, vaddr, paddr, FALSE, executable, TRUE);
        }
    }

    return dst;
}

/* Возвращает базу и максимальный размер региона, которому принадлежит
   слот (для растущих регионов -- стека/кучи -- это МАКСИМАЛЬНО
   допустимая величина, а не текущая уже подкачанная часть). TRUE, если
   слот вообще относится к одному из известных регионов. */
static bool_t region_of_slot(u32 slot, u64 *out_base, u32 *out_size, bool_t *out_grows_down) {
    if (slot == pd_slot_of(PROC_CODE_BASE)) {
        *out_base = PROC_CODE_BASE; *out_size = PROC_CODE_SIZE; *out_grows_down = FALSE;
        return TRUE;
    }
    if (slot == pd_slot_of(PROC_STACK_BASE)) {
        *out_base = PROC_STACK_BASE; *out_size = PROC_STACK_SIZE; *out_grows_down = TRUE;
        return TRUE;
    }
    if (slot >= pd_slot_of(PROC_HEAP_BASE) &&
        slot <  pd_slot_of(PROC_HEAP_BASE) + (PROC_HEAP_SIZE / SLOT_SIZE)) {
        *out_base = PROC_HEAP_BASE; *out_size = PROC_HEAP_SIZE; *out_grows_down = FALSE;
        return TRUE;
    }
    return FALSE;
}

bool_t as_range_is_user_accessible(const address_space_t *as, u64 start, u32 len) {
    if (!as) return FALSE;
    if (len == 0) return TRUE;

    u64 end = start + len;
    if (end < start) return FALSE; /* переполнение адреса */

    u32 slot = pd_slot_of(start);

    if (slot == pd_slot_of(PROC_SHM_BASE)) {
        /* Окно разделяемой памяти (см. include/memlayout.h) -- ЕДИНСТВЕННЫЙ
           регион, где недостаточно проверить, что диапазон попадает в
           максимальный размер окна: в отличие от стека/кучи, здесь ничего
           не подкачивается лениво через #PF (см. paging_handle_page_fault
           ниже -- он про этот слот вообще ничего не знает), поэтому
           страница, которая ещё не была явно отображена через
           shm_attach() (kernel/ipc.c), никогда сама не появится -- значит,
           доверять указателю на неё нельзя. Проверяем, что диапазон лежит
           в границах окна И что каждая покрываемая 4 KiB страница
           ДЕЙСТВИТЕЛЬНО присутствует прямо сейчас. */
        if (start < PROC_SHM_BASE || end > PROC_SHM_BASE + PROC_SHM_SIZE) return FALSE;

        u64 *pt = get_or_create_pt((address_space_t *)as, slot, FALSE);
        if (!pt) return FALSE; /* в этом окне у процесса вообще ничего не присоединено */

        u64 page_start = start & ~0xFFFull;
        u64 page_end   = (end + 0xFFFull) & ~0xFFFull;
        for (u64 a = page_start; a < page_end; a += PAGE_SIZE) {
            u32 idx = (u32)((a >> 12) & 0x1FF);
            if (!(pt[idx] & PTE_PRESENT)) return FALSE;
        }
        return TRUE;
    }

    u64 base; u32 size; bool_t grows_down;
    if (!region_of_slot(slot, &base, &size, &grows_down)) return FALSE;
    if (start < base || end > base + size) return FALSE;
    /* Для регионов из нескольких слотов (куча) убеждаемся, что второй
       конец диапазона попадает в тот же самый регион, а не улетел в
       соседний слот другого назначения -- уже проверено сравнением с
       [base, base+size), этого достаточно, т.к. регионы не соприкасаются
       вплотную (между ними обычные "дырявые" слоты не выделены никому). */
    return TRUE;
}

void as_unmap_range(address_space_t *as, u64 vaddr, u32 len) {
    if (!as || len == 0) return;

    u64 page_start = vaddr & ~0xFFFull;
    u64 page_end   = (vaddr + len + 0xFFFull) & ~0xFFFull;

    for (u64 a = page_start; a < page_end; a += PAGE_SIZE) {
        u32 slot = pd_slot_of(a);
        u64 *pt = get_or_create_pt(as, slot, FALSE);
        if (!pt) continue; /* для этого слота вообще нет PT -- ничего и не было отображено */

        u32 idx = (u32)((a >> 12) & 0x1FF);
        if (!(pt[idx] & PTE_PRESENT)) continue;

        u64 paddr = pt[idx] & PTE_ADDR_MASK;
        pt[idx] = 0;
        invlpg(a);
        pmm_frame_release((void *)(uintptr_t)paddr);
    }
}

bool_t paging_handle_page_fault(address_space_t *as, u64 fault_vaddr, u64 error_code) {
    if (!as) return FALSE;

    u32 slot = pd_slot_of(fault_vaddr);
    u64 base; u32 size; bool_t grows_down;
    if (!region_of_slot(slot, &base, &size, &grows_down)) return FALSE;
    if (fault_vaddr < base || fault_vaddr >= base + size) return FALSE;

    bool_t was_present = (error_code & PF_ERR_PRESENT) != 0;
    bool_t is_write    = (error_code & PF_ERR_WRITE) != 0;

    u64 page_vaddr = fault_vaddr & ~0xFFFull;
    u64 *pt = get_or_create_pt(as, slot, TRUE);
    if (!pt) return FALSE; /* нехватка памяти под саму таблицу -- ничего не поделать */
    u32 idx = (u32)((page_vaddr >> 12) & 0x1FF);

    if (was_present) {
        /* Страница существует -- единственный случай, который мы умеем
           чинить, это разрешение copy-on-write при попытке записи. */
        if (!is_write) return FALSE;
        if (!(pt[idx] & PTE_COW)) return FALSE; /* обычное нарушение прав, не COW */

        u64 old_paddr = pt[idx] & PTE_ADDR_MASK;
        u64 old_nx    = pt[idx] & PTE_NX; /* переносим как есть -- см. as_clone_cow() в этом файле:
                                              единственные страницы, доходящие сюда через COW, это
                                              бывшие writable-страницы (данные/куча/стек), у которых
                                              NX уже был выставлен корректно при их создании; код/rodata
                                              сюда попасть не могут -- as_clone_cow() больше не помечает
                                              read-only страницы как COW (см. её комментарий). */
        void *new_frame = pmm_alloc_frame();
        if (!new_frame) return FALSE; /* OOM -- программе не повезло, пусть падает */

        memcpy(new_frame, (const void *)(uintptr_t)old_paddr, PAGE_SIZE);
        pmm_frame_release((void *)(uintptr_t)old_paddr); /* наша ссылка на старый (общий) кадр */

        pt[idx] = (u64)(uintptr_t)new_frame | PTE_PRESENT | PTE_WRITABLE | PTE_USER | old_nx;
        invlpg(page_vaddr);
        return TRUE;
    }

    /* Страницы ещё нет: либо рост кучи/стека "по требованию", либо
       (для стека) реальное переполнение за отведённый максимум. Для
       региона кода такого случая быть не должно (он весь отображается
       заранее целиком, см. kernel/process.c) -- если всё же случилось,
       считаем это багом программы, а не поводом что-то придумывать. */
    if (base == PROC_CODE_BASE) return FALSE;
    (void)grows_down; /* для стека рост "вниз" уже гарантирован тем, что весь
                          регион [base, base+size) заранее известен и проверен
                          выше -- отдельная нижняя граница здесь не нужна */

    void *frame = pmm_alloc_frame();
    if (!frame) return FALSE;
    memset(frame, 0, PAGE_SIZE);

    /* Свежая страница кучи/стека -- всегда NX (см. W^X-комментарий в
       начале файла и в include/paging.h): программе никогда не нужно
       исполнять данные из собственного стека или кучи, а вот эксплойту,
       который туда что-то записал (переполнение буфера, шелл-код в
       куче и т.п.), это было бы очень нужно. */
    pt[idx] = (u64)(uintptr_t)frame | PTE_PRESENT | PTE_WRITABLE | PTE_USER
              | (g_nx_supported ? PTE_NX : 0);
    invlpg(page_vaddr);
    return TRUE;
}
