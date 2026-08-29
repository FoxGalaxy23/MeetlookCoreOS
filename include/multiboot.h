#ifndef MULTIBOOT_H
#define MULTIBOOT_H

#include "types.h"

/* multiboot.h -- минимальное описание структуры multiboot_info,
 * которую GRUB передаёт ядру в регистре ebx (см. boot/boot.s, где он
 * сохраняется в edi и потом передаётся в kernel_main как mb_info).
 *
 * Мы используем только флаг бита 0 (MULTIBOOT_INFO_MEMORY) и поля
 * mem_lower/mem_upper -- их предоставляет любой multiboot-загрузчик,
 * если в заголовке (boot/boot.s, MEMINFO = 1<<1) запрошена базовая
 * информация о памяти. Полная карта памяти (mmap_addr/mmap_length,
 * бит 6) была бы точнее (учла бы "дыры" в адресном пространстве под
 * PCI/MMIO), но потребовала бы менять multiboot-заголовок в boot.s;
 * для игрушечной ОС с типичными объёмами RAM в QEMU (64-512 MiB)
 * базовой информации достаточно с большим запасом.
 */

#define MULTIBOOT_INFO_MEMORY 0x1u

typedef struct multiboot_info {
    u32 flags;
    u32 mem_lower;   /* KiB обычной памяти (0..640 KiB) */
    u32 mem_upper;   /* KiB памяти начиная с 1 MiB и до первой "дыры" */
    /* остальные поля структуры нам не нужны и здесь не описаны */
} multiboot_info_t;

#endif
