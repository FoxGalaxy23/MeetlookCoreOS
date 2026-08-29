#include "vfs.h"
#include "fat32.h"
#include "ufs.h"
#include "ramfs.h"

typedef enum {
    BACKEND_RAMFS = 0,
    BACKEND_FAT32 = 1,
    BACKEND_UFS   = 2,
    BACKEND_UFS_ENCRYPTED = 3 /* ATOMIC_PLAN.md, Фаза 4 -- та же UFS-реализация (fs/ufs.c),
        смонтированная через ufs_mount_encrypted() (поверх lib/cryptoblk.c) вместо
        обычного ufs_mount(). Отдельное значение enum здесь -- ТОЛЬКО чтобы
        fs_backend_name()/fs_backend_is_ufs() могли честно различать "обычный
        диск" и "расшифрованный дом" для человека (приглашение шелла и т.п.);
        fs/ufs.c сам не хранит это различие -- у него общая реализация что
        для обычных дисков, что для зашифрованных, поэтому все fs_* вызовы
        ниже, диспетчирующие по backend_t, для обоих значений идут в один и
        тот же case (см. is_ufs_family() ниже). */
} backend_t;

static backend_t active_backend = BACKEND_RAMFS;

/* И BACKEND_UFS, и BACKEND_UFS_ENCRYPTED -- один и тот же драйвер
   (fs/ufs.c), команды диспетчируются туда одинаково -- см. общий
   комментарий у BACKEND_UFS_ENCRYPTED выше. */
static bool_t is_ufs_family(void) {
    return active_backend == BACKEND_UFS || active_backend == BACKEND_UFS_ENCRYPTED;
}

void vfs_init(void) {
    ramfs_init();
    active_backend = BACKEND_RAMFS;
}

bool_t fs_mount(u32 drive_index) {
    /* Автоопределение: пробуем сначала FAT32 (проверка по BPB), затем UFS
       (проверка по магическому числу суперблока). Обе *_mount() сами
       отказывают, если сигнатура на диске не совпадает с их форматом. */
    if (fat32_mount(drive_index)) {
        active_backend = BACKEND_FAT32;
        return TRUE;
    }
    if (ufs_mount(drive_index)) {
        active_backend = BACKEND_UFS;
        return TRUE;
    }
    return FALSE;
}

bool_t fs_format(u32 drive_index, fs_type_t type) {
    if (type == FS_TYPE_UFS) {
        if (ufs_format(drive_index)) {
            active_backend = BACKEND_UFS;
            return TRUE;
        }
        return FALSE;
    }
    if (fat32_format(drive_index)) {
        active_backend = BACKEND_FAT32;
        return TRUE;
    }
    return FALSE;
}

void fs_use_ramfs(void) {
    active_backend = BACKEND_RAMFS;
    ramfs_reset_cwd();
}

/* Монтирует тело зашифрованного пользовательского образа (Фаза 3)
 * через ufs_mount_encrypted() (fs/ufs.c, поверх lib/cryptoblk.c, Фаза 4)
 * и переключает активный backend на него -- та же роль, что fs_mount()
 * для обычного диска, но БЕЗ автоопределения (тип всегда UFS -- см.
 * общий комментарий в apps/create-user.c, тело слота форматируется
 * именно так) и с готовым, уже расшифрованным master_key вместо
 * номера диска "как есть" (пароль здесь ни при чём -- его проверяет
 * userimg_verify_password(), include/userimg.h, ДО этого вызова; сам
 * этот файл про пароли ничего не знает).
 *
 * ВАЖНО (см. общий комментарий у BACKEND_UFS_ENCRYPTED выше и у
 * ufs_mount_encrypted() в fs/ufs.c) -- это ПЕРЕКЛЮЧЕНИЕ активного
 * backend'а, не добавление второй одновременной точки монтирования:
 * пока кто-то смотрит "домашний" зашифрованный образ, обычный
 * FAT32/UFS-backend (если был активен) недоступен, пока явно не
 * вызвать fs_mount()/fs_use_ramfs() обратно. Настоящий многоточечный
 * VFS (одновременно доступные "/" и "/home" без переключения) --
 * отдельная, более крупная архитектурная задача, сознательно не
 * входящая в эту Фазу -- см. ATOMIC_PLAN.md, Фаза 4, за подробным
 * объяснением, почему это разумная граница, а не "не успели". */
bool_t fs_mount_home_encrypted(u32 drive_index, u32 slot_body_lba_start,
                                const u8 master_key[UFS_MASTER_KEY_LEN], bool_t readonly) {
    if (!ufs_mount_encrypted(drive_index, slot_body_lba_start, master_key, readonly)) return FALSE;
    active_backend = BACKEND_UFS_ENCRYPTED;
    return TRUE;
}

/* См. общий комментарий в include/vfs.h -- НЕ переключает active_backend,
   в отличие от fs_mount_home_encrypted() выше. */
bool_t fs_format_home_encrypted(u32 drive_index, u32 slot_body_lba_start, u32 slot_body_sectors,
                                 const u8 master_key[UFS_MASTER_KEY_LEN]) {
    return ufs_format_encrypted(drive_index, slot_body_lba_start, slot_body_sectors, master_key);
}

