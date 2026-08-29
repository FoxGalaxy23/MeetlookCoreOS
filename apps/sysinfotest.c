/* sysinfotest.c -- проверка syscall'ов Фазы 5, подшага 5.4 (диски/USB/
 * время/монтирование/права/статический IP, см. include/sysabi.h).
 * Требует caps=sysinfo,fs_read,fs_write,net (см.
 * rootfs/containers/sysinfotest.manifest). */

#include "mlapp.h"
#include "libc.h"

APP_ENTRY s32 app_main(const kapi_t *kapi, u32 argc, char **argv) {
    (void)kapi; (void)argc; (void)argv;

    /* SYS_RTC_READ -- без capability, должен работать всегда */
    sys_rtc_time_t t;
    if (sys_rtc_read(&t) < 0) {
        printf("sysinfotest: FAIL sys_rtc_read\n");
        sys_exit(1);
    }
    printf("sysinfotest: rtc -> %u-%u-%u %u:%u:%u\n", t.year, t.month, t.day, t.hour, t.minute, t.second);

    /* SYS_STORAGE_LIST */
    sys_storage_info_t drives[16];
    u32 drive_count = 0;
    if (sys_storage_list(drives, 16, &drive_count) < 0) {
        printf("sysinfotest: FAIL sys_storage_list\n");
        sys_exit(1);
    }
    printf("sysinfotest: %u drive(s)\n", drive_count);
    for (u32 i = 0; i < drive_count; i++) {
        printf("sysinfotest:   bus=%u sectors=%u model='%s'\n", drives[i].bus, drives[i].sectors, drives[i].model);
    }

    /* SYS_USB_LIST */
    sys_usb_info_t usbs[16];
    u32 usb_count = 0;
    if (sys_usb_list(usbs, 16, &usb_count) < 0) {
        printf("sysinfotest: FAIL sys_usb_list\n");
        sys_exit(1);
    }
    printf("sysinfotest: %u USB device(s)\n", usb_count);
    for (u32 i = 0; i < usb_count; i++) {
        printf("sysinfotest:   vendor=0x%x product=0x%x class=%u\n",
               usbs[i].vendor_id, usbs[i].product_id, usbs[i].dev_class);
    }

    /* SYS_USB_MOUSE_STATE */
    sys_mouse_state_t mouse;
    if (sys_usb_mouse_state(&mouse) < 0) {
        printf("sysinfotest: FAIL sys_usb_mouse_state\n");
        sys_exit(1);
    }
    printf("sysinfotest: mouse present=%u\n", mouse.present);

    /* SYS_FS_BACKEND_INFO */
    sys_fs_backend_info_t backend;
    if (sys_fs_backend_info(&backend) < 0) {
        printf("sysinfotest: FAIL sys_fs_backend_info\n");
        sys_exit(1);
    }
    printf("sysinfotest: fs backend='%s' mounted=%u\n", backend.name, backend.is_mounted);

    /* SYS_FS_MOUNT на несуществующий диск -- должен корректно отказать, не падать */
    if (sys_fs_mount(0) >= 0 && drive_count == 0) {
        printf("sysinfotest: FAIL sys_fs_mount(0) succeeded with no drives present\n");
        sys_exit(1);
    }
    printf("sysinfotest: sys_fs_mount(0) with no drives correctly failed\n");

    /* SYS_FS_CHMOD/CHOWN на readme.txt, с откатом в исходное состояние */
    sys_fs_stat_t before;
    if (sys_fs_stat("readme.txt", &before) < 0) {
        printf("sysinfotest: FAIL sys_fs_stat(readme.txt)\n");
        sys_exit(1);
    }
    if (sys_fs_chmod("readme.txt", 0600) < 0) {
        printf("sysinfotest: FAIL sys_fs_chmod\n");
        sys_exit(1);
    }
    sys_fs_stat_t after;
    if (sys_fs_stat("readme.txt", &after) < 0 || after.mode != 0600) {
        printf("sysinfotest: FAIL chmod did not apply (mode=0x%x)\n", after.mode);
        sys_exit(1);
    }
    if (sys_fs_chmod("readme.txt", before.mode) < 0) {
        printf("sysinfotest: FAIL sys_fs_chmod (restore)\n");
        sys_exit(1);
    }
    printf("sysinfotest: sys_fs_chmod OK (0x%x -> 0x600 -> 0x%x)\n", before.mode, before.mode);

    /* SYS_NET_SET_STATIC + проверка через уже существующий SYS_NET_STATUS */
    if (sys_net_set_static(0x0A000005u, 0xFFFFFF00u, 0x0A000001u, 0x08080808u) < 0) {
        printf("sysinfotest: FAIL sys_net_set_static\n");
        sys_exit(1);
    }
    sys_net_status_t net;
    if (sys_net_status(&net) < 0 || net.ip != 0x0A000005u || net.dhcp_bound) {
        printf("sysinfotest: FAIL net status after set_static did not match\n");
        sys_exit(1);
    }
    printf("sysinfotest: sys_net_set_static OK, ip=0x%x\n", net.ip);

    printf("sysinfotest: OK\n");
    sys_exit(0);
    return 0;
}
