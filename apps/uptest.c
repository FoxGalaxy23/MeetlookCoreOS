/* uptest.c -- проверка пользовательских syscall'ов Фазы 5, подшага 5.3
 * (SYS_USERS_WHOAMI/LIST/AUTHENTICATE/PASSWD/ADD/DEL/SU, см.
 * include/sysabi.h). Питание (SYS_POWER_REBOOT/SHUTDOWN) намеренно НЕ
 * проверяется положительным сценарием здесь -- это реально перезагрузит/
 * выключит машину; см. rootfs/containers/powertest-noperms.manifest за
 * отрицательной проверкой (без него нельзя убедиться, что проверка прав
 * вообще стоит на месте, не разрушая при этом тестовый прогон). Требует
 * caps=users (см. rootfs/containers/uptest.manifest) и должен запускаться
 * из-под root (см. общий комментарий у CAP_USERS, include/container.h --
 * useradd/userdel/чужой passwd дополнительно требуют этого). */

#include "mlapp.h"
#include "libc.h"

APP_ENTRY s32 app_main(const kapi_t *kapi, u32 argc, char **argv) {
    (void)kapi; (void)argc; (void)argv;

    sys_user_info_t me;
    if (sys_users_whoami(&me) < 0) {
        printf("uptest: FAIL sys_users_whoami\n");
        sys_exit(1);
    }
    printf("uptest: whoami -> uid=%u name='%s' is_root=%u\n", me.uid, me.name, me.is_root);
    if (!me.is_root) {
        printf("uptest: FAIL expected to run as root (log in as root first)\n");
        sys_exit(1);
    }

    sys_user_info_t users[16];
    u32 count_before = 0;
    if (sys_users_list(users, 16, &count_before) < 0) {
        printf("uptest: FAIL sys_users_list (before)\n");
        sys_exit(1);
    }
    printf("uptest: %u user(s) before:\n", count_before);
    for (u32 i = 0; i < count_before; i++) {
        printf("uptest:   uid=%u name='%s' is_root=%u\n", users[i].uid, users[i].name, users[i].is_root);
    }

    if (sys_users_authenticate("root", "root") < 0) {
        printf("uptest: FAIL sys_users_authenticate(root, root) should succeed\n");
        sys_exit(1);
    }
    if (sys_users_authenticate("root", "definitely-wrong") >= 0) {
        printf("uptest: FAIL sys_users_authenticate(root, wrong) should fail\n");
        sys_exit(1);
    }
    printf("uptest: sys_users_authenticate OK (correct accepted, wrong rejected)\n");

    if (sys_users_add("uptest_user", "uptest_pass", FALSE) < 0) {
        printf("uptest: FAIL sys_users_add\n");
        sys_exit(1);
    }

    u32 count_after_add = 0;
    if (sys_users_list(users, 16, &count_after_add) < 0 || count_after_add != count_before + 1) {
        printf("uptest: FAIL user count after add: got %u, expected %u\n", count_after_add, count_before + 1);
        sys_exit(1);
    }
    printf("uptest: sys_users_add OK, count now %u\n", count_after_add);

    /* root меняет пароль ЧУЖОГО пользователя -- current_pass игнорируется, передаём NULL */
    if (sys_users_passwd("uptest_user", 0, "uptest_newpass") < 0) {
        printf("uptest: FAIL sys_users_passwd (root changing another user's password)\n");
        sys_exit(1);
    }
    if (sys_users_authenticate("uptest_user", "uptest_newpass") < 0) {
        printf("uptest: FAIL new password does not work\n");
        sys_exit(1);
    }
    if (sys_users_authenticate("uptest_user", "uptest_pass") >= 0) {
        printf("uptest: FAIL old password still works after change\n");
        sys_exit(1);
    }
    printf("uptest: sys_users_passwd OK\n");

    if (sys_users_del("uptest_user") < 0) {
        printf("uptest: FAIL sys_users_del\n");
        sys_exit(1);
    }
    u32 count_after_del = 0;
    if (sys_users_list(users, 16, &count_after_del) < 0 || count_after_del != count_before) {
        printf("uptest: FAIL user count after del: got %u, expected %u\n", count_after_del, count_before);
        sys_exit(1);
    }
    printf("uptest: sys_users_del OK, count back to %u\n", count_after_del);

    /* su: root -> user (без пароля, как обычный su) -> обратно в root (с паролем, т.к. 'user' не root) */
    if (sys_users_su("user", 0) < 0) {
        printf("uptest: FAIL sys_users_su(user) as root should not need a password\n");
        sys_exit(1);
    }
    sys_user_info_t after_su;
    if (sys_users_whoami(&after_su) < 0 || strcmp(after_su.name, "user") != 0 || after_su.is_root) {
        printf("uptest: FAIL whoami after su(user) unexpected\n");
        sys_exit(1);
    }
    printf("uptest: sys_users_su(user) OK, now '%s'\n", after_su.name);

    if (sys_users_su("root", "root") < 0) {
        printf("uptest: FAIL sys_users_su(root, root) to switch back\n");
        sys_exit(1);
    }
    sys_user_info_t back_to_root;
    if (sys_users_whoami(&back_to_root) < 0 || !back_to_root.is_root) {
        printf("uptest: FAIL not back to root after su\n");
        sys_exit(1);
    }
    printf("uptest: sys_users_su(root) OK, back to root\n");

    printf("uptest: OK\n");
    sys_exit(0);
    return 0;
}
