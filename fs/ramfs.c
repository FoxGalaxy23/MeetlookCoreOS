/* ramfs.c -- простая файловая система в оперативной памяти.
 *
 * Все узлы (каталоги и файлы) хранятся в статическом массиве -- в
 * ядре пока нет кучи (heap), поэтому динамическая память не используется.
 * Содержимое файлов хранится отдельно, в фиксированном пуле "слотов",
 * чтобы каталоги (которым содержимое не нужно) не расходовали память.
 *
 * RAMFS всегда доступна и монтируется автоматически при старте ядра
 * (см. vfs_init() / kernel_main()), заполняясь базовыми файлами из
 * ramfs_seed_files (сгенерированы из каталога rootfs/, см. ramfs_data.h
 * и tools/gen_rootfs.py).
 */

#include "ramfs.h"
#include "ramfs_data.h"
#include "string.h"
#include "kprintf.h"
#include "users.h"

#define RAMFS_MAX_NODES      96
#define RAMFS_MAX_FILES      72
/* Было 48 -- этого хватало, пока в rootfs/ не появилось больше файлов,
   чем слотов: посевных файлов сейчас 55 (rootfs/, см. tools/gen_rootfs.py),
   и при 48 слотах ramfs_init() молча (!) пропускал file_slot для
   "лишних" файлов (см. `if (n->file_slot < 0) continue;` ниже -- без
   предупреждения, файл просто оставался пустым), а главное -- ВСЕ 48
   слотов уходили под посевные файлы, не оставляя ни одного свободного
   для эфемерных файлов вроде .run.manifest (apps/shell.c, RUN_MANIFEST_PATH)
   -- отсюда "run: could not prepare manifest (write failed)" абсолютно
   на любом бинарнике. 72 = 55 посевных + запас ~17 под эфемерные файлы
   сессии (манифесты run, временные файлы команд и т.п.).
   Достаточно места под небольшие пользовательские бинарники (см.
   kernel/loader.c, apps/user.ld), а не только текстовые файлы. */
#define RAMFS_NAME_LEN       32
#define RAMFS_MAX_DEPTH      16
#define RAMFS_ROOT_INDEX     0

typedef struct {
    bool_t used;
    bool_t is_dir;
    char name[RAMFS_NAME_LEN];
    s32 parent;           /* индекс родителя, -1 для корня */
    s32 file_slot;         /* индекс в file_data[], -1 для каталогов */
    u32 size;               /* используется только для файлов */
    ramfs_app_id_t app_id;
    u32 owner_uid;          /* uid владельца (создателя) */
    u32 owner_gid;          /* RAMFS_GID_ROOT или RAMFS_GID_USERS, снимок на момент создания */
    u32 mode;               /* rwxrwxrwx владелец/группа/прочие, младшие 9 бит */
} ramfs_node_t;

static ramfs_node_t nodes[RAMFS_MAX_NODES];
static u8 file_data[RAMFS_MAX_FILES][RAMFS_FILE_DATA_SIZE];
static bool_t file_slot_used[RAMFS_MAX_FILES];

static s32 dir_stack[RAMFS_MAX_DEPTH];
static u32 stack_depth = 1;

/* ---------- Внутренние помощники ---------- */

static s32 alloc_node(void) {
    for (u32 i = 0; i < RAMFS_MAX_NODES; i++) {
        if (!nodes[i].used) return (s32)i;
    }
    return -1;
}

static s32 alloc_file_slot(void) {
    for (u32 i = 0; i < RAMFS_MAX_FILES; i++) {
        if (!file_slot_used[i]) {
            file_slot_used[i] = TRUE;
            return (s32)i;
        }
    }
    return -1;
}

static void free_file_slot(s32 slot) {
    if (slot >= 0 && slot < RAMFS_MAX_FILES) {
        file_slot_used[slot] = FALSE;
    }
}

static s32 current_dir(void) {
    return dir_stack[stack_depth - 1];
}

static s32 find_child(s32 parent, const char *name) {
    for (u32 i = 0; i < RAMFS_MAX_NODES; i++) {
        if (nodes[i].used && nodes[i].parent == parent &&
            strcmp(nodes[i].name, name) == 0) {
            return (s32)i;
        }
    }
    return -1;
}

static bool_t has_children(s32 idx) {
    for (u32 i = 0; i < RAMFS_MAX_NODES; i++) {
        if (nodes[i].used && nodes[i].parent == idx) return TRUE;
    }
    return FALSE;
}

