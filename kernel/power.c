#include "power.h"
#include "io.h"
#include "kprintf.h"

void power_shutdown(void) {
    kprintf("Shutting down your system now...\n");

    /* Известные порты ACPI-выключения для эмуляторов (QEMU/Bochs/VirtualBox) */
    outw(0x604, 0x2000);   /* QEMU (новые версии, isa-debug-exit/acpi) */
    outw(0xB004, 0x2000);  /* QEMU старые версии / Bochs */
    outw(0x4004, 0x3400);  /* VirtualBox */

    /* Если ни один порт не сработал (реальное железо без ACPI-костыля) */
    kprintf("You can now power off the machine manually.\n");
    for (;;) {
        __asm__ volatile ("cli; hlt");
    }
}

void power_reboot(void) {
    kprintf("Rebooting...\n");

    u8 good = 0x02;
    while (good & 0x02) good = inb(0x64);
    outb(0x64, 0xFE); /* импульс сброса через контроллер клавиатуры */

    /* Резервный вариант - тройной фолт через загрузку некорректного IDT */
    for (;;) {
        __asm__ volatile ("cli; hlt");
    }
}
