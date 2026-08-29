/* boot_assets.c -- см. include/boot_assets.h. Сама эта функция
 * рукописная и не меняется от сборки к сборке; фактические байты
 * ассетов -- в отдельном СГЕНЕРИРОВАННОМ файле kernel/boot_assets_data.c
 * (tools/gen_boot_assets.py, аналогично тому, как fs/ramfs_data.c
 * генерируется tools/gen_rootfs.py) -- тот файл предоставляет
 * BOOT_ASSET_COUNT массивов (g_boot_asset_stage1/.../_stage2_atomic,
 * см. include/boot_assets.h) и их размеры через boot_assets_table(),
 * которую эта функция вызывает -- индексирована id, поэтому Фаза 6
 * (два новых ассета, STAGE1_ATOMIC/STAGE2_ATOMIC) саму эту функцию не
 * тронула вообще. */

#include "boot_assets.h"

bool_t boot_asset_get(boot_asset_id_t id, const u8 **out_data, u32 *out_size) {
    if (id >= BOOT_ASSET_COUNT) return FALSE;
    const boot_asset_entry_t *table = boot_assets_table();
    *out_data = table[id].data;
    *out_size = table[id].size;
    return TRUE;
}