/* ---------- права доступа: вспомогательные ---------- */

/* uid 0 -- всегда root (см. users.c: users_init создаёт root с uid 0
   первым и это единственный способ получить uid 0). Используется вместо
   users_current_is_root() там, где нужен вывод по uid, а не по "текущему
   вошедшему", например при создании узлов во время ramfs_init() (когда
   ещё никто не вошёл в систему). */
static u32 gid_for_uid(u32 uid) {
    return (uid == 0) ? RAMFS_GID_ROOT : RAMFS_GID_USERS;
}

static bool_t current_in_group(u32 gid) {
    return gid_for_uid(users_current_uid()) == gid;
}

/* Проверяет, что у текущего вошедшего пользователя есть все биты прав
   want (комбинация RAMFS_PERM_READ/WRITE/EXEC) на узел n. root всегда
   проходит проверку, как и в настоящем unix. */
static bool_t check_perm(const ramfs_node_t *n, u32 want) {
    if (users_current_is_root()) return TRUE;

    u32 uid = users_current_uid();
    u32 shift;
    if (uid == n->owner_uid) {
        shift = 6; /* владелец */
    } else if (current_in_group(n->owner_gid)) {
        shift = 3; /* группа */
    } else {
        shift = 0; /* прочие */
    }
    u32 bits = (n->mode >> shift) & 07;
    return (bits & want) == want;
}

static s32 make_node(s32 parent, const char *name, bool_t is_dir) {
    s32 idx = alloc_node();
    if (idx < 0) return -1;
    ramfs_node_t *n = &nodes[idx];
    n->used = TRUE;
    n->is_dir = is_dir;
    strncpy(n->name, name, RAMFS_NAME_LEN - 1);
    n->name[RAMFS_NAME_LEN - 1] = '\0';
    n->parent = parent;
    n->file_slot = -1;
    n->size = 0;
    n->app_id = RAMFS_APP_NONE;
    n->owner_uid = users_current_uid();
    n->owner_gid = gid_for_uid(n->owner_uid);
    n->mode = is_dir ? RAMFS_MODE_DEFAULT_DIR : RAMFS_MODE_DEFAULT_FILE;
    return idx;
}

/* Находит (или создаёт) каталог с заданным путём вида "/bin", начиная
   от корня. Возвращает индекс каталога либо -1 при ошибке. */
static s32 ensure_dir_path(const char *path) {
    s32 cur = RAMFS_ROOT_INDEX;
    if (path[0] == '\0' || (path[0] == '/' && path[1] == '\0')) return cur;

    char comp[RAMFS_NAME_LEN];
    u32 ci = 0;
    const char *p = path;
    if (*p == '/') p++;

    while (TRUE) {
        if (*p == '/' || *p == '\0') {
            comp[ci] = '\0';
            if (ci > 0) {
                s32 child = find_child(cur, comp);
                if (child < 0) {
                    child = make_node(cur, comp, TRUE);
                    if (child < 0) return -1;
                }
                cur = child;
            }
            ci = 0;
            if (*p == '\0') break;
            p++;
        } else {
            if (ci < RAMFS_NAME_LEN - 1) comp[ci++] = *p;
            p++;
        }
    }
    return cur;
}

/* Разбивает полный путь "/bin/calc" на (каталог-родитель, "calc"). */
static s32 split_parent_and_leaf(const char *full_path, char *leaf_out) {
    u32 len = strlen(full_path);
    s32 last_slash = -1;
    for (u32 i = 0; i < len; i++) {
        if (full_path[i] == '/') last_slash = (s32)i;
    }
    if (last_slash < 0) {
        strncpy(leaf_out, full_path, RAMFS_NAME_LEN - 1);
        leaf_out[RAMFS_NAME_LEN - 1] = '\0';
        return RAMFS_ROOT_INDEX;
    }

    strncpy(leaf_out, full_path + last_slash + 1, RAMFS_NAME_LEN - 1);
    leaf_out[RAMFS_NAME_LEN - 1] = '\0';

    if (last_slash == 0) return RAMFS_ROOT_INDEX;

    char dir_path[128];
    u32 dl = (u32)last_slash;
    if (dl > sizeof(dir_path) - 1) dl = sizeof(dir_path) - 1;
    memcpy(dir_path, full_path, dl);
    dir_path[dl] = '\0';
    return ensure_dir_path(dir_path);
}

