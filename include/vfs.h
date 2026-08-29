#ifndef VFS_H
#define VFS_H

#include "types.h"
#include "ramfs.h"

/* VFS -- тонкий диспетчер между backend'ами файловой системы:
 *   - RAMFS  (по умолчанию, всегда доступна, живёт в оперативной памяти)
 *   - FAT32  (активируется командами "select <n>" / "format <n> fat32")
 *   - UFS    (активируется командами "select <n>" / "format <n> ufs")
 *   - UFS, смонтированная поверх AES-XTS (fs_mount_home_encrypted(),
 *     ATOMIC_PLAN.md Фаза 4) -- тот же UFS-драйвер, что и обычная UFS
 *     выше, но читает/пишет через lib/cryptoblk.c, а не напрямую с
 *     диска (см. общий комментарий у fs_mount_home_encrypted() ниже
 *     и BACKEND_UFS_ENCRYPTED в fs/vfs.c).
 *
 * Весь остальной код (shell и т.д.) работает только с функциями fs_*
 * из этого файла и не знает, какой backend активен в данный момент.
 *
 * ВАЖНО -- это диспетчер ОДНОГО активного backend'а за раз (переключение,
 * не одновременное монтирование нескольких точек) -- см. общий
 * комментарий у fs_mount_home_encrypted() за тем, почему это
 * сознательная граница этой Фазы, а не недосмотр.
 */

/* Тип файловой системы для форматирования диска. При монтировании
   ("select") тип определяется автоматически по содержимому диска --
   fs_mount() пробует распознать сначала FAT32, потом UFS. */
typedef enum {
    FS_TYPE_FAT32 = 0,
    FS_TYPE_UFS   = 1
} fs_type_t;

#define UFS_MASTER_KEY_LEN 32u /* см. include/ufs.h -- то же значение, продублировано по той же причине (не тянуть крипто-заголовки в vfs.h без надобности) */

/* Инициализирует RAMFS и делает её активным backend'ом. Вызывается
   один раз при старте ядра, до запуска шелл-контейнера (см. Фазу 5,
   подшаг 5.5, kernel/kernel.c: container_spawn() над /bin/shell читает
   его ELF из уже смонтированной RAMFS через fs_get_app()). */
void vfs_init(void);

bool_t fs_mount(u32 drive_index);                 /* монтирует диск, автоопределяя FAT32 или UFS, переключает backend на него */
bool_t fs_format(u32 drive_index, fs_type_t type); /* форматирует диск в указанную ФС, переключает backend на него */
void   fs_use_ramfs(void);                         /* возвращает активный backend обратно на RAMFS */

/* Монтирует тело зашифрованного пользовательского образа (include/userimg.h,
   ATOMIC_PLAN.md Фаза 3) через AES-XTS (lib/cryptoblk.c, Фаза 4) и
   переключает активный backend на него -- см. общий комментарий в
   fs/vfs.c за подробностями (в т.ч. за тем, почему это переключение,
   а не добавление второй ОДНОВРЕМЕННОЙ точки монтирования). master_key
   -- уже РАСШИФРОВАННЫЙ ключ из userimg_verify_password(), не пароль.
   slot_body_lba_start -- абсолютный LBA начала тела слота (см.
   apps/create-user.c). readonly -- см. fs_backend_is_readonly() ниже. */
bool_t fs_mount_home_encrypted(u32 drive_index, u32 slot_body_lba_start,
                                const u8 master_key[UFS_MASTER_KEY_LEN], bool_t readonly);

/* Форматирует НОВЫЙ UFS поверх AES-XTS в диапазоне [slot_body_lba_start,
   slot_body_lba_start+slot_body_sectors) -- см. общий комментарий у
   ufs_format_encrypted(), fs/ufs.c, и у SYS_FS_FORMAT_HOME_ENCRYPTED,
   include/sysabi.h. НЕ переключает активный backend (в отличие от
   fs_format()/fs_mount_home_encrypted()) -- сразу после форматирования
   вызывающему обычно нечего с ним делать (create-user вызывает это
   один раз при создании пользователя, никого при этом не логиня). */
bool_t fs_format_home_encrypted(u32 drive_index, u32 slot_body_lba_start, u32 slot_body_sectors,
                                 const u8 master_key[UFS_MASTER_KEY_LEN]);

bool_t fs_backend_is_fat32(void);   /* TRUE, если сейчас активен FAT32-диск */
bool_t fs_backend_is_ufs(void);     /* TRUE, если сейчас активен UFS-диск (обычный ИЛИ зашифрованный -- см. fs_mount_home_encrypted()) */
bool_t fs_backend_is_readonly(void); /* TRUE, если активный backend смонтирован только для чтения (RAMFS -- всегда FALSE, см. fs_chmod и т.п. отдельно) */
const char *fs_backend_name(void);  /* "FAT32", "UFS", "UFS(encrypted)" или "RAM" -- для приглашения shell'а */
bool_t fs_is_mounted(void);         /* смонтирован ли какой-либо диск (независимо от активного backend'а) */
u32    fs_current_drive(void);

void fs_ls(void);
bool_t fs_cd(const char *name);
bool_t fs_mkdir(const char *name);
bool_t fs_rm(const char *name);
bool_t fs_write_file(const char *name, const char *data, u32 len);
bool_t fs_cat(const char *name);
void fs_pwd(char *buf, u32 bufsize);

/* Заполняет out[] содержимым текущего каталога активного backend'а, до
   max_entries штук (fs_dirent_t -- см. include/ramfs.h, живёт там же,
   где и ramfs_perm_info_t, по той же причине: vfs.h подключает
   ramfs.h, а не наоборот). *out_count -- сколько записей РЕАЛЬНО есть в
   каталоге (может быть БОЛЬШЕ max_entries, если не влезло -- вызывающий
   узнаёт об этом сравнением *out_count > max_entries, ровно как обычный
   ls предупредил бы "и ещё N файлов", а не молча обрезал бы список).
   Возвращает FALSE, только если backend не смог быть опрошен вообще
   (например FAT32/UFS без смонтированного диска) -- пустой каталог
   (*out_count == 0) это НЕ ошибка, а обычный успешный результат. */
bool_t fs_list(fs_dirent_t *out, u32 max_entries, u32 *out_count);

/* Читает содержимое файла в buf (максимум bufsize байт), *out_size --
   реальный прочитанный размер. Используется загрузчиком пользовательских
   бинарников (kernel/loader.c). Поддерживается только на RAMFS backend'е
   (на FAT32 запуск бинарников пока не поддержан, см. fs_get_app). */
bool_t fs_read_file(const char *name, void *buf, u32 bufsize, u32 *out_size);

/* Возвращает RAMFS_APP_BINARY, если файл с этим именем/путём в активном
   backend'е помечен как исполняемый бинарник, иначе RAMFS_APP_NONE.
   На FAT32 backend'е всегда возвращает RAMFS_APP_NONE -- запуск
   бинарников пока поддержан только на RAMFS. */
ramfs_app_id_t fs_get_app(const char *name);

/* ---- права доступа (rwx + владелец) ----
 * Поддержаны только на RAMFS backend'е -- у FAT32/UFS пока нет понятия
 * владельца файла на диске (см. include/ramfs.h). На FAT32/UFS все три
 * функции просто возвращают FALSE. */
bool_t fs_chmod(const char *name, u32 mode);
bool_t fs_chown(const char *name, u32 new_uid);
bool_t fs_stat(const char *name, ramfs_perm_info_t *out);
bool_t fs_supports_permissions(void); /* TRUE только на RAMFS */

#endif
