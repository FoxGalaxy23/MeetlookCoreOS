/* pic.c -- см. include/pic.h за общим описанием. */

#include "pic.h"
#include "io.h"

#define ICW1_ICW4      0x01  /* дальше следует ICW4 */
#define ICW1_INIT      0x10  /* команда инициализации */
#define ICW4_8086      0x01  /* режим 8086/88 (а не старый 8085) */

void pic_remap(u8 offset1, u8 offset2) {
    u8 mask1 = inb(PIC1_DATA); /* сохраняем текущие маски -- восстановим после ремапа */
    u8 mask2 = inb(PIC2_DATA);

    outb(PIC1_COMMAND, ICW1_INIT | ICW1_ICW4);
    io_wait();
    outb(PIC2_COMMAND, ICW1_INIT | ICW1_ICW4);
    io_wait();

    outb(PIC1_DATA, offset1);      /* ICW2: базовый вектор ведущего PIC */
    io_wait();
    outb(PIC2_DATA, offset2);      /* ICW2: базовый вектор ведомого PIC */
    io_wait();

    outb(PIC1_DATA, 4);            /* ICW3: у ведущего на линии IRQ2 сидит ведомый (бит 2) */
    io_wait();
    outb(PIC2_DATA, 2);            /* ICW3: ведомый сообщает свой номер каскадной линии (2) */
    io_wait();

    outb(PIC1_DATA, ICW4_8086);
    io_wait();
    outb(PIC2_DATA, ICW4_8086);
    io_wait();

    /* По умолчанию маскируем всё -- конкретные драйверы (timer.c и т.д.)
       сами размаскируют то, что им нужно, через pic_clear_mask(). */
    outb(PIC1_DATA, 0xFF);
    outb(PIC2_DATA, 0xFF);

    (void)mask1;
    (void)mask2;
}

void pic_send_eoi(u8 irq) {
    if (irq >= 8) {
        outb(PIC2_COMMAND, PIC_EOI);
    }
    outb(PIC1_COMMAND, PIC_EOI);
}

void pic_set_mask(u8 irq) {
    u16 port = (irq < 8) ? PIC1_DATA : PIC2_DATA;
    u8 bit = irq < 8 ? irq : (u8)(irq - 8);
    u8 mask = inb(port);
    outb(port, mask | (u8)(1u << bit));
}

void pic_clear_mask(u8 irq) {
    u16 port = (irq < 8) ? PIC1_DATA : PIC2_DATA;
    u8 bit = irq < 8 ? irq : (u8)(irq - 8);
    u8 mask = inb(port);
    outb(port, mask & (u8)~(1u << bit));

    /* IRQ8-15 физически приходят через каскадную линию IRQ2 ведущего
       PIC -- если она замаскирована, ведомый PIC достучаться до CPU
       не может, независимо от собственных масок. */
    if (irq >= 8) {
        pic_clear_mask(2);
    }
}