/* Находит узел по имени/пути. Если path начинается с '/', поиск идёт от
   корня, иначе -- от текущего каталога (обычное относительное имя, как
   было раньше). В отличие от ensure_dir_path() каталоги не создаются --
   если какого-то компонента пути нет, возвращается -1. Используется для
   поиска исполняемых бинарников по пути вида "/bin/имя" независимо от
   текущего каталога пользователя. */
static s32 resolve_node(const char *path) {
    s32 cur = (path[0] == '/') ? RAMFS_ROOT_INDEX : current_dir();
    const char *p = path;
    if (*p == '/') p++;
    if (*p == '\0') return cur; /* путь был просто "/" */

    char comp[RAMFS_NAME_LEN];
    u32 ci = 0;
    while (TRUE) {
        if (*p == '/' || *p == '\0') {
            comp[ci] = '\0';
            if (ci > 0) {
                s32 child = find_child(cur, comp);
                if (child < 0) return -1;
                cur = child;
            }
            ci = 0;
            if (*p == '\0') break;
            p++;
        } else {
            if (ci < RAMFS_NAME_LEN - 1) comp[ci++] = *p;
            p++;
        }
    }
    return cur;
}

/* ---------- Инициализация ---------- */

void ramfs_init(void) {
    memset(nodes, 0, sizeof(nodes));
    memset(file_slot_used, 0, sizeof(file_slot_used));

    nodes[RAMFS_ROOT_INDEX].used = TRUE;
    nodes[RAMFS_ROOT_INDEX].is_dir = TRUE;
    strcpy(nodes[RAMFS_ROOT_INDEX].name, "/");
    nodes[RAMFS_ROOT_INDEX].parent = -1;
    nodes[RAMFS_ROOT_INDEX].file_slot = -1;
    nodes[RAMFS_ROOT_INDEX].owner_uid = 0;
    nodes[RAMFS_ROOT_INDEX].owner_gid = RAMFS_GID_ROOT;
    nodes[RAMFS_ROOT_INDEX].mode = RAMFS_MODE_DEFAULT_DIR;

    stack_depth = 1;
    dir_stack[0] = RAMFS_ROOT_INDEX;

    for (u32 i = 0; i < ramfs_seed_file_count; i++) {
        const ramfs_seed_file_t *sf = &ramfs_seed_files[i];

        char leaf[RAMFS_NAME_LEN];
        s32 parent = split_parent_and_leaf(sf->path, leaf);
        if (parent < 0 || leaf[0] == '\0') continue;

        s32 idx = find_child(parent, leaf);
        if (idx < 0) idx = make_node(parent, leaf, FALSE);
        if (idx < 0) continue;

        ramfs_node_t *n = &nodes[idx];
        n->is_dir = FALSE;
        if (n->file_slot < 0) n->file_slot = alloc_file_slot();
        if (n->file_slot < 0) {
            kprintf("ramfs: WARNING -- нет свободных file-слотов (RAMFS_MAX_FILES=%u), "
                    "'%s' загружен пустым!\n", RAMFS_MAX_FILES, sf->path);
            continue;
        }

        u32 len = sf->size;
        if (len > RAMFS_FILE_DATA_SIZE) len = RAMFS_FILE_DATA_SIZE;
        memcpy(file_data[n->file_slot], sf->data, len);
        n->size = len;
        n->app_id = sf->app_id;
        /* Все файлы из rootfs/ принадлежат root'у (сеются до входа в
           систему); исполняемые бинарники дополнительно получают бит x. */
        n->owner_uid = 0;
        n->owner_gid = RAMFS_GID_ROOT;
        n->mode = (sf->app_id != RAMFS_APP_NONE) ? RAMFS_MODE_DEFAULT_APP : RAMFS_MODE_DEFAULT_FILE;
    }
}

void ramfs_reset_cwd(void) {
    stack_depth = 1;
    dir_stack[0] = RAMFS_ROOT_INDEX;
}

/* ---------- Команды ---------- */

/* Строит строку вида "drwxr-x---" (10 символов + '\0') из режима и
   типа узла, в стиле "ls -l". buf должен быть не меньше 11 байт. */
