#ifndef FAT32_H
#define FAT32_H

#include "types.h"
#include "ramfs.h" /* fs_dirent_t -- см. fat32_list() ниже */

/* Низкоуровневый драйвер FAT32 на реальном диске (ATA).
   Используется через VFS-слой (см. vfs.h) -- шелл и остальной код
   не должны вызывать эти функции напрямую. */

bool_t fat32_mount(u32 drive_index);
bool_t fat32_format(u32 drive_index);
bool_t fat32_is_mounted(void);
u32 fat32_current_drive(void);

/* Разрешает/запрещает запись на уже смонтированной ФС -- см. общий
   комментарий у ufs_set_readonly(), include/ufs.h (тот же приём,
   ATOMIC_PLAN.md Фаза 4). Не влияет на fat32_format(). */
void fat32_set_readonly(bool_t readonly);
bool_t fat32_is_readonly(void);

void fat32_ls(void);
bool_t fat32_list(fs_dirent_t *out, u32 max_entries, u32 *out_count); /* см. fs_list(), include/vfs.h */
bool_t fat32_cd(const char *name);
bool_t fat32_mkdir(const char *name);
bool_t fat32_rm(const char *name);
bool_t fat32_write_file(const char *name, const char *data, u32 len);
bool_t fat32_cat(const char *name);
bool_t fat32_read_file(const char *name, void *buf, u32 bufsize, u32 *out_size);
void fat32_pwd(char *buf, u32 bufsize);

#endif
