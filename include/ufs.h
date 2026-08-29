#ifndef UFS_H
#define UFS_H

#include "types.h"
#include "ramfs.h" /* fs_dirent_t -- см. ufs_list() ниже */

/* Низкоуровневый драйвер UFS (Unix File System) на реальном диске.
 *
 * Это упрощённая, но структурно верная реализация в духе классической
 * BSD UFS: суперблок + битовая карта свободных блоков + таблица inode'ов
 * + блоки данных. В отличие от FAT32 (fs/fat32.c), где файл -- это
 * связный список кластеров через таблицу FAT, здесь файл -- это inode
 * с прямыми указателями на блоки данных (direct blocks) и, для файлов
 * покрупнее, одним косвенным блоком (single indirect block), как в
 * настоящем UFS/ext2. Имена файлов в inode не хранятся -- они лежат в
 * записях каталога (directory entry: inode + имя), поэтому один и тот
 * же inode может быть виден под разными именами (hardlink), хотя это
 * пока не используется.
 *
 * Используется только через VFS-слой (см. vfs.h) -- shell и остальной
 * код не должны вызывать эти функции напрямую. */

bool_t ufs_mount(u32 drive_index);
bool_t ufs_format(u32 drive_index);
bool_t ufs_is_mounted(void);
u32    ufs_current_drive(void);

#define UFS_MASTER_KEY_LEN 32u /* = XTS_MASTER_KEY_LEN (include/crypto_xts.h), продублировано здесь
    вместо #include "crypto_xts.h", чтобы ufs.h не тянул крипто-заголовки
    для кода, которому encrypted-монтирование не нужно (тот же принцип
    минимальности, что и у остального этого файла, см. общий комментарий
    выше) -- значения ОБЯЗАНЫ совпадать, оба равны размеру ключа AES-256. */

/* Монтирует ТЕЛО зашифрованного пользовательского образа (include/userimg.h,
   ATOMIC_PLAN.md Фаза 3) через lib/cryptoblk.c (Фаза 4) вместо обычного
   диска -- см. общий комментарий у реализации в fs/ufs.c. master_key --
   уже РАСШИФРОВАННЫЙ ключ (из userimg_verify_password()), не пароль.
   slot_body_lba_start -- абсолютный LBA начала тела слота на физическом
   диске (сразу после заголовка userimg.h, см. apps/create-user.c). */
bool_t ufs_mount_encrypted(u32 drive_index, u32 slot_body_lba_start,
                            const u8 master_key[UFS_MASTER_KEY_LEN], bool_t readonly);

/* Форматирует НОВЫЙ UFS поверх тела зашифрованного слота -- см. общий
   комментарий у реализации в fs/ufs.c. slot_body_sectors -- размер
   ИМЕННО тела (не всего слота -- caller вычитает заголовок userimg.h
   сам, см. apps/create-user.c). РАЗРУШАЕТ данные в этом диапазоне. */
bool_t ufs_format_encrypted(u32 drive_index, u32 slot_body_lba_start, u32 slot_body_sectors,
                             const u8 master_key[UFS_MASTER_KEY_LEN]);

/* Разрешает/запрещает запись на УЖЕ смонтированной ФС (mkdir/rm/
   write_file начинают возвращать FALSE, не трогая сам факт монтирования)
   -- см. общий комментарий у реализации. Не влияет на ufs_format(). */
void   ufs_set_readonly(bool_t readonly);
bool_t ufs_is_readonly(void);

void   ufs_ls(void);
bool_t ufs_list(fs_dirent_t *out, u32 max_entries, u32 *out_count); /* см. fs_list(), include/vfs.h */
bool_t ufs_cd(const char *name);
bool_t ufs_mkdir(const char *name);
bool_t ufs_rm(const char *name);
bool_t ufs_write_file(const char *name, const char *data, u32 len);
bool_t ufs_cat(const char *name);
bool_t ufs_read_file(const char *name, void *buf, u32 bufsize, u32 *out_size);
void   ufs_pwd(char *buf, u32 bufsize);

#endif