static void mode_to_str(bool_t is_dir, u32 mode, char *buf) {
    buf[0] = is_dir ? 'd' : '-';
    static const char *bits = "rwx";
    for (u32 cls = 0; cls < 3; cls++) {
        u32 shift = 6 - cls * 3;
        u32 v = (mode >> shift) & 07;
        for (u32 b = 0; b < 3; b++) {
            buf[1 + cls * 3 + b] = (v & (04 >> b)) ? bits[b] : '-';
        }
    }
    buf[10] = '\0';
}

void ramfs_ls(void) {
    s32 dir = current_dir();
    u32 count = 0;
    char perm[11];
    for (u32 i = 0; i < RAMFS_MAX_NODES; i++) {
        if (!nodes[i].used || nodes[i].parent != dir) continue;
        mode_to_str(nodes[i].is_dir, nodes[i].mode, perm);
        const char *owner = users_name_by_uid(nodes[i].owner_uid);
        if (nodes[i].is_dir) {
            kprintf("  %s  %s\t<DIR>\t%s\n", perm, owner, nodes[i].name);
        } else if (nodes[i].app_id != RAMFS_APP_NONE) {
            kprintf("  %s  %s\t%u\t%s\t[app]\n", perm, owner, nodes[i].size, nodes[i].name);
        } else {
            kprintf("  %s  %s\t%u\t%s\n", perm, owner, nodes[i].size, nodes[i].name);
        }
        count++;
    }
    if (count == 0) kprintf("  (empty)\n");
}

bool_t ramfs_list(fs_dirent_t *out, u32 max_entries, u32 *out_count) {
    s32 dir = current_dir();
    u32 count = 0;
    for (u32 i = 0; i < RAMFS_MAX_NODES; i++) {
        if (!nodes[i].used || nodes[i].parent != dir) continue;
        if (count < max_entries) {
            fs_dirent_t *d = &out[count];
            strncpy(d->name, nodes[i].name, FS_DIRENT_NAME_MAX - 1);
            d->name[FS_DIRENT_NAME_MAX - 1] = '\0';
            d->size = nodes[i].size;
            d->owner_uid = nodes[i].owner_uid;
            d->mode = nodes[i].mode;
            d->is_dir = nodes[i].is_dir;
            d->is_app = (nodes[i].app_id != RAMFS_APP_NONE);
        }
        count++;
    }
    if (out_count) *out_count = count;
    return TRUE;
}

bool_t ramfs_cd(const char *name) {
    if (strcmp(name, "/") == 0) {
        ramfs_reset_cwd();
        return TRUE;
    }
    if (strcmp(name, ".") == 0) return TRUE;
    if (strcmp(name, "..") == 0) {
        if (stack_depth > 1) stack_depth--;
        return TRUE;
    }

    s32 child = find_child(current_dir(), name);
    if (child < 0 || !nodes[child].is_dir) return FALSE;
    if (!check_perm(&nodes[child], RAMFS_PERM_EXEC)) return FALSE; /* нужен бит x, чтобы войти в каталог */
    if (stack_depth >= RAMFS_MAX_DEPTH) return FALSE;

    dir_stack[stack_depth++] = child;
    return TRUE;
}

bool_t ramfs_mkdir(const char *name) {
    /* Создание записи в каталоге требует w+x на сам каталог (как в unix). */
    if (!check_perm(&nodes[current_dir()], RAMFS_PERM_WRITE | RAMFS_PERM_EXEC)) return FALSE;
    if (find_child(current_dir(), name) >= 0) return FALSE;
    return make_node(current_dir(), name, TRUE) >= 0;
}

bool_t ramfs_rm(const char *name) {
    /* Удаление записи из каталога тоже требует w+x на каталог (право
       удалять файл определяется правами на каталог, а не на сам файл --
       так же, как в настоящем unix). */
    if (!check_perm(&nodes[current_dir()], RAMFS_PERM_WRITE | RAMFS_PERM_EXEC)) return FALSE;

    s32 idx = find_child(current_dir(), name);
    if (idx < 0) return FALSE;
    if (nodes[idx].is_dir && has_children(idx)) return FALSE; /* каталог не пуст */

    if (!nodes[idx].is_dir && nodes[idx].file_slot >= 0) {
        free_file_slot(nodes[idx].file_slot);
    }
    memset(&nodes[idx], 0, sizeof(ramfs_node_t));
    return TRUE;
}

