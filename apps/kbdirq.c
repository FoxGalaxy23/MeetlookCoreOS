/* kbdirq.c -- демонстрация Фазы 4, подшаг 2 (см. CONTAINER_ARCHITECTURE.md):
 * НАСТОЯЩЕЕ ожидание аппаратного прерывания из ring 3, через
 * SYS_IRQ_WAIT (include/usyscall.h), в паре с CAP_IO из подшага 1 для
 * самого чтения регистров устройства -- ровно та комбинация, которая
 * понадобится будущему сетевому драйверу-контейнеру (Фаза 4, подшаг 4):
 * "разбудили по прерыванию -> сам прочитал/записал регистры устройства".
 *
 * Использует линию IRQ1 (клавиатура, PS/2) -- выбрана потому, что
 * drivers/keyboard.c в этой ОС ОПРАШИВАЕТ порт статуса (см. его же
 * исходник), а не подписывается на IRQ1 через irq_register_handler() --
 * то есть линия физически свободна для моста (kernel/irqbridge.c) без
 * конфликта с уже существующим драйвером клавиатуры (в отличие от IRQ0,
 * которую держит таймер, или линии сетевой карты, которую держит
 * drivers/rtl8139.c).
 *
 * Запуск -- rootfs/containers/kbdirq.manifest (caps=io,irq; io=[0x60,0x65);
 * irq=1): нажмите несколько клавиш после запуска, приложение напечатает
 * их скан-коды и завершится после NUM_EVENTS штук. */

#include "mlapp.h"
#include "libc.h"
#include "io.h"

#define KBD_DATA_PORT   0x60
#define KBD_STATUS_PORT 0x64
#define KBD_IRQ_LINE    1
#define NUM_EVENTS      5

APP_ENTRY s32 app_main(const kapi_t *kapi, u32 argc, char **argv) {
    (void)kapi; (void)argc; (void)argv;

    printf("kbdirq: waiting for keyboard IRQ (line %u) -- press a few keys...\n", (u32)KBD_IRQ_LINE);

    for (u32 i = 0; i < NUM_EVENTS; i++) {
        s32 rc = sys_irq_wait(KBD_IRQ_LINE);
        if (rc < 0) {
            printf("kbdirq: sys_irq_wait failed, rc=%d "
                   "(SYSCALL_ERR_PERM=-5 -- нет CAP_IRQ, irq в манифесте не 1, "
                   "либо линию уже занял драйвер ядра)\n", rc);
            sys_exit(1);
        }

        /* Проснулись -- прерывание УЖЕ случилось, теперь читаем регистры
           устройства сами, напрямую (CAP_IO из подшага 1, тот же
           протокол, что и в kernel/drivers/keyboard.c, но из ring 3). */
        if ((inb(KBD_STATUS_PORT) & 1) != 0) {
            u8 sc = inb(KBD_DATA_PORT);
            printf("kbdirq: event %u/%u -- scancode 0x%x (%s)\n",
                   i + 1, (u32)NUM_EVENTS, (u32)sc, (sc & 0x80) ? "release" : "press");
        } else {
            printf("kbdirq: event %u/%u -- IRQ fired but output buffer already empty "
                   "(кто-то ещё успел прочитать байт первым)\n", i + 1, (u32)NUM_EVENTS);
        }
    }

    printf("kbdirq: done -- ring-3 IRQ wait + direct port I/O both worked as expected\n");
    sys_exit(0);
    return 0;
}
