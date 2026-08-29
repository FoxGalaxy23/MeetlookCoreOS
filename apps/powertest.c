/* powertest.c -- проверка SYS_POWER_REBOOT (Фаза 5, подшаг 5.3, см.
 * CONTAINER_ARCHITECTURE.md). Печатает попытку и вызывает
 * sys_power_reboot() напрямую -- с caps=none это должно вернуть
 * SYSCALL_ERR_PERM и напечатать "FAIL as expected" (см.
 * rootfs/containers/powertest-noperms.manifest); с caps=power и текущим
 * пользователем root (rootfs/containers/powertest.manifest) машина
 * реально перезагрузится -- этот вызов НЕ возвращается в случае
 * успеха. */

#include "mlapp.h"
#include "libc.h"

APP_ENTRY s32 app_main(const kapi_t *kapi, u32 argc, char **argv) {
    (void)kapi; (void)argc; (void)argv;

    printf("powertest: calling sys_power_reboot()...\n");
    s32 rc = sys_power_reboot();
    printf("powertest: sys_power_reboot() returned %d (FAIL as expected if caps=none or not root)\n", rc);
    sys_exit(rc < 0 ? 1 : 0);
    return 0;
}
