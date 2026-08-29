/* smp.c -- см. include/smp.h за общим описанием модели. */

#include "smp.h"
#include "acpi.h"
#include "apic.h"
#include "percpu.h"
#include "task.h"
#include "gdt.h"
#include "idt.h"
#include "isr.h"
#include "paging.h"
#include "kheap.h"
#include "kprintf.h"
#include "string.h"
#include "timer.h"

/* Встроенный в образ ядра образ трамплина (см. boot/ap_trampoline.s,
   Makefile: build/ap_trampoline.bin -> kernel/ap_trampoline_blob.s) --
   ровно AP_TRAMPOLINE_SIZE байт, начиная с ap_trampoline_blob. */
extern u8 ap_trampoline_blob[];
extern u8 ap_trampoline_blob_end[];

/* Заглушки из kernel/smp_isr.s. */
extern void lapic_timer_isr(void);
extern void ipi_reschedule_isr(void);
extern void ipi_call_isr(void);
extern void lapic_spurious_isr(void);

/* Точка входа AP после трамплина (см. общий комментарий в
   boot/ap_trampoline.s про "почтовый ящик") -- адрес этой функции
   записывается в мейлбокс ДО первого SIPI, см. smp_start_ap() ниже.
   cpu_index приходит в rdi (1-й аргумент по System V AMD64 ABI) --
   трамплин кладёт его туда из мейлбокса непосредственно перед прыжком. */
void ap_entry64(u32 cpu_index);

/* TRUE, как только очередной поднимаемый AP исполнил самую первую
   строчку ap_entry64() -- см. общий комментарий там же. BSP ждёт этот
   флаг (с таймаутом) после каждого SIPI, прежде чем переходить к
   следующему ядру -- вся процедура подъёма НАМЕРЕННО последовательная
   (см. общий комментарий в include/smp.h), поэтому один и тот же
   мейлбокс безопасно переиспользуется для каждого следующего AP. */
static volatile u32 g_ap_booted = 0;

static void ap_idle_entry(void);

/* ---- обработчики новых SMP-векторов (см. kernel/smp_isr.s) ---- */

void lapic_timer_c_handler(void) {
    scheduler_tick();
    lapic_eoi();
}

void ipi_reschedule_c_handler(void) {
    /* Ничего, кроме подтверждения -- само прерывание уже прервало HLT в
       ap_idle_entry() этого ядра (см. include/smp.h: smp_kick_idle_cpus()),
       следующий оборот цикла простоя сам обратится к общей очереди
       планировщика через task_yield(). */
    lapic_eoi();
}

void ipi_call_c_handler(void) {
    lapic_eoi();
}

/* Ждёт до max_ticks тиков PIT (см. include/timer.h), либо пока
   g_ap_booted не станет TRUE -- возвращает TRUE, если дождались. */
static bool_t wait_for_ap_boot(u32 max_ticks) {
    u64 deadline = timer_ticks() + max_ticks;
    while (timer_ticks() < deadline) {
        if (__atomic_load_n(&g_ap_booted, __ATOMIC_SEQ_CST)) return TRUE;
        __asm__ volatile ("pause" ::: "memory");
    }
    return __atomic_load_n(&g_ap_booted, __ATOMIC_SEQ_CST);
}

/* Грубая задержка на ~us микросекунд, не завязанная на timer_ticks()
   (PIT в этой системе тикает всего на 100 Гц, т.е. раз в 10 мс -- этого
   недостаточно для пауз в десятки-сотни микросекунд, которые MP-спецификация
   рекомендует между INIT и SIPI). Простой busy-loop с "pause" -- не
   точная задержка, а разумный запас: современные CPU выполняют его
   более чем достаточно быстро относительно требуемых интервалов, а
   реальная точность здесь не критична (см. Intel MP Spec 1.4 §B.4 --
   рекомендованные тайминги описаны как минимумы с большим запасом). */
static void busy_wait_short(u32 iterations) {
    for (u32 i = 0; i < iterations; i++) {
        __asm__ volatile ("pause" ::: "memory");
    }
}

/* Поднимает ОДНО ядро с физическим APIC ID target_apic_id, присваивая
   ему логический номер cpu_index (1, 2, 3... -- 0 зарезервирован за
   BSP). Последовательная процедура (см. общий комментарий в
   include/smp.h): мейлбокс трамплина переиспользуется для каждого AP
   по очереди, поэтому следующий SIPI нельзя слать, пока текущий AP не
   подтвердил, что уже считал СВОИ значения из мейлбокса в регистры
   (см. g_ap_booted выше). */
