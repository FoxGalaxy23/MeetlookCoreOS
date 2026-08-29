/* Автогенерируемый файл -- НЕ РЕДАКТИРОВАТЬ РУКАМИ.
   Собран из каталога rootfs/ скриптом tools/gen_rootfs.py. */

#ifndef RAMFS_DATA_H
#define RAMFS_DATA_H

#include "types.h"
#include "ramfs.h"

typedef struct {
    const char *path;   /* полный путь в RAMFS, напр. "/bin/calc" */
    const u8 *data;
    u32 size;
    ramfs_app_id_t app_id;
} ramfs_seed_file_t;

extern const ramfs_seed_file_t ramfs_seed_files[];
extern const u32 ramfs_seed_file_count;

#endif
