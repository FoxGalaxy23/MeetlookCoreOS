#ifndef BOOT_ASSETS_H
#define BOOT_ASSETS_H

#include "types.h"

/* boot_assets.h -- см. общий комментарий у SYS_BOOT_ASSET_READ
 * (include/sysabi.h) за тем, почему это не RAMFS-файлы: RAMFS ограничен
 * RAMFS_FILE_DATA_SIZE=65536 байт на файл (include/ramfs.h), а
 * kernel.flat много больше. Данные лежат прямо в .rodata самого ядра
 * (см. kernel/boot_assets_data.c, генерируется tools/gen_boot_assets.py)
 * -- без файловой системы, без ограничения по размеру, кроме памяти. */

typedef enum {
    BOOT_ASSET_STAGE1 = 0,
    BOOT_ASSET_STAGE2 = 1,
    BOOT_ASSET_KERNEL_FLAT = 2,
    /* ATOMIC_PLAN.md, Фаза 6 -- отдельная пара ассетов для GPT/Atomic-
     * дисков (см. общий комментарий в начале boot/stage2_atomic.s):
     * STAGE1_ATOMIC -- та же самая boot/stage1.s, собранная с
     * -D STAGE2_LBA_START=34 (Makefile) -- т.е. читает stage2 из
     * раздела bios-boot, а не с LBA 1, но ИНАЧЕ байт в байт совпадает
     * с обычным BOOT_ASSET_STAGE1 (используется как boot_code в
     * protective MBR через mbr_build(), install_atomic_layout(),
     * apps/install-os.c, ровно как BOOT_ASSET_STAGE1 в legacy-пути).
     * STAGE2_ATOMIC -- boot/stage2_atomic.s целиком, пишется в раздел
     * bios-boot (НЕ на LBA 1..63, там на Atomic-диске GPT-заголовок и
     * таблица разделов). */
    BOOT_ASSET_STAGE1_ATOMIC = 3,
    BOOT_ASSET_STAGE2_ATOMIC = 4,
    BOOT_ASSET_COUNT
} boot_asset_id_t;

typedef struct {
    const u8 *data;
    u32 size;
} boot_asset_entry_t;

/* Реализовано в сгенерированном kernel/boot_assets_data.c
 * (tools/gen_boot_assets.py) -- возвращает массив из BOOT_ASSET_COUNT
 * записей, индексируемый boot_asset_id_t. */
const boot_asset_entry_t *boot_assets_table(void);

/* Отдаёт указатель на данные ассета id (read-only, живёт столько же,
 * сколько само ядро -- копировать не нужно, но и писать в этот адрес
 * нельзя) и его размер через out_size. Возвращает FALSE, если id вне
 * диапазона BOOT_ASSET_COUNT. */
bool_t boot_asset_get(boot_asset_id_t id, const u8 **out_data, u32 *out_size);

#endif