/* TRUE, если активный backend сейчас смонтирован только для чтения --
 * см. ufs_set_readonly()/fat32_set_readonly() и общий комментарий у
 * них. RAMFS никогда не read-only в этом смысле (у нее свои отдельные
 * права -- см. fs_chmod/fs_stat ниже), поэтому default -- FALSE, не
 * "неизвестно". */
bool_t fs_backend_is_readonly(void) {
    if (is_ufs_family()) return ufs_is_readonly();
    if (active_backend == BACKEND_FAT32) return fat32_is_readonly();
    return FALSE;
}

bool_t fs_backend_is_fat32(void) { return active_backend == BACKEND_FAT32; }
bool_t fs_backend_is_ufs(void)   { return is_ufs_family(); }

const char *fs_backend_name(void) {
    switch (active_backend) {
        case BACKEND_FAT32: return "FAT32";
        case BACKEND_UFS:   return "UFS";
        case BACKEND_UFS_ENCRYPTED: return "UFS(encrypted)";
        default:            return "RAM";
    }
}

bool_t fs_is_mounted(void) {
    return fat32_is_mounted() || ufs_is_mounted();
}

u32 fs_current_drive(void) {
    if (is_ufs_family()) return ufs_current_drive();
    return fat32_current_drive();
}

void fs_ls(void) {
    switch (active_backend) {
        case BACKEND_FAT32: fat32_ls(); break;
        case BACKEND_UFS_ENCRYPTED:
        case BACKEND_UFS:   ufs_ls();   break;
        default:            ramfs_ls(); break;
    }
}

bool_t fs_list(fs_dirent_t *out, u32 max_entries, u32 *out_count) {
    switch (active_backend) {
        case BACKEND_FAT32: return fat32_list(out, max_entries, out_count);
        case BACKEND_UFS_ENCRYPTED:
        case BACKEND_UFS:   return ufs_list(out, max_entries, out_count);
        default:             return ramfs_list(out, max_entries, out_count);
    }
}

bool_t fs_cd(const char *name) {
    switch (active_backend) {
        case BACKEND_FAT32: return fat32_cd(name);
        case BACKEND_UFS_ENCRYPTED:
        case BACKEND_UFS:   return ufs_cd(name);
        default:            return ramfs_cd(name);
    }
}

bool_t fs_mkdir(const char *name) {
    switch (active_backend) {
        case BACKEND_FAT32: return fat32_mkdir(name);
        case BACKEND_UFS_ENCRYPTED:
        case BACKEND_UFS:   return ufs_mkdir(name);
        default:            return ramfs_mkdir(name);
    }
}

bool_t fs_rm(const char *name) {
    switch (active_backend) {
        case BACKEND_FAT32: return fat32_rm(name);
        case BACKEND_UFS_ENCRYPTED:
        case BACKEND_UFS:   return ufs_rm(name);
        default:            return ramfs_rm(name);
    }
}

bool_t fs_write_file(const char *name, const char *data, u32 len) {
    switch (active_backend) {
        case BACKEND_FAT32: return fat32_write_file(name, data, len);
        case BACKEND_UFS_ENCRYPTED:
        case BACKEND_UFS:   return ufs_write_file(name, data, len);
        default:            return ramfs_write_file(name, data, len);
    }
}

bool_t fs_cat(const char *name) {
    switch (active_backend) {
        case BACKEND_FAT32: return fat32_cat(name);
        case BACKEND_UFS_ENCRYPTED:
        case BACKEND_UFS:   return ufs_cat(name);
        default:            return ramfs_cat(name);
    }
}

void fs_pwd(char *buf, u32 bufsize) {
    switch (active_backend) {
        case BACKEND_FAT32: fat32_pwd(buf, bufsize); break;
        case BACKEND_UFS_ENCRYPTED:
        case BACKEND_UFS:   ufs_pwd(buf, bufsize);   break;
        default:            ramfs_pwd(buf, bufsize); break;
    }
}

bool_t fs_read_file(const char *name, void *buf, u32 bufsize, u32 *out_size) {
    switch (active_backend) {
        case BACKEND_FAT32: return fat32_read_file(name, buf, bufsize, out_size);
        case BACKEND_UFS_ENCRYPTED:
        case BACKEND_UFS:   return ufs_read_file(name, buf, bufsize, out_size);
        default:            return ramfs_read_file(name, buf, bufsize, out_size);
    }
}

ramfs_app_id_t fs_get_app(const char *name) {
    if (active_backend != BACKEND_RAMFS) return RAMFS_APP_NONE; /* на реальном диске приложения не поддерживаются */
    return ramfs_get_app(name);
}

bool_t fs_supports_permissions(void) {
    return active_backend == BACKEND_RAMFS;
}

bool_t fs_chmod(const char *name, u32 mode) {
    if (active_backend != BACKEND_RAMFS) return FALSE; /* FAT32/UFS: нет владельца файла */
    return ramfs_chmod(name, mode);
}

bool_t fs_chown(const char *name, u32 new_uid) {
    if (active_backend != BACKEND_RAMFS) return FALSE;
    return ramfs_chown(name, new_uid);
}

bool_t fs_stat(const char *name, ramfs_perm_info_t *out) {
    if (active_backend != BACKEND_RAMFS) return FALSE;
    return ramfs_stat(name, out);
}
