/* apic.c -- см. include/apic.h за общим описанием модели. */

#include "apic.h"
#include "timer.h"

#define IA32_APIC_BASE_MSR 0x1Bu

#define APIC_BASE_MSR_ENABLE (1u << 11)  /* глобальный software-enable всего LAPIC */
#define APIC_SVR_ENABLE      (1u << 8)   /* тот же смысл, но в регистре Spurious Interrupt Vector */

static volatile u32 *g_lapic_mmio = 0; /* базовый адрес окна регистров, см. lapic_base_addr() */
static u32 g_timer_ticks_per_ms = 0;    /* результат lapic_timer_calibrate(), общий для всех ядер
                                            (см. комментарий у lapic_timer_calibrate() в apic.h) */

static inline u64 rdmsr64(u32 msr) {
    u32 lo, hi;
    __asm__ volatile ("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return ((u64)hi << 32) | lo;
}

static inline void wrmsr64(u32 msr, u64 value) {
    u32 lo = (u32)(value & 0xFFFFFFFFu);
    u32 hi = (u32)(value >> 32);
    __asm__ volatile ("wrmsr" : : "c"(msr), "a"(lo), "d"(hi) : "memory");
}

/* Регистры LAPIC -- 32-битные, но каждый выровнен на границу 16 байт
   (соседние 12 байт зарезервированы/не используются, см. Intel SDM
   Vol.3 §10.4.1 табл.10-1) -- отсюда деление смещения на 4, а не на 1,
   при индексации u32-массива. */
static inline u32 lapic_read(u32 reg) {
    return g_lapic_mmio[reg / 4];
}

static inline void lapic_write(u32 reg, u32 value) {
    g_lapic_mmio[reg / 4] = value;
}

static void *lapic_base_addr(void) {
    /* Биты 12-35 IA32_APIC_BASE -- физический базовый адрес окна LAPIC
       (обычно 0xFEE00000, см. общий комментарий в apic.h); маскируем
       и биты enable/BSP/резерв, и всё, что выше 32 бит (адрес заведомо
       в нижних 4 GiB на любой системе, которую видит эта ОС -- boot.s
       identity-мапит ровно этот диапазон). */
    u64 base = rdmsr64(IA32_APIC_BASE_MSR);
    return (void *)(uintptr_t)(base & 0xFFFFF000ull);
}

void lapic_init(void) {
    if (g_lapic_mmio == 0) {
        g_lapic_mmio = (volatile u32 *)lapic_base_addr();
    }

    /* Программный enable на MSR-уровне (бит 11) -- без него весь LAPIC
       выключен целиком, независимо от SVR ниже (Intel SDM Vol.3
       §10.4.3). Аппаратный сброс уже включает его почти везде, но
       выставляем явно, а не полагаемся на состояние по умолчанию. */
    u64 base = rdmsr64(IA32_APIC_BASE_MSR);
    wrmsr64(IA32_APIC_BASE_MSR, base | APIC_BASE_MSR_ENABLE);

    /* SVR: бит 8 -- отдельный, "мягкий" enable уровня самого APIC (в
       отличие от MSR-уровня выше -- это две разные вещи, обе нужны);
       младший байт -- вектор spurious-прерывания. Для x86 младшие 4
       бита вектора spurious обязаны быть все единицами на некоторых
       старых моделях (Intel SDM Vol.3 §10.9) -- 0xFF этому требованию
       удовлетворяет и заодно оставляет весь остальной верх страницы
       векторов (0xF0-0xFE) свободным для реальных нужд (таймер, IPI). */
    lapic_write(LAPIC_REG_SPURIOUS, LAPIC_SPURIOUS_VECTOR | APIC_SVR_ENABLE);

    /* TPR=0 -- принимать прерывания любого приоритета (ничего не
       маскируем на уровне LAPIC, вся маскировка по-прежнему на 8259
       для унаследованных IRQ, см. общий комментарий в apic.h). */
    lapic_write(LAPIC_REG_TPR, 0);
}

u32 lapic_id(void) {
    /* В регистре ID физический APIC ID лежит в верхних 8 битах
       32-битного слова (Intel SDM Vol.3 §10.4.6) -- на всех системах,
       которые видит эта ОС (xAPIC, не x2APIC), этого достаточно. */
    return lapic_read(LAPIC_REG_ID) >> 24;
}

void lapic_eoi(void) {
    /* Само значение не важно -- запись чего угодно в этот регистр и
       есть сигнал "EOI" (Intel SDM Vol.3 §10.8.5). */
    lapic_write(LAPIC_REG_EOI, 0);
}

/* Ждём, пока аппаратура не отправит предыдущую команду ICR (busy-бит,
   Intel SDM Vol.3 §10.6.1) -- нельзя выставлять новую команду поверх
   ещё не ушедшей. На современных ядрах это почти всегда мгновенно, но
   гонка возможна, если IPI шлются одно за другим без паузы. */
static void icr_wait_idle(void) {
    while (lapic_read(LAPIC_REG_ICR_LOW) & (1u << 12)) {
        __asm__ volatile ("pause" ::: "memory");
    }
}

static void icr_send(u32 target_apic_id, u32 low) {
    icr_wait_idle();
    /* Сначала ICR_HIGH (получатель), потом ICR_LOW (собственно команда
       -- запись в ICR_LOW и есть момент фактической отправки, Intel SDM
       Vol.3 §10.6.1) -- порядок обязателен. */
    lapic_write(LAPIC_REG_ICR_HIGH, target_apic_id << 24);
    lapic_write(LAPIC_REG_ICR_LOW, low);
    icr_wait_idle();
}

void lapic_send_init(u32 target_apic_id) {
    /* delivery mode = 101 (INIT), level = assert (1), trigger = level (1) --
       классические магические числа 0xC500/0x8500 из любого учебника по
       написанию SMP-загрузки (см., например, OSDev Wiki "Symmetric
       Multiprocessing"): 0x500 -- delivery mode INIT, 0x4000 -- level
       assert, 0x8000 -- trigger mode level. */
    icr_send(target_apic_id, 0xC500u);

    /* На современных CPU (P4 и новее) отдельный "INIT deassert" не
       требуется теми же практическими рекомендациями -- но многие
       гипервизоры (в т.ч. QEMU/KVM) по-прежнему ожидают его ради
       полной совместимости со старой MP-спецификацией (Intel MP Spec
       1.4, §B.4) -- посылаем explicit deassert, это безопасно и ничего
       не стоит даже там, где не требуется. */
    icr_send(target_apic_id, 0x8500u);
}

void lapic_send_sipi(u32 target_apic_id, u8 vector) {
    /* delivery mode = 110 (Startup), vector -- страница (vector*0x1000),
       с которой AP начнёт исполнение в 16-битном real mode (см.
       kernel/smp.c: смысл vector там же, где строится образ трамплина). */
    icr_send(target_apic_id, 0x600u | vector);
}

void lapic_send_ipi(u32 target_apic_id, u8 vector) {
    icr_send(target_apic_id, vector); /* delivery mode = 000 (Fixed) по умолчанию (биты 8-10 нулевые) */
}

void lapic_send_ipi_broadcast(u8 vector) {
    icr_wait_idle();
    lapic_write(LAPIC_REG_ICR_HIGH, 0);
    /* destination shorthand = 11 (biты 18-19) -- "all excluding self"
       (Intel SDM Vol.3 §10.6.1 табл.10-6): не нужно ни знать APIC ID
       получателей, ни исключать себя вручную. */
    lapic_write(LAPIC_REG_ICR_LOW, (u32)vector | (3u << 18));
    icr_wait_idle();
}

void lapic_timer_start_periodic(u32 freq_hz) {
    if (freq_hz == 0) freq_hz = 100;
    if (g_timer_ticks_per_ms == 0) {
        /* Калибровка ещё не выполнялась (не должно происходить в
           нормальном порядке инициализации -- см. smp_init() в
           kernel/smp.c) -- используем заведомо грубую, но безопасную
           оценку, чтобы хотя бы не оставить таймер полностью
           незапущенным. */
        g_timer_ticks_per_ms = 100000u;
    }

    u32 divisor_reg = 0x3u; /* делитель шины на 16 (см. табл. значений ниже) */
    lapic_write(LAPIC_REG_TIMER_DIV, divisor_reg);

    u32 count = (g_timer_ticks_per_ms * 1000u) / freq_hz;
    if (count == 0) count = 1;

    /* Биты 0-7 LVT Timer -- вектор; бит 17 -- режим periodic (иначе
       one-shot, Intel SDM Vol.3 §10.5.1 рис.10-8). */
    lapic_write(LAPIC_REG_LVT_TIMER, LAPIC_TIMER_VECTOR | (1u << 17));
    lapic_write(LAPIC_REG_TIMER_INIT, count);
}

void lapic_timer_calibrate(void) {
    /* Делитель "16" -- код 0x3 в таблице значений регистра Timer Divide
       Configuration (Intel SDM Vol.3 §10.5.4 табл.10-11; не монотонная
       последовательность кодов, отсюда не 0,1,2... а именно эти
       значения): 000=2,001=4,010=8,011=16,... Берём 16 -- разумный
       компромисс между разрешением и тем, что initial count не
       переполнит 32 бита на быстрых шинах. */
    lapic_write(LAPIC_REG_TIMER_DIV, 0x3u);

    /* One-shot с заведомо большим начальным значением -- считаем,
       сколько тактов LAPIC успевает пройти за известный интервал
       времени по PIT (см. include/timer.h), который откалиброван
       заранее (timer_init(), см. kernel/kernel.c) и потому надёжен как
       эталон. ~10 мс (timer_frequency() тиков в секунду -> берём долю
       секунды) -- достаточно длинный интервал, чтобы ошибка округления
       PIT (целые тики) не исказила результат заметно, и достаточно
       короткий, чтобы не задерживать загрузку. */
    u32 pit_freq = timer_frequency();
    if (pit_freq == 0) pit_freq = 100;

    lapic_write(LAPIC_REG_TIMER_INIT, 0xFFFFFFFFu);

    u64 start_tick = timer_ticks();
    u64 target_tick = start_tick + (pit_freq / 100 == 0 ? 1 : pit_freq / 100); /* ~10 мс */
    while (timer_ticks() < target_tick) {
        __asm__ volatile ("pause" ::: "memory");
    }

    u32 elapsed = 0xFFFFFFFFu - lapic_read(LAPIC_REG_TIMER_CUR);
    lapic_write(LAPIC_REG_TIMER_INIT, 0); /* останавливаем one-shot */

    /* elapsed тактов LAPIC-таймера прошло примерно за 10 мс -- тактов в
       миллисекунду: */
    g_timer_ticks_per_ms = elapsed / 10u;
    if (g_timer_ticks_per_ms == 0) g_timer_ticks_per_ms = 1u;
}
