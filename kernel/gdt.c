#include "gdt.h"
#include "string.h"
#include "percpu.h"

/* Биты обычного (не системного) 8-байтного дескриптора сегмента --
   позиции совпадают с тем, что уже собрано вручную в boot/boot.s,
   просто здесь они именованы, а не написаны магическими числами. */
#define SEG_RW           (1ULL << 41)   /* readable(code) / writable(data) */
#define SEG_EXECUTABLE   (1ULL << 43)
#define SEG_NOT_SYSTEM   (1ULL << 44)   /* S=1 -- обычный код/данные, не TSS/LDT */
#define SEG_DPL(x)       (((u64)(x) & 3ULL) << 45)
#define SEG_PRESENT      (1ULL << 47)
#define SEG_LONG         (1ULL << 53)   /* L=1 -- 64-битный код (только для code) */

#define KERNEL_CODE_DESC (SEG_PRESENT | SEG_NOT_SYSTEM | SEG_EXECUTABLE | SEG_RW | SEG_LONG | SEG_DPL(0))
#define KERNEL_DATA_DESC (SEG_PRESENT | SEG_NOT_SYSTEM | SEG_RW | SEG_DPL(0))
#define USER_CODE_DESC   (SEG_PRESENT | SEG_NOT_SYSTEM | SEG_EXECUTABLE | SEG_RW | SEG_LONG | SEG_DPL(3))
#define USER_DATA_DESC   (SEG_PRESENT | SEG_NOT_SYSTEM | SEG_RW | SEG_DPL(3))

/* TSS в 64-битном режиме -- структура фиксированного размера (Intel
   SDM Vol.3 §7.7): база/лимит сегментов из 32-битного TSS исчезли,
   вместо них RSP0..RSP2 (стеки для переходов на более привилегированные
   кольца) и IST1..IST7 (альтернативные стеки для конкретных векторов
   прерываний, не используем, все нули). Нам из всего этого реально
   нужен только rsp0 -- см. gdt.h.

   io_bitmap -- битовая карта портов ввода-вывода (Intel SDM Vol.3
   §18.5.2): по одному биту на КАЖДЫЙ из 65536 портов (8192 байта), плюс
   один завершающий байт 0xFF (см. тот же параграф SDM -- если 2-байтный
   in/out задевает бит ровно на границе карты, процессор должен суметь
   прочитать байт СРАЗУ ЗА картой, и он обязан быть весь единицами,
   иначе поведение не определено). Бит=0 -- порт разрешён из ring 3,
   бит=1 -- запрещён (#GP). До Фазы 4 картой не пользовались вообще
   (iomap_base указывал ЗА пределы TSS -- см. gdt_init_cpu() ниже в
   старой версии -- что эквивалентно "все биты 1", то есть полный
   запрет). Теперь это настоящая карта, которую планировщик
   перепрограммирует под каждую задачу перед переключением на неё --
   см. gdt_apply_io_permissions() и её вызов в kernel/task.c: schedule(). */
typedef struct __attribute__((packed)) {
    u32 reserved0;
    u64 rsp0;
    u64 rsp1;
    u64 rsp2;
    u64 reserved1;
    u64 ist1, ist2, ist3, ist4, ist5, ist6, ist7;
    u64 reserved2;
    u16 reserved3;
    u16 iomap_base;
    u8  io_bitmap[8193];
} tss_t;

typedef struct __attribute__((packed)) {
    u16 limit;
    u64 base;
} gdt_ptr_t;

/* null, kernel-code, kernel-data, user-code, user-data, tss(lo), tss(hi) */
#define GDT_ENTRIES 7

/* SMP: КАЖДОЕ ядро обязано иметь СВОЙ GDT и СВОЙ TSS -- вот почему это
 * теперь массивы, индексированные this_cpu()->cpu_index (см.
 * include/percpu.h), а не одиночные статические переменные, какими они
 * были до SMP. Причина -- TSS.RSP0: это единственное, ради чего вообще
 * нужен TSS в этой ОС (см. общий комментарий в include/gdt.h), а RSP0 --
 * это "куда переключить стек при прерывании из ring 3 у ЭТОГО
 * конкретного ядра ПРЯМО СЕЙЧАС" (планировщик обновляет его перед каждым
 * переключением на процессную задачу, см. schedule() в kernel/task.c).
 * Если бы TSS был один на всех, второе ядро, обслуживающее своё
 * ring3-прерывание примерно в то же время, затёрло бы RSP0 первого --
 * оба тогда попытались бы использовать чей-то чужой стек ядра, что почти
 * гарантированно кончилось бы повреждением памяти или double fault.
 * Сам GDT (кроме TSS-дескриптора) у всех ядер СОДЕРЖАТЕЛЬНО одинаков
 * (одни и те же 4 обычных дескриптора кольца 0/3) -- отдельные копии
 * нужны не потому, что содержимое разное, а потому что запись TSS
 * (индексы 5-6) внутри GDT обязана указывать на TSS ИМЕННО этого ядра. */
