/* spawner.c -- демонстрация Фазы 3 (см. CONTAINER_ARCHITECTURE.md):
 * контейнер, который сам порождает дочерний контейнер по манифесту --
 * через sys_container_spawn() (include/usyscall.h), а не 'runc' в
 * shell (у которого, как и у ядра в целом, доверия не меньше, чем у
 * root'а самой ОС).
 *
 * Нужен CAP_SPAWN (см. запуск через rootfs/containers/spawner-net.manifest
 * или rootfs/containers/spawner-restricted.manifest). Дочерний манифест
 * не может попросить прав больше, чем есть у САМОГО spawner -- ядро это
 * проверяет в sys_container_spawn() (kernel/syscall.c) до запуска ребёнка.
 *
 * Использование: spawner <дочерний манифест> [args...]
 *   run     /bin/spawner ...                -- нет CAP_SPAWN -> SYSCALL_ERR_PERM сразу
 *   runc /containers/spawner-net.manifest /containers/curl-net.manifest google.com /
 *                                            -- spawner имеет spawn+net, curl-net просит net -> подмножество, разрешено
 *   runc /containers/spawner-restricted.manifest /containers/curl-net.manifest google.com /
 *                                            -- spawner имеет только spawn (без net), curl-net просит net -> отказ */

#include "mlapp.h"
#include "libc.h"

APP_ENTRY s32 app_main(const kapi_t *kapi, u32 argc, char **argv) {
    (void)kapi;

    if (argc < 2) {
        printf("Usage: spawner <manifest> [args...]\n");
        sys_exit(1);
    }

    printf("spawner: requesting container spawn from manifest '%s'\n", argv[1]);

    s32 rc = sys_container_spawn(argv[1], argc - 2, &argv[2]);
    if (rc < 0) {
        printf("spawner: sys_container_spawn failed, rc=%d "
               "(SYSCALL_ERR_PERM=-5 -- либо своего CAP_SPAWN нет, либо ребёнок просит caps шире родительских)\n", rc);
        sys_exit(1);
    }

    printf("spawner: child container exited with code %d\n", rc);
    sys_exit(rc);
    return 0;
}
