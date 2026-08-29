#include "mlapp.h"
#include "usyscall.h"
#include "string.h"

APP_ENTRY s32 app_main(const kapi_t *kapi, u32 argc, char **argv) {
    (void)kapi;
    (void)argc;
    (void)argv;

    static const char msg[] = "exectest: calling sys_execve(\"/bin/hello\", argc=3, ...)\n";
    sys_write(msg, sizeof(msg) - 1);

    char *exec_argv[4];
    exec_argv[0] = "/bin/hello";
    exec_argv[1] = "exec_arg1";
    exec_argv[2] = "exec_arg2";
    exec_argv[3] = 0;

    sys_execve("/bin/hello", 3, exec_argv);

    static const char fail[] = "exectest: sys_execve returned (error)!\n";
    sys_write(fail, sizeof(fail) - 1);

    sys_exit(-1);
    return -1;
}
