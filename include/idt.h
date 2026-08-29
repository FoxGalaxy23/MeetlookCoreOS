#ifndef IDT_H
#define IDT_H

#include "types.h"

/* idt.h -- Interrupt Descriptor Table для long mode (x86_64).
 *
 * В 64-битном режиме каждый дескриптор шлюза (gate) занимает 16 байт
 * (вдвое больше, чем в 32-битном protected mode) и хранит 64-битный
 * адрес обработчика, разбитый на три части (offset_low/mid/high).
 * Всего 256 векторов: 0-31 зарезервированы Intel/AMD под исключения
 * процессора (деление на ноль, page fault, ...), 32-255 свободны --
 * сюда попадают ремапнутые аппаратные IRQ (см. pic.h) и, в будущем,
 * программные прерывания (syscall через int).
 */

typedef struct __attribute__((packed)) {
    u16 offset_low;   /* биты 0-15 адреса обработчика */
    u16 selector;      /* селектор сегмента кода (см. boot/boot.s: gdt64.code = 0x08) */
    u8  ist;           /* Interrupt Stack Table index; 0 = не используется */
    u8  type_attr;     /* present | DPL | тип шлюза (0x8E = interrupt gate, кольцо 0) */
    u16 offset_mid;    /* биты 16-31 адреса обработчика */
    u32 offset_high;   /* биты 32-63 адреса обработчика */
    u32 zero;          /* зарезервировано, должно быть 0 */
} idt_entry_t;

typedef struct __attribute__((packed)) {
    u16 limit;
    u64 base;
} idt_ptr_t;

#define IDT_ENTRIES 256

/* Тип шлюза: 64-битный interrupt gate, present, DPL=0.
   Interrupt gate (а не trap gate) автоматически сбрасывает флаг IF при
   входе -- пока мы внутри обработчика, новые прерывания замаскированы
   на уровне CPU (не путать с маскировкой конкретных IRQ на PIC). */
#define IDT_GATE_INTERRUPT 0x8E

/* То же самое, но DPL=3 -- единственный гейт, которому это нужно,
   это вектор 0x80 (см. kernel/syscall.c). Без DPL=3 инструкция
   `int 0x80`, выполненная из ring 3, сама по себе даёт #GP ещё до
   входа в обработчик: процессор требует CPL <= DPL гейта (Intel SDM
   Vol.3 §6.8.1), а из кольца 0 этот вектор всё равно никто руками не
   дёргает. */
#define IDT_GATE_INTERRUPT_USER 0xEE

/* Регистрирует один дескриптор шлюза. handler -- адрес точки входа
   обработчика (обычно из isr_stub_table, см. isr.h), selector --
   селектор сегмента кода ядра (0x08). */
void idt_set_gate(u8 num, u64 handler, u16 selector, u8 type_attr);

/* Заполняет все 256 дескрипторов (0-31 исключения + 32-47 IRQ через
   isr_stub_table) и выполняет lidt. Должна вызываться один раз при
   старте ядра (BSP), после чего можно включать прерывания (sti). */
void idt_init(void);

/* SMP: IDT -- ровно одна общая таблица на всю машину (в отличие от GDT/
   TSS, см. include/gdt.h -- у неё нет ничего персонального для
   конкретного ядра, все обработчики одни и те же независимо от того,
   какое физическое ядро их вызвало). Поэтому каждому AP не нужно
   строить СВОЮ IDT заново -- достаточно выполнить lidt на уже
   заполненную BSP-ом таблицу (g_idt/g_idt_ptr в kernel/idt.c, всё ещё
   доступны из любого ядра благодаря identity-map, см. общий комментарий
   в include/pmm.h). Вызывается один раз из ap_entry64() (kernel/smp.c),
   уже ПОСЛЕ того, как idt_init() отработал на BSP. */
void idt_load_current(void);

#endif
