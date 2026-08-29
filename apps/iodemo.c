/* iodemo.c -- демонстрация Фазы 4, подшаг 1 (см. CONTAINER_ARCHITECTURE.md):
 * НАСТОЯЩИЙ прямой доступ к портам ввода-вывода из ring 3, без единого
 * syscall'а на сам доступ -- в отличие от всего остального в этой ОС
 * (сеть, fs, shm), где ring 3 всегда просит ядро сделать что-то за
 * него. Здесь после того, как манифест выдал CAP_IO с диапазоном
 * портов (rootfs/containers/iodemo-cmos.manifest: io_base=0x70,
 * io_len=2), обычные inb()/outb() (include/io.h -- тот же заголовок,
 * которым пользуются драйверы В ЯДРЕ) просто РАБОТАЮТ прямо из
 * пользовательского кода: разрешение на уровне TSS.io_bitmap
 * (gdt_apply_io_permissions(), include/gdt.h), проверяемое процессором
 * аппаратно на каждой in/out, а не программной прослойкой ядра.
 *
 * Читает регистр секунд часов реального времени через порты CMOS
 * (0x70 -- индекс регистра, 0x71 -- данные, см. drivers/rtc.c за тем
 * же протоколом, но из ring 0) -- выбран как самый безобидный из
 * доступных портов для демонстрации: только читает время, не может
 * ничего сломать, даже если запустить с более широким диапазоном по
 * ошибке в манифесте.
 *
 * Запуск БЕЗ CAP_IO (например, run /bin/iodemo вместо runc) даст #GP
 * на первой же попытке inb()/outb() -- ring 3 без разрешения в
 * TSS.io_bitmap получает исключение процессора немедленно, до того,
 * как ядро вообще успевает что-то проверить программно. kernel/isr.c
 * ловит это как "sandboxed program crashed" и завершает ТОЛЬКО эту
 * задачу (process_exit()), не всю систему -- попробуйте сами, это и
 * есть ожидаемое поведение, а не баг. */

#include "mlapp.h"
#include "libc.h"
#include "io.h"

#define CMOS_INDEX 0x70
#define CMOS_DATA  0x71
#define CMOS_REG_SECONDS 0x00

APP_ENTRY s32 app_main(const kapi_t *kapi, u32 argc, char **argv) {
    (void)kapi; (void)argc; (void)argv;

    printf("iodemo: reading CMOS seconds register directly from ring 3 (ports 0x70/0x71)\n");

    outb(CMOS_INDEX, CMOS_REG_SECONDS);
    u8 bcd = inb(CMOS_DATA);

    /* Регистр в формате BCD (см. drivers/rtc.c) -- переводим для читаемости. */
    u8 seconds = (u8)(((bcd >> 4) * 10) + (bcd & 0x0F));

    printf("iodemo: RTC seconds register = %u (raw BCD byte read directly via inb())\n", seconds);
    printf("iodemo: ring-3 port I/O works -- CAP_IO/TSS.io_bitmap granted access as expected\n");

    sys_exit(0);
    return 0;
}
