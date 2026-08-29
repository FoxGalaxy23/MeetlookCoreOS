/* timer.c -- см. include/timer.h за общим описанием. */

#include "timer.h"
#include "isr.h"
#include "pic.h"
#include "io.h"
#include "task.h"

#define PIT_CHANNEL0_DATA 0x40
#define PIT_COMMAND       0x43
#define PIT_BASE_FREQ     1193182u /* тактовая частота входа PIT, Гц */

static volatile u64 g_ticks = 0;
static u32 g_freq_hz = 0;

static void timer_irq_handler(registers_t *regs) {
    (void)regs;
    g_ticks++;

    /* scheduler_tick() сама решает, пора ли снимать текущую задачу с
       процессора (см. TASK_TIME_SLICE_TICKS в task.h) -- таймер лишь
       поставляет равномерный "пульс", вся политика планирования живёт
       в task.c. */
    scheduler_tick();
}

void timer_init(u32 freq_hz) {
    if (freq_hz == 0) freq_hz = 100;

    u32 divisor = PIT_BASE_FREQ / freq_hz;
    if (divisor == 0) divisor = 1;
    if (divisor > 0xFFFF) divisor = 0xFFFF; /* делитель PIT -- 16 бит */

    outb(PIT_COMMAND, 0x36); /* канал 0, lobyte/hibyte, режим 3 (square wave), binary */
    outb(PIT_CHANNEL0_DATA, (u8)(divisor & 0xFF));
    outb(PIT_CHANNEL0_DATA, (u8)((divisor >> 8) & 0xFF));

    irq_register_handler(0, timer_irq_handler);
    pic_clear_mask(0); /* размаскируем именно IRQ0 -- остальные не трогаем */

    g_freq_hz = freq_hz; /* запоминаем ПОСЛЕ округления freq_hz==0, ДО деления на divisor */
}

u64 timer_ticks(void) {
    return g_ticks;
}

u32 timer_frequency(void) {
    return g_freq_hz;
}
