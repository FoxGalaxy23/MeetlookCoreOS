/* forktest.c -- пример настоящего fork(): один процесс, две независимые
 * (после этого момента) задачи с собственными адресными пространствами
 * (см. include/paging.h, kernel/process.c). Пока никто не запишет в
 * общую страницу -- физическая память общая (copy-on-write); первая же
 * запись в глобальную переменную ниже (или в собственный стек) заводит
 * потомку и родителю раздельные копии этой страницы -- отсюда и разные
 * значения counter, которые каждый из них увидит после fork(). */

#include "mlapp.h"
#include "usyscall.h"

static s32 g_counter = 100; /* лежит в .data -- общая страница до первой записи */

APP_ENTRY s32 app_main(const kapi_t *kapi, u32 argc, char **argv) {
    (void)kapi; (void)argc; (void)argv;

    static const char before[] = "forktest: before fork(), counter=100\n";
    sys_write(before, sizeof(before) - 1);

    s32 pid = sys_fork();

    if (pid < 0) {
        static const char fail[] = "forktest: fork() failed (out of memory?)\n";
        sys_write(fail, sizeof(fail) - 1);
        sys_exit(-1);
    }

    /* С этого момента родитель и потомок -- РАЗНЫЕ задачи планировщика,
       у каждой свой counter (общая физическая страница до сих пор одна
       на двоих, пока кто-то из них её не изменит -- см. as_clone_cow()
       в kernel/paging.c). */
    if (pid == 0) {
        g_counter += 1; /* эта запись -- та самая, что провоцирует COW: только у потомка */
        static const char child[] = "forktest: child, counter=";
        sys_write(child, sizeof(child) - 1);
    } else {
        g_counter += 100; /* и эта -- только у родителя, независимо от потомка */
        static const char parent[] = "forktest: parent, counter=";
        sys_write(parent, sizeof(parent) - 1);
    }

    char digits[8]; u32 n = 0;
    s32 v = g_counter;
    if (v == 0) digits[n++] = '0';
    while (v > 0) { digits[n++] = (char)('0' + (v % 10)); v /= 10; }
    while (n > 0) sys_write(&digits[--n], 1);
    sys_write("\n", 1);

    sys_exit(0);
    return 0; /* не достигается */
}
