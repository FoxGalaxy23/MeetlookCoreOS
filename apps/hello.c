/* hello.c -- минимальный пример sandboxed (ring 3) пользовательского
 * бинарника: работает ЧЕРЕЗ СИСТЕМНЫЕ ВЫЗОВЫ (include/sysabi.h,
 * include/usyscall.h), а не через прямые указатели на функции ядра
 * (kapi_t), как apps/calc.c. calc.c остался на старом ring0-режиме и
 * запускается только через 'rungod' (см. include/kapi.h,
 * kernel/loader.c) -- в ring 3 попытка вызвать функцию ядра через
 * kapi-указатель немедленно дала бы #PF: страницы кода ядра не
 * помечены user-accessible (см. kernel/paging.c).
 *
 * Собирается и запускается точно так же, как любой другой бинарник
 * (apps/user.ld, команда 'run hello' в шелле), но по умолчанию
 * исполняется в ring 3: попытка прочитать/записать что-то за
 * пределами собственной памяти этой программы убьёт только её саму,
 * а не всю систему (см. kernel/isr.c).
 *
 * Использует include/libc.h (printf) вместо ручной печати цифр --
 * libc_init() здесь не вызывается: без неё printf()/getchar()/readline()
 * сами по себе работают через системные вызовы, что для ring3-режима
 * (запуска через 'run', как здесь) -- ровно то, что нужно. См. общий
 * комментарий в libc.h. */

#include "mlapp.h"
#include "libc.h"

APP_ENTRY s32 app_main(const kapi_t *kapi, u32 argc, char **argv) {
    (void)kapi; /* в ring 3 kapi всегда NULL -- см. kernel/process.c */

    printf("Hello from ring 3! This process runs in user mode.\n");

    if (argc > 1) {
        printf("Arguments:");
        for (u32 i = 1; i < argc; i++) printf(" %s", argv[i]);
        printf("\n");
    }

    sys_meminfo_t info;
    if (sys_meminfo(&info) == 0) {
        printf("Free physical memory (read via syscall): %u KB\n",
               info.mem_free_bytes / 1024);
    }

    printf("Own heap (libc malloc, независимая от кучи ядра): %u/%u bytes used\n",
           heap_used_bytes(), heap_total_bytes());

    sys_exit(0);
    return 0; /* не достигается: sys_exit() не возвращается */
}
