#ifndef SYSCALL_H
#define SYSCALL_H

#include "isr.h"

/* syscall.h -- диспетчер системных вызовов (вектор 0x80, см.
 * include/sysabi.h за номерами и соглашением о регистрах).
 *
 * Регистрация гейта отделена от общей инициализации IDT (idt_init(),
 * include/idt.h): вектор 0x80 -- единственный во всей таблице с DPL=3
 * (IDT_GATE_INTERRUPT_USER), и логически он принадлежит подсистеме
 * syscall'ов, а не общей настройке прерываний. */

/* Регистрирует вектор 0x80 -> isr128 (kernel/isr.s) с DPL=3. Вызывается
   один раз из kernel.c, после idt_init(). */
void syscall_init(void);

/* Вызывается из syscall_common_stub (kernel/isr.s) при каждом
   `int 0x80` -- не предназначена для прямого вызова из остального
   кода ядра. Для SYS_EXIT не возвращается (см. usermode_terminate()). */
void syscall_isr_handler(registers_t *regs);

#endif
