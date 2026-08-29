#ifndef LIBC_H
#define LIBC_H

#include <stdarg.h>
#include "types.h"
#include "string.h"
#include "usyscall.h"
#include "mlapp.h"

/* libc.h -- маленькая libc для пользовательских бинарников MeetlookCoreOS.
 *
 * До этого файла у каждого бинарника не было ни форматированного
 * вывода (kernel/kprintf.c есть только у САМОГО ядра и доступен ring0-
 * приложениям лишь как один указатель kapi->kprintf, а ring3-приложения
 * не видят его вообще -- см. include/kapi.h), ни malloc/free в ring 3
 * (kapi_t, где лежит kmalloc/kfree, в sandboxed-режиме всегда NULL, см.
 * include/mlapp.h) -- отсюда ручная печать цифр в apps/hello.c и
 * apps/forktest.c, и то, что ни один ring3-бинарник просто не мог
 * ничего выделить динамически.
 *
 * Два режима запуска бинарника (см. include/loader.h) требуют разного
 * способа "написать текст на экран и прочитать строку с клавиатуры":
 *   - ring 0 ('rungod'): через таблицу kapi_t (указатель приходит
 *     аргументом в app_main());
 *   - ring 3 ('run', режим по умолчанию): через системные вызовы
 *     (int 0x80, include/usyscall.h) -- kapi_t в этом режиме всегда NULL.
 *
 * Эта libc сама решает, каким способом пользоваться: по умолчанию (без
 * вызова libc_init()) используются системные вызовы -- то есть ring3-
 * поведение, которое требуется чаще всего (обычный 'run', см. общий
 * комментарий в apps/hello.c). Если бинарник запускается через 'rungod'
 * и хочет, чтобы printf()/readline() шли через настоящий kapi->kprintf/
 * console_readline (а не проваливались с ошибкой, см. комментарий в
 * include/uaccess.h -- у god-режима нет task->as, поэтому int 0x80 там
 * ничего не может сделать), нужно вызвать libc_init(kapi) ПЕРВОЙ строкой
 * app_main(), передав туда как раз тот kapi_t, что пришёл аргументом:
 *
 *     APP_ENTRY s32 app_main(const kapi_t *kapi, u32 argc, char **argv) {
 *         libc_init(kapi);
 *         printf("free heap: %u bytes\n", heap_free_bytes());
 *         ...
 *     }
 *
 * malloc/free/calloc/realloc работают ОДИНАКОВО в обоих режимах и не
 * требуют libc_init(): это собственная, независимая от ядра куча --
 * простой аллокатор первого подходящего блока (first-fit, тот же
 * принцип, что и у kernel/kheap.c, но целиком в памяти самого
 * бинарника, см. lib/libc.c) поверх статической арены фиксированного
 * размера (LIBC_HEAP_SIZE, по умолчанию 256 KiB -- определите свой
 * макрос ДО #include "libc.h", если этого мало или, наоборот, слишком
 * много для бюджета APP_MAX_SIZE = 2 MiB, см. include/loader.h). Она
 * никак не связана с общей кучей ядра (kapi->kmalloc) -- у каждого
 * бинарника, таким образом, полностью частная куча, независимо от
 * режима запуска, и она автоматически освобождается целиком, когда
 * процесс завершается (это просто часть его .bss). */

#ifndef LIBC_HEAP_SIZE
#define LIBC_HEAP_SIZE (256u * 1024u)
#endif

/* Переключает printf()/putchar()/puts()/readline()/getchar_() на вывод/
   ввод через kapi_t вместо системных вызовов -- нужно ТОЛЬКО в 'rungod'
   (ring 0). kapi == NULL (в т.ч. если не вызывать libc_init() вовсе)
   означает "использовать системные вызовы" -- обычный ring3-режим. */
void libc_init(const kapi_t *kapi);

/* ---- Форматированный вывод ----
   Набор спецификаторов совпадает с kernel/kprintf.c, чтобы поведение не
   отличалось от того, что уже видно на экране до запуска любого
   бинарника: %s %c %% и %d/%u/%x с необязательным модификатором длины
   l/ll (то есть %ld, %lu, %llx и т.п. читают 64-битный аргумент). Ни
   ширины поля, ни точности, ни флагов вроде '0'/'-' НЕ поддерживается --
   этого с запасом хватает для консольного вывода командной строки, а
   более сложный форматтер (glibc-совместимый) для этой ОС избыточен. */
s32 vsnprintf(char *buf, u32 size, const char *fmt, va_list args);
s32 snprintf(char *buf, u32 size, const char *fmt, ...);
s32 vprintf(const char *fmt, va_list args);
s32 printf(const char *fmt, ...);

/* ---- Консольный ввод-вывод (см. libc_init() выше) ---- */
void putchar(char c);
void puts(const char *s);          /* добавляет '\n' в конце, как и puts(3) */
s32  getchar(void);                /* блокирующее чтение одного символа */
s32  readline(char *buf, u32 maxlen); /* НЕ GNU readline -- простой аналог
    sys_readline()/kapi->console_readline() под общим именем; длина строки
    (без '\0') либо <0 */

/* ---- Динамическая память (см. общий комментарий выше) ---- */
void *malloc(u32 size);
void *calloc(u32 nmemb, u32 size);
void *realloc(void *ptr, u32 new_size);
void  free(void *ptr);

u32 heap_total_bytes(void);
u32 heap_used_bytes(void);
u32 heap_free_bytes(void);

#endif
