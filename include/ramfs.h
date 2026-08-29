#ifndef RAMFS_H
#define RAMFS_H

#include "types.h"

/* Идентификатор, привязанный к файлу в RAMFS, показывающий, является ли
   он исполняемым пользовательским бинарником (см. kernel/loader.c).
   Любой файл в rootfs/bin/<name> без точки в имени автоматически
   помечается как RAMFS_APP_BINARY при генерации ramfs_data.c
   (см. tools/gen_rootfs.py) -- никакого списка приложений в коде ядра
   больше нет, бинарником может стать любой файл. */
#define RAMFS_FILE_DATA_SIZE 65536

typedef enum {
    RAMFS_APP_NONE = 0,
    RAMFS_APP_BINARY
} ramfs_app_id_t;

/* ---------- права доступа (rwx + владелец) ----------
 *
 * Классическая unix-модель "9 бит": владелец/группа/прочие x rwx,
 * упакованные в младшие 9 бит u32 (как обычный chmod-режим, 0..0777).
 * Группа здесь -- не отдельная сущность (в этой ОС нет /etc/group),
 * а производная от роли: RAMFS_GID_ROOT -- root-пользователи,
 * RAMFS_GID_USERS -- обычные. Владелец файла -- это uid пользователя,
 * который его создал (см. users.h). root всегда обходит проверку прав,
 * как и в настоящем unix. */
#define RAMFS_PERM_READ  4
#define RAMFS_PERM_WRITE 2
#define RAMFS_PERM_EXEC  1

#define RAMFS_GID_ROOT   0
#define RAMFS_GID_USERS  1

#define RAMFS_MODE_DEFAULT_FILE 0644 /* rw-r--r-- : новые файлы */
#define RAMFS_MODE_DEFAULT_DIR  0755 /* rwxr-xr-x : новые каталоги */
#define RAMFS_MODE_DEFAULT_APP  0755 /* rwxr-xr-x : исполняемые бинарники */
#define RAMFS_MODE_MASK         0777

/* Информация о правах и владельце узла, для отображения (ls) и chmod/chown. */
typedef struct {
    u32 owner_uid;
    u32 owner_gid;
    u32 mode; /* младшие 9 бит: rwxrwxrwx */
} ramfs_perm_info_t;

/* Фаза 5, подшаг 5.2 (см. CONTAINER_ARCHITECTURE.md): одна запись
   листинга каталога -- общий для всех трёх backend'ов VFS (RAMFS/
   FAT32/UFS, см. include/vfs.h: fs_list()) тип, поэтому живёт здесь,
   а не в vfs.h -- vfs.h подключает этот файл, а не наоборот, и заводить
   отдельный header ради одного типа не тот случай (тот же принцип, что
   уже применён к ramfs_perm_info_t выше). owner_uid/mode осмысленны
   только на RAMFS backend'е -- на FAT32/UFS (у них нет понятия
   владельца узла на диске) заполняются нулями, is_app всегда FALSE вне
   RAMFS. FS_DIRENT_NAME_MAX -- независимая от внутреннего
   RAMFS_NAME_LEN (fs/ramfs.c) константа: этот тип уже граница между VFS
   и остальным ядром, а не деталь одного конкретного backend'а. */
#define FS_DIRENT_NAME_MAX 32
typedef struct {
    char name[FS_DIRENT_NAME_MAX];
    u32  size;
    u32  owner_uid;
    u32  mode;
    bool_t is_dir;
    bool_t is_app;
} fs_dirent_t;

/* Создаёт корневой каталог "/" и распаковывает в него базовые файлы
   из таблицы ramfs_seed_files (см. ramfs_data.h), включая /bin/calc
   и /readme.txt. Вызывается один раз при старте ядра. */
void ramfs_init(void);

/* Сбрасывает текущий каталог RAMFS обратно в корень (используется при
   переключении между RAMFS и FAT32 через VFS). */
void ramfs_reset_cwd(void);

void ramfs_ls(void);
bool_t ramfs_list(fs_dirent_t *out, u32 max_entries, u32 *out_count); /* см. fs_list(), include/vfs.h */
bool_t ramfs_cd(const char *name);
bool_t ramfs_mkdir(const char *name);
bool_t ramfs_rm(const char *name);
bool_t ramfs_write_file(const char *name, const char *data, u32 len);
bool_t ramfs_cat(const char *name);
void ramfs_pwd(char *buf, u32 bufsize);

/* Читает содержимое файла в буфер buf (максимум bufsize байт), кладёт
   реальный размер прочитанных данных в *out_size (если не NULL).
   Используется загрузчиком бинарников (kernel/loader.c). name может
   быть как простым именем (ищется в текущем каталоге), так и абсолютным
   путём вида "/bin/имя". */
bool_t ramfs_read_file(const char *name, void *buf, u32 bufsize, u32 *out_size);

/* Если файл с данным именем/путём существует и помечен как исполняемый
   бинарник, возвращает RAMFS_APP_BINARY, иначе RAMFS_APP_NONE. name
   может быть как простым именем, так и абсолютным путём. */
ramfs_app_id_t ramfs_get_app(const char *name);

/* ---------- права доступа ---------- */

/* Меняет режим доступа (rwx владелец/группа/прочие, младшие 9 бит mode)
   файла/каталога в текущем каталоге. Разрешено владельцу файла и root.
   FALSE, если файл не найден или прав недостаточно. */
bool_t ramfs_chmod(const char *name, u32 mode);

/* Меняет владельца (uid) файла/каталога в текущем каталоге; группа
   пересчитывается автоматически по новому uid (см. RAMFS_GID_*).
   Разрешено только root (как в настоящем unix). */
bool_t ramfs_chown(const char *name, u32 new_uid);

/* Заполняет *out информацией о правах/владельце файла/каталога с данным
   именем в текущем каталоге. FALSE, если не найден. */
bool_t ramfs_stat(const char *name, ramfs_perm_info_t *out);

#endif
