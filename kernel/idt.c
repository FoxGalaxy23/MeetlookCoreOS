/* idt.c -- см. include/idt.h за общим описанием. */

#include "idt.h"
#include "isr.h"
#include "string.h"

static idt_entry_t g_idt[IDT_ENTRIES];
static idt_ptr_t g_idt_ptr;

void idt_set_gate(u8 num, u64 handler, u16 selector, u8 type_attr) {
    g_idt[num].offset_low  = (u16)(handler & 0xFFFF);
    g_idt[num].offset_mid  = (u16)((handler >> 16) & 0xFFFF);
    g_idt[num].offset_high = (u32)((handler >> 32) & 0xFFFFFFFFu);
    g_idt[num].selector    = selector;
    g_idt[num].ist         = 0;
    g_idt[num].type_attr   = type_attr;
    g_idt[num].zero        = 0;
}

void idt_init(void) {
    memset(g_idt, 0, sizeof(g_idt));

    /* Векторы 0-47 (32 исключения + 16 IRQ) указывают на заглушки из
       isr.s; 48-255 остаются нулевыми (present=0) -- случайное
       прерывание по ним вызовет #GP, что честно укажет на баг, а не
       тихо утащит CPU в непонятный код. */
    for (u32 i = 0; i < 48; i++) {
        idt_set_gate((u8)i, (u64)isr_stub_table[i], 0x08, IDT_GATE_INTERRUPT);
    }

    g_idt_ptr.limit = (u16)(sizeof(g_idt) - 1);
    g_idt_ptr.base = (u64)&g_idt;

    __asm__ volatile ("lidt %0" : : "m"(g_idt_ptr));
}

void idt_load_current(void) {
    /* g_idt_ptr уже заполнен BSP-ом внутри idt_init() выше -- просто
       говорим ЭТОМУ (текущему) ядру использовать ту же самую таблицу
       (см. общий комментарий в include/idt.h про то, почему IDT общая
       на всю машину, в отличие от GDT/TSS). */
    __asm__ volatile ("lidt %0" : : "m"(g_idt_ptr));
}