static bool_t smp_start_ap(u32 cpu_index, u32 target_apic_id) {
    u8 *stack = (u8 *)kmalloc(TASK_STACK_SIZE);
    if (!stack) {
        kprintf("smp: cpu %u: out of memory for stack, skipping.\n", cpu_index);
        return FALSE;
    }

    /* -8 от 16-байтной границы -- имитирует состояние стека сразу ПОСЛЕ
       обычного `call` (см. boot/ap_trampoline.s: ap_long64 просто
       `jmp`, а не `call`, в entry_point -- без этой поправки первая же
       инструкция ap_entry64(), сохраняющая что-то через выровненный по
       16 байт SSE/movaps, работала бы с неверно выровненным rsp). */
    u64 stack_top = ((u64)(uintptr_t)(stack + TASK_STACK_SIZE) & ~0xFULL) - 8u;

    volatile u64 *mailbox = (volatile u64 *)(uintptr_t)(AP_TRAMPOLINE_BASE + AP_MAILBOX_OFFSET);
    mailbox[AP_MAILBOX_STACK_OFF / 8]  = stack_top;
    mailbox[AP_MAILBOX_ENTRY_OFF / 8]  = (u64)(uintptr_t)ap_entry64;
    mailbox[AP_MAILBOX_CPUIDX_OFF / 8] = cpu_index;

    __atomic_store_n(&g_ap_booted, 0u, __ATOMIC_SEQ_CST);

    /* INIT -> пауза ~10 мс -> SIPI -> пауза -> SIPI (см. Intel MP Spec
       1.4 §B.4, а также lapic_send_init()/lapic_send_sipi() в
       kernel/apic.c за смыслом каждого шага). */
    lapic_send_init(target_apic_id);
    {
        u64 deadline = timer_ticks() + 2; /* ~10-20 мс при 100 Гц PIT -- заведомо с запасом */
        while (timer_ticks() < deadline) __asm__ volatile ("pause" ::: "memory");
    }

    lapic_send_sipi(target_apic_id, AP_TRAMPOLINE_VECTOR);
    busy_wait_short(200000u);
    if (wait_for_ap_boot(2)) {
        kprintf("smp: cpu %u (apic id %u) online.\n", cpu_index, target_apic_id);
        return TRUE;
    }

    /* Первый SIPI не подтвердился вовремя -- по MP-спецификации это
       штатный повод послать второй (некоторые старые прошивки требуют
       ровно двух SIPI подряд) прежде чем считать AP неработоспособным. */
    lapic_send_sipi(target_apic_id, AP_TRAMPOLINE_VECTOR);
    if (wait_for_ap_boot(10)) {
        kprintf("smp: cpu %u (apic id %u) online (после второго SIPI).\n", cpu_index, target_apic_id);
        return TRUE;
    }

    kprintf("smp: cpu %u (apic id %u) не ответил на SIPI, пропускаем.\n", cpu_index, target_apic_id);
    return FALSE;
}

void smp_init(void) {
    /* LAPIC нужен уже на BSP -- и чтобы вообще слать IPI/SIPI ниже, и
       чтобы BSP сам был полноценным участником IPI-рассылок в будущем
       (см. smp_kick_idle_cpus()). percpu_init_early(0, ...) для BSP уже
       был вызван раньше, с заведомо неточным lapic_id (см.
       kernel/kernel.c) -- поправляем его на настоящий здесь же. */
    lapic_init();
    this_cpu()->lapic_id = lapic_id();

    idt_set_gate(LAPIC_TIMER_VECTOR,   (u64)(uintptr_t)lapic_timer_isr,     GDT_KERNEL_CODE_SEL, IDT_GATE_INTERRUPT);
    idt_set_gate(LAPIC_IPI_RESCHEDULE, (u64)(uintptr_t)ipi_reschedule_isr,  GDT_KERNEL_CODE_SEL, IDT_GATE_INTERRUPT);
    idt_set_gate(LAPIC_IPI_CALL,       (u64)(uintptr_t)ipi_call_isr,        GDT_KERNEL_CODE_SEL, IDT_GATE_INTERRUPT);
    idt_set_gate(LAPIC_SPURIOUS_VECTOR,(u64)(uintptr_t)lapic_spurious_isr,  GDT_KERNEL_CODE_SEL, IDT_GATE_INTERRUPT);

    lapic_timer_calibrate();

    acpi_cpu_info_t cpus[ACPI_MAX_CPUS];
    u32 found = acpi_find_cpus(cpus, ACPI_MAX_CPUS);

    if (found <= 1) {
        kprintf("smp: ACPI недоступен или нашлось только одно ядро -- работаем однопроцессорно.\n");
        return;
    }

    /* Копируем трамплин в физическую память один раз -- см. общий
       комментарий в boot/ap_trampoline.s про то, почему адрес фиксирован
       (AP_TRAMPOLINE_BASE) и почему это безопасно (зарезервированный
       первый мегабайт, см. include/pmm.h). PML4 в мейлбоксе тоже общий
       для всех AP -- пишем один раз, до цикла ниже. */
    memcpy((void *)(uintptr_t)AP_TRAMPOLINE_BASE, ap_trampoline_blob, AP_TRAMPOLINE_SIZE);
    volatile u64 *mailbox = (volatile u64 *)(uintptr_t)(AP_TRAMPOLINE_BASE + AP_MAILBOX_OFFSET);
    mailbox[AP_MAILBOX_PML4_OFF / 8] = paging_kernel_cr3();

    u32 next_cpu_index = 1; /* 0 занят BSP-ом */
    u32 started = 1;         /* BSP уже "поднят" по определению */

    for (u32 i = 0; i < found; i++) {
        if (!cpus[i].enabled) continue;
        if (cpus[i].apic_id == this_cpu()->lapic_id) continue; /* это сам BSP -- уже работает */

        if (smp_start_ap(next_cpu_index, cpus[i].apic_id)) {
            started++;
            next_cpu_index++;
        }
        if (next_cpu_index >= SMP_MAX_CPUS) break; /* см. include/percpu.h: SMP_MAX_CPUS */
    }

    kprintf("smp: %u ядро(а) в строю (из %u найденных прошивкой).\n", started, found);
}

