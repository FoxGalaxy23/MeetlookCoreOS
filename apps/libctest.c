/* libctest.c -- демонстрация include/libc.h: форматированный вывод
 * (printf) и собственная куча процесса (malloc/calloc/realloc/free),
 * независимая от кучи ядра (kapi->kmalloc) и работающая в ring 3 (где
 * kapi_t всегда NULL, а значит kapi->kmalloc в принципе недоступен) --
 * см. общий комментарий в include/libc.h за тем, почему это раньше было
 * невозможно.
 *
 * Как forktest.c/ipctest.c/exectest.c, это в первую очередь тест: в
 * конце печатает либо "OK", либо конкретное место, где поведение кучи
 * разошлось с ожидаемым (что само по себе означало бы баг в
 * lib/libc.c). */

#include "mlapp.h"
#include "libc.h"

APP_ENTRY s32 app_main(const kapi_t *kapi, u32 argc, char **argv) {
    (void)kapi; (void)argc; (void)argv;

    printf("libctest: heap capacity = %u bytes\n", heap_total_bytes());

    /* ---- printf: числа, строки, шестнадцатеричный, длинные (l) ---- */
    printf("printf check: %d %u %x %s %c %%\n", -42, 42u, 0xDEADu, "text", '!');

    /* ---- malloc/free: несколько блоков, потом освобождаем средний --
       после free() средний блок должен слиться с соседями по коалессингу
       (см. heap_coalesce() в lib/libc.c), так что использованная память
       должна вернуться туда же, откуда была. ---- */
    u32 used_before = heap_used_bytes();

    char *a = (char *)malloc(64);
    char *b = (char *)malloc(128);
    char *c = (char *)malloc(64);
    if (!a || !b || !c) {
        printf("libctest: FAIL -- malloc() returned NULL on a fresh heap\n");
        sys_exit(1);
    }

    for (int i = 0; i < 64; i++) a[i] = 'A';
    for (int i = 0; i < 128; i++) b[i] = 'B';
    for (int i = 0; i < 64; i++) c[i] = 'C';
    a[63] = '\0'; b[127] = '\0'; c[63] = '\0';

    if (a[0] != 'A' || b[0] != 'B' || c[0] != 'C') {
        printf("libctest: FAIL -- содержимое блоков перекрылось (аллокатор пересекает блоки)\n");
        sys_exit(1);
    }

    free(b);
    free(a);
    free(c);

    u32 used_after = heap_used_bytes();
    if (used_after != used_before) {
        printf("libctest: FAIL -- после free() всех блоков занято %u байт, ожидалось %u (утечка либо неверный коалессинг)\n",
               used_after, used_before);
        sys_exit(1);
    }

    /* ---- realloc: рост данных с сохранением содержимого ---- */
    char *r = (char *)malloc(16);
    for (int i = 0; i < 15; i++) r[i] = (char)('a' + i);
    r[15] = '\0';
    r = (char *)realloc(r, 256);
    if (!r || strcmp(r, "abcdefghijklmno") != 0) {
        printf("libctest: FAIL -- realloc() не сохранил старое содержимое\n");
        sys_exit(1);
    }
    free(r);

    /* ---- calloc: обязан обнулять память ---- */
    u32 *nums = (u32 *)calloc(16, sizeof(u32));
    bool_t all_zero = TRUE;
    for (int i = 0; i < 16 && all_zero; i++) if (nums[i] != 0) all_zero = FALSE;
    if (!all_zero) {
        printf("libctest: FAIL -- calloc() не обнулил память\n");
        sys_exit(1);
    }
    free(nums);

    printf("libctest: heap after test = %u/%u bytes used (should equal initial %u)\n",
           heap_used_bytes(), heap_total_bytes(), used_before);
    printf("libctest: OK\n");

    sys_exit(0);
    return 0; /* не достигается */
}