static u64       g_gdt[SMP_MAX_CPUS][GDT_ENTRIES];
static gdt_ptr_t g_gdt_ptr[SMP_MAX_CPUS];
static tss_t     g_tss[SMP_MAX_CPUS];

/* Стек по умолчанию для TSS.RSP0 каждого ядра, пока ни одно ring3-приложение
   ещё не запускалось на нём. Ни одна задача планировщика (kernel/task.c)
   сама по себе кольцо не меняет, так что фактически он никогда не
   используется -- но оставлять RSP0 равным 0 нельзя: если процессор
   вдруг захочет переключиться на RSP0=0, он упадёт в double fault на
   первом же push, ещё до того, как мы успеем вывести хоть какую-то
   диагностику. Отдельный такой стек на каждое ядро -- по той же причине,
   по которой и весь остальной TSS персональный (см. выше). */
static u8 g_default_rsp0_stack[SMP_MAX_CPUS][4096] __attribute__((aligned(16)));

static void gdt_set_entry(u32 cpu, u32 index, u64 base, u32 limit, u64 flags) {
    u64 desc = flags;
    desc |= (u64)(limit & 0xFFFFu);
    desc |= ((u64)(limit >> 16) & 0xFu) << 48;
    desc |= ((u64)base & 0xFFFFFFu) << 16;
    desc |= ((u64)(base >> 24) & 0xFFu) << 56;
    g_gdt[cpu][index] = desc;
}

/* Системный дескриптор (TSS) в long mode занимает 16 байт -- два
   соседних 8-байтных слота вместо одного, потому что базовый адрес
   теперь 64-битный, а не 32-битный. Младший слот устроен почти как
   обычный дескриптор (S=0 -- системный, type=0x9 -- "64-bit TSS
   (available)", Intel SDM Vol.3 §7.2.3), старший -- просто продолжение
   базы (биты 32-63) плюс зарезервированные нули. */
static void gdt_set_tss(u32 cpu, u32 index, u64 base, u32 limit) {
    u64 low = 0;
    low |= (u64)(limit & 0xFFFFu);
    low |= ((u64)base & 0xFFFFFFu) << 16;
    low |= (0x9ULL) << 40;                       /* type = available 64-bit TSS */
    low |= (1ULL << 47);                          /* present */
    low |= ((u64)(limit >> 16) & 0xFu) << 48;
    low |= ((u64)(base >> 24) & 0xFFu) << 56;

    u64 high = (base >> 32) & 0xFFFFFFFFULL;

    g_gdt[cpu][index]     = low;
    g_gdt[cpu][index + 1] = high;
}

/* Классический трюк "перезагрузить CS": инструкции mov в cs не
   существует, поэтому пере-указываем сегментные регистры данных
   обычным mov, а CS -- дальним возвратом (push нового селектора и
   адреса метки, затем lretq), см. любую вводную по написанию GDT
   (например OSDev Wiki, "GDT Tutorial"). Компилируется как обычная
   функция, поэтому используем %rip-относительную метку. */
static inline void reload_segments_and_cs(void) {
    __asm__ volatile (
        "mov $0x10, %%ax\n\t"
        "mov %%ax, %%ds\n\t"
        "mov %%ax, %%es\n\t"
        "mov %%ax, %%fs\n\t"
        "mov %%ax, %%ss\n\t"
        "pushq $0x08\n\t"
        "leaq 1f(%%rip), %%rax\n\t"
        "pushq %%rax\n\t"
        "lretq\n\t"
        "1:\n\t"
        :
        :
        : "rax", "memory"
    );
    /* %gs НЕ трогаем здесь (в отличие от исходной однопроцессорной
       версии, которая перезагружала все сегментные регистры данных
       одинаково) -- на SMP скрытая база %gs хранит указатель на
       percpu_t этого ядра (см. include/percpu.h: this_cpu()), выставленный
       WRMSR-ом в IA32_GS_BASE ДО вызова gdt_init()/gdt_init_ap(). Обычная
       загрузка селектора в %gs (mov %ax, %gs) сбросила бы эту скрытую
       базу на дескрипторную (нулевую) -- то есть сломала бы this_cpu()
       сразу после первого же вызова gdt_init(). Селектор в самом %gs
       по-прежнему используется только неявно (через WRMSR), так что то,
       какое значение лежит в самом видимом регистре %gs, роли не играет
       -- лишь бы никто не переписывал его скрытую базу обычной загрузкой. */
}