u32 smp_cpu_count(void) {
    return percpu_online_count();
}

void smp_kick_idle_cpus(void) {
    lapic_send_ipi_broadcast(LAPIC_IPI_RESCHEDULE);
}

/* Цикл простоя КАЖДОГО AP (см. task_register_this_cpu() в
   kernel/task.c -- он и превращает вызов этой функции в полноценную
   task_t планировщика, участвующую в общей очереди). task_yield() даёт
   общей очереди шанс подсунуть этому ядру настоящую работу; если
   очередь пуста, schedule() внутри task_yield() просто сразу вернёт
   управление сюда же (см. общий комментарий в kernel/task.c: ветка
   "candidate == prev") -- и тогда hlt экономит энергию и ждёт следующего
   прерывания (локальный таймер этого ядра либо reschedule-IPI, см.
   smp_kick_idle_cpus()), после чего цикл повторяется. */
static void ap_idle_entry(void) {
    for (;;) {
        task_yield();
        __asm__ volatile ("hlt");
    }
}

void ap_entry64(u32 cpu_index) {
    /* Самая первая строчка -- сигнализируем BSP, что мейлбокс трамплина
       (см. boot/ap_trampoline.s) уже безопасно переиспользовать для
       следующего AP: все нужные нам значения (стек, свой номер, точка
       входа) мы уже получили через регистры ДО прыжка сюда, самого
       мейлбокса эта функция больше не читает. */
    __atomic_store_n(&g_ap_booted, 1u, __ATOMIC_SEQ_CST);

    /* ВТОРАЯ строчка, и совершенно намеренно раньше вообще всего
       остального (в т.ч. раньше percpu_init_early()): пока IDT на этом
       ядре не загружена, у процессора попросту нет куда провалиться при
       ЛЮБОМ исключении (даже #PF/#UD, случившемся из-за ещё не готового
       this_cpu()/GDT) -- а это гарантированный double, следом triple
       fault и перезагрузка всей машины (см. общий комментарий в
       boot/ap_trampoline.s про enable_sse -- отсутствие рабочей IDT
       ровно на этом крошечном интервале и было настоящей причиной
       буквально такого симптома). idt_load_current() -- чистая lidt на
       уже заполненную BSP-ом таблицу (см. include/idt.h), никаких
       зависимостей от percpu/GDT ей не нужно. */
    idt_load_current();

    u32 apic = lapic_id(); /* MMIO-регистры LAPIC -- честно локальные для КАЖДОГО ядра
                               (см. общий комментарий в include/apic.h), хотя виртуальный/
                               физический адрес окна регистров (g_lapic_mmio в apic.c)
                               одинаков на всех ядрах и потому безопасно переиспользуется
                               без отдельной инициализации на каждом ядре. */
    percpu_init_early(cpu_index, apic);

    gdt_init_ap();
    lapic_init();
    lapic_timer_start_periodic(100); /* та же частота, что и PIT у BSP (см. kernel/kernel.c) --
                                         не архитектурное требование, а просто единообразие */

    task_register_this_cpu("ap-idle");

    __asm__ volatile ("sti");

    ap_idle_entry(); /* не возвращается */
}
