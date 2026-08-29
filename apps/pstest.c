/* pstest.c -- проверка SYS_PS_LIST/SYS_SLEEP (Фаза 5, подшаг 5.2, см.
 * CONTAINER_ARCHITECTURE.md): список задач планировщика и сон на N мс.
 * Требует caps=ps (см. rootfs/containers/pstest.manifest). */

#include "mlapp.h"
#include "libc.h"

APP_ENTRY s32 app_main(const kapi_t *kapi, u32 argc, char **argv) {
    (void)kapi; (void)argc; (void)argv;

    sys_task_info_t tasks[32];
    u32 count = 0;
    if (sys_ps_list(tasks, 32, &count) < 0) {
        printf("pstest: FAIL sys_ps_list\n");
        sys_exit(1);
    }

    printf("pstest: %u task(s) in the system:\n", count);
    u32 shown = count < 32 ? count : 32;
    bool_t found_self = FALSE;
    for (u32 i = 0; i < shown; i++) {
        printf("pstest:   #%u '%s' state=%u\n", tasks[i].id, tasks[i].name, tasks[i].state);
        if (strcmp(tasks[i].name, "app") == 0) found_self = TRUE;
    }

    if (!found_self) {
        printf("pstest: FAIL did not find myself ('app') in the list\n");
        sys_exit(1);
    }

    printf("pstest: sleeping 50ms...\n");
    sys_sleep(50);
    printf("pstest: woke up, OK\n");

    sys_exit(0);
    return 0;
}
