/* fstest.c -- проверка файловых syscall'ов Фазы 5, подшага 5.1
 * (SYS_FS_READ_FILE/SYS_FS_STAT/SYS_FS_PWD/SYS_FS_CD/SYS_FS_MKDIR/
 * SYS_FS_RM, см. include/sysabi.h) -- полный цикл mkdir->cd->write->
 * stat->read->cd .. ->rm, каждый шаг с проверкой кода возврата. Как
 * forktest.c/ipctest.c, в первую очередь тест: печатает либо "OK", либо
 * конкретный шаг, на котором поведение разошлось с ожидаемым. Требует
 * caps=fs_read,fs_write (см. rootfs/containers/fstest.manifest). */

#include "mlapp.h"
#include "libc.h"

APP_ENTRY s32 app_main(const kapi_t *kapi, u32 argc, char **argv) {
    (void)kapi; (void)argc; (void)argv;

    char pwd_before[128];
    if (sys_fs_pwd(pwd_before, sizeof(pwd_before)) < 0) {
        printf("fstest: FAIL sys_fs_pwd (before)\n");
        sys_exit(1);
    }
    printf("fstest: starting in '%s'\n", pwd_before);

    if (sys_fs_mkdir("fstest_dir") < 0) {
        printf("fstest: FAIL sys_fs_mkdir\n");
        sys_exit(1);
    }

    if (sys_fs_cd("fstest_dir") < 0) {
        printf("fstest: FAIL sys_fs_cd(fstest_dir)\n");
        sys_exit(1);
    }

    sys_fs_dirent_t entries[8];
    u32 entry_count = 0;
    if (sys_fs_list(entries, 8, &entry_count) < 0) {
        printf("fstest: FAIL sys_fs_list (empty dir)\n");
        sys_exit(1);
    }
    if (entry_count != 0) {
        printf("fstest: FAIL sys_fs_list expected empty dir, got %u entries\n", entry_count);
        sys_exit(1);
    }
    printf("fstest: sys_fs_list on fresh dir -> %u entries, as expected\n", entry_count);

    char pwd_inside[128];
    if (sys_fs_pwd(pwd_inside, sizeof(pwd_inside)) < 0) {
        printf("fstest: FAIL sys_fs_pwd (inside)\n");
        sys_exit(1);
    }
    printf("fstest: now in '%s'\n", pwd_inside);

    const char *msg = "Hello from fstest!\n";
    u32 msg_len = strlen(msg);
    if (sys_fs_write_file("hello.txt", msg, msg_len) < 0) {
        printf("fstest: FAIL sys_fs_write_file\n");
        sys_exit(1);
    }

    sys_fs_stat_t st;
    if (sys_fs_stat("hello.txt", &st) < 0) {
        printf("fstest: FAIL sys_fs_stat\n");
        sys_exit(1);
    }
    printf("fstest: hello.txt owner_uid=%u mode=0x%x\n", st.owner_uid, st.mode);

    char buf[128];
    u32 read_size = 0;
    if (sys_fs_read_file("hello.txt", buf, sizeof(buf) - 1, &read_size) < 0) {
        printf("fstest: FAIL sys_fs_read_file\n");
        sys_exit(1);
    }
    buf[read_size] = '\0';

    if (read_size != msg_len || strcmp(buf, msg) != 0) {
        printf("fstest: FAIL content mismatch (got %u bytes: '%s')\n", read_size, buf);
        sys_exit(1);
    }
    printf("fstest: read back %u bytes, content matches\n", read_size);

    if (sys_fs_list(entries, 8, &entry_count) < 0) {
        printf("fstest: FAIL sys_fs_list (after write)\n");
        sys_exit(1);
    }
    if (entry_count != 1 || strcmp(entries[0].name, "hello.txt") != 0 || entries[0].is_dir) {
        printf("fstest: FAIL sys_fs_list expected 1 file 'hello.txt', got %u entries ('%s')\n",
               entry_count, entry_count > 0 ? entries[0].name : "?");
        sys_exit(1);
    }
    printf("fstest: sys_fs_list sees hello.txt, size=%u\n", entries[0].size);

    if (sys_fs_rm("hello.txt") < 0) {
        printf("fstest: FAIL sys_fs_rm(hello.txt)\n");
        sys_exit(1);
    }

    if (sys_fs_cd("..") < 0) {
        printf("fstest: FAIL sys_fs_cd(..)\n");
        sys_exit(1);
    }

    if (sys_fs_rm("fstest_dir") < 0) {
        printf("fstest: FAIL sys_fs_rm(fstest_dir)\n");
        sys_exit(1);
    }

    char pwd_after[128];
    if (sys_fs_pwd(pwd_after, sizeof(pwd_after)) < 0 || strcmp(pwd_after, pwd_before) != 0) {
        printf("fstest: FAIL pwd mismatch after cleanup ('%s' vs '%s')\n", pwd_after, pwd_before);
        sys_exit(1);
    }

    printf("fstest: OK\n");
    sys_exit(0);
    return 0;
}
