#ifndef MLAPP_H
#define MLAPP_H

/* mlapp.h -- минимальный SDK для пользовательских бинарников
 * MeetlookCoreOS.
 *
 * Бинарник -- это "плоский" (flat) образ без формата ELF: файл
 * собирается со своим линковочным скриптом (apps/user.ld), который
 * кладёт код по фиксированному виртуальному адресу, и объявляет ровно
 * одну точку входа:
 *
 *     APP_ENTRY s32 app_main(const kapi_t *k, u32 argc, char **argv) {
 *         k->kprintf("hello from userland, but ring 0!\n");
 *         return 0;
 *     }
 *
 * Сигнатура app_main() одна и та же для ОБОИХ режимов запуска (см.
 * include/loader.h): в ring 0 ('rungod', как apps/calc.c) k -- рабочая
 * таблица функций ядра; в ring 3 ('run', по умолчанию, как apps/hello.c)
 * k всегда NULL -- функции ядра из ring 3 напрямую недоступны (страницы
 * их кода не помечены user-accessible, см. kernel/paging.c), нужный
 * сервис ring3-приложение получает только через системные вызовы
 * (include/usyscall.h, "int 0x80").
 *
 * Макрос APP_ENTRY кладёт функцию в отдельную секцию ".text.entry",
 * которую линковочный скрипт помещает в самое начало образа -- это
 * гарантирует, что первые байты файла являются машинным кодом именно
 * этой функции, и загрузчику ядра (kernel/loader.c) не нужен ни формат
 * ELF, ни отдельный ассемблерный crt0: он просто приводит адрес начала
 * загруженного образа к указателю на функцию и вызывает её напрямую как
 * обычную функцию Си (System V AMD64 ABI). */

#include "types.h"
#include "kapi.h"

#define APP_ENTRY __attribute__((section(".text.entry"), used))

#endif