/* Общая часть построения GDT+TSS для КОНКРЕТНОГО ядра cpu_index -- и
   gdt_init() (BSP), и gdt_init_ap() (любой AP) сводятся к этому вызову,
   различаясь только тем, ЧТО они мимоходом означают в остальной
   инициализации (см. общий комментарий у BSP/AP путей в
   kernel/kernel.c и kernel/smp.c). */
static void gdt_init_cpu(u32 cpu) {
    memset(g_gdt[cpu], 0, sizeof(g_gdt[cpu]));
    memset(&g_tss[cpu], 0, sizeof(g_tss[cpu]));

    gdt_set_entry(cpu, 1, 0, 0, KERNEL_CODE_DESC);
    gdt_set_entry(cpu, 2, 0, 0, KERNEL_DATA_DESC);
    gdt_set_entry(cpu, 3, 0, 0, USER_CODE_DESC);
    gdt_set_entry(cpu, 4, 0, 0, USER_DATA_DESC);

    g_tss[cpu].rsp0 = (u64)(uintptr_t)(g_default_rsp0_stack[cpu] + sizeof(g_default_rsp0_stack[cpu]));
    g_tss[cpu].iomap_base = (u16)offsetof(tss_t, io_bitmap);
    memset(g_tss[cpu].io_bitmap, 0xFF, sizeof(g_tss[cpu].io_bitmap)); /* по умолчанию -- запрет на все 65536 портов */
    gdt_set_tss(cpu, 5, (u64)(uintptr_t)&g_tss[cpu], sizeof(tss_t) - 1);

    g_gdt_ptr[cpu].limit = (u16)(sizeof(g_gdt[cpu]) - 1);
    g_gdt_ptr[cpu].base  = (u64)(uintptr_t)&g_gdt[cpu];

    __asm__ volatile ("lgdt %0" : : "m"(g_gdt_ptr[cpu]));
    reload_segments_and_cs();

    /* Селектор TSS (0x28) одинаков для всех ядер -- это просто индекс
       ВНУТРИ собственной GDT этого ядра (см. GDT_TSS_SEL, include/gdt.h),
       а не что-то, требующее знать номер ядра. Каждое ядро загружает
       СВОЮ же собственную GDT (lgdt выше) непосредственно перед этим,
       так что ltr не перепутает TSS одного ядра с TSS другого. */
    __asm__ volatile ("ltr %%ax" : : "a"((u16)GDT_TSS_SEL));
}

void gdt_init(void) {
    gdt_init_cpu(this_cpu()->cpu_index); /* BSP -- ожидается cpu_index == 0, см. include/gdt.h */
}

void gdt_init_ap(void) {
    gdt_init_cpu(this_cpu()->cpu_index);
}

void gdt_set_kernel_stack(u64 rsp0) {
    g_tss[this_cpu()->cpu_index].rsp0 = rsp0;
}

void gdt_apply_io_permissions(u16 io_base, u16 io_len) {
    tss_t *tss = &g_tss[this_cpu()->cpu_index];

    /* Запрет по умолчанию -- прежде чем разрешить диапазон ЭТОЙ задачи,
       снимаем всё, что могло остаться разрешённым от задачи, которая
       выполнялась на ЭТОМ ядре раньше (см. общий комментарий у
       io_bitmap в tss_t, kernel/gdt.c). 8192 байта -- дёшево по
       сравнению со стоимостью самого переключения контекста, поэтому
       не пытаемся оптимизировать "снять только старый диапазон, поставить
       только новый". */
    memset(tss->io_bitmap, 0xFF, sizeof(tss->io_bitmap));

    if (io_len == 0) return; /* у задачи вообще нет CAP_IO -- запрет остаётся полным */

    u32 end = (u32)io_base + (u32)io_len;
    if (end > 65536u) end = 65536u; /* защита от переполнения, если кто-то передаст мусорные значения */

    for (u32 port = io_base; port < end; port++) {
        tss->io_bitmap[port / 8] &= (u8)~(1u << (port % 8));
    }
}