bool_t ramfs_write_file(const char *name, const char *data, u32 len) {
    if (len > RAMFS_FILE_DATA_SIZE) len = RAMFS_FILE_DATA_SIZE;

    s32 idx = find_child(current_dir(), name);
    if (idx < 0) {
        /* Новый файл: нужны w+x на каталог, где он создаётся. */
        if (!check_perm(&nodes[current_dir()], RAMFS_PERM_WRITE | RAMFS_PERM_EXEC)) return FALSE;
        idx = make_node(current_dir(), name, FALSE);
        if (idx < 0) return FALSE;
    } else if (nodes[idx].is_dir) {
        return FALSE;
    } else {
        /* Перезапись существующего файла: нужен w на сам файл. */
        if (!check_perm(&nodes[idx], RAMFS_PERM_WRITE)) return FALSE;
    }

    ramfs_node_t *n = &nodes[idx];
    if (n->file_slot < 0) {
        n->file_slot = alloc_file_slot();
        if (n->file_slot < 0) return FALSE;
    }

    memcpy(file_data[n->file_slot], data, len);
    n->size = len;
    return TRUE;
}

bool_t ramfs_cat(const char *name) {
    s32 idx = find_child(current_dir(), name);
    if (idx < 0 || nodes[idx].is_dir) return FALSE;
    if (!check_perm(&nodes[idx], RAMFS_PERM_READ)) return FALSE; /* нужен бит r */

    if (nodes[idx].file_slot >= 0) {
        const u8 *d = file_data[nodes[idx].file_slot];
        for (u32 i = 0; i < nodes[idx].size; i++) {
            kprintf("%c", d[i]);
        }
    }
    kprintf("\n");
    return TRUE;
}

void ramfs_pwd(char *buf, u32 bufsize) {
    buf[0] = '\0';
    if (stack_depth == 1) {
        strncpy(buf, "/", bufsize);
        return;
    }
    for (u32 i = 1; i < stack_depth; i++) {
        strcat(buf, "/");
        strcat(buf, nodes[dir_stack[i]].name);
    }
}

ramfs_app_id_t ramfs_get_app(const char *name) {
    s32 idx = resolve_node(name);
    if (idx < 0 || nodes[idx].is_dir) return RAMFS_APP_NONE;
    return nodes[idx].app_id;
}

bool_t ramfs_read_file(const char *name, void *buf, u32 bufsize, u32 *out_size) {
    s32 idx = resolve_node(name);
    if (idx < 0 || nodes[idx].is_dir) return FALSE;
    if (!check_perm(&nodes[idx], RAMFS_PERM_READ)) return FALSE; /* нужен бит r (для бинарников по умолчанию есть) */

    u32 len = nodes[idx].size;
    if (len > bufsize) len = bufsize;
    if (nodes[idx].file_slot >= 0 && len > 0) {
        memcpy(buf, file_data[nodes[idx].file_slot], len);
    }
    if (out_size) *out_size = len;
    return TRUE;
}

/* ---------- права доступа: chmod/chown/stat ---------- */

bool_t ramfs_chmod(const char *name, u32 mode) {
    s32 idx = find_child(current_dir(), name);
    if (idx < 0) return FALSE;

    /* Менять права может только владелец файла или root -- так же, как
       в настоящем unix (обычный пользователь не может отдать себе право
       записи на чужой файл, даже если у него есть право x/w на каталог). */
    if (!users_current_is_root() && nodes[idx].owner_uid != users_current_uid()) return FALSE;

    nodes[idx].mode = mode & RAMFS_MODE_MASK;
    return TRUE;
}

bool_t ramfs_chown(const char *name, u32 new_uid) {
    s32 idx = find_child(current_dir(), name);
    if (idx < 0) return FALSE;

    /* Передавать владение файлом может только root -- иначе можно было
       бы "подкинуть" файл и переложить ответственность на кого-то ещё. */
    if (!users_current_is_root()) return FALSE;

    nodes[idx].owner_uid = new_uid;
    nodes[idx].owner_gid = gid_for_uid(new_uid);
    return TRUE;
}

bool_t ramfs_stat(const char *name, ramfs_perm_info_t *out) {
    s32 idx = find_child(current_dir(), name);
    if (idx < 0 || !out) return FALSE;
    out->owner_uid = nodes[idx].owner_uid;
    out->owner_gid = nodes[idx].owner_gid;
    out->mode = nodes[idx].mode;
    return TRUE;
}
