/* container.c -- см. include/container.h за общей идеей, форматом
 * манифеста и текущими ограничениями (Фаза 1 -- капабилити на процесс;
 * Фаза 2 -- этот файл: манифест как файл на диске, а не C-литерал). */

#include "container.h"
#include "process.h"
#include "vfs.h"
#include "ramfs.h"
#include "kheap.h"
#include "kprintf.h"
#include "string.h"

/* Манифест -- маленький текстовый файл, поэтому 4 КиБ с большим запасом
   хватает; больше -- явная ошибка конфигурации (или кто-то подсунул не
   тот файл), а не повод заводить динамический буфер. */
#define CONTAINER_MANIFEST_MAX_SIZE 4096u

/* Разбирает десятичное ("256") или шестнадцатеричное ("0xC000"/"0XC000")
   число -- для io_base=/io_len= (см. include/container.h). Останавливается
   на первом нераспознанном символе, как atoi(); отдельной валидации
   "мусор после числа -- ошибка" сознательно нет, тем же самым принципом
   уже пользуется atoi() из include/string.h везде в этом дереве. */
static u32 parse_manifest_u32(const char *s) {
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        u32 v = 0;
        s += 2;
        while (*s) {
            char c = to_lower(*s);
            u32 d;
            if (c >= '0' && c <= '9') d = (u32)(c - '0');
            else if (c >= 'a' && c <= 'f') d = (u32)(c - 'a') + 10;
            else break;
            v = v * 16 + d;
            s++;
        }
        return v;
    }
    return (u32)atoi(s);
}

/* Разбирает "caps=net,fs_write" -- список через запятую, регистр не
   важен. Пустое значение (просто "caps=") -- валидно и означает
   CAP_NONE. Возвращает FALSE на неизвестном имени капабилити, ничего не
   выводя в *out_caps (частичный результат вызывающему не отдаём -- ошибка
   конфигурации должна быть всё-или-ничего, а не "запустился с
   каким-то случайно урезанным набором прав"). */
static bool_t parse_caps_list(const char *value, u32 *out_caps) {
    u32 caps = CAP_NONE;
    char buf[256];
    strncpy(buf, value, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';

    char *p = buf;
    while (*p) {
        while (*p == ' ' || *p == '\t' || *p == ',') p++;
        if (!*p) break;

        char *start = p;
        while (*p && *p != ',') p++;
        char saved = *p;
        *p = '\0';

        char tok[32];
        strncpy(tok, start, sizeof(tok) - 1);
        tok[sizeof(tok) - 1] = '\0';
        trim(tok);
        for (char *c = tok; *c; c++) *c = to_lower(*c);

        if (tok[0] == '\0') {
            /* пусто между запятыми ("net,,shm") -- просто пропускаем */
        } else if (strcmp(tok, "net") == 0) {
            caps |= CAP_NET;
        } else if (strcmp(tok, "fs_write") == 0) {
            caps |= CAP_FS_WRITE;
        } else if (strcmp(tok, "fs_read") == 0) {
            caps |= CAP_FS_READ;
        } else if (strcmp(tok, "ps") == 0) {
            caps |= CAP_PS;
        } else if (strcmp(tok, "users") == 0) {
            caps |= CAP_USERS;
        } else if (strcmp(tok, "power") == 0) {
            caps |= CAP_POWER;
        } else if (strcmp(tok, "sysinfo") == 0) {
            caps |= CAP_SYSINFO;
        } else if (strcmp(tok, "shm") == 0) {
            caps |= CAP_SHM;
        } else if (strcmp(tok, "spawn") == 0) {
            caps |= CAP_SPAWN;
        } else if (strcmp(tok, "io") == 0) {
            caps |= CAP_IO;
        } else if (strcmp(tok, "irq") == 0) {
            caps |= CAP_IRQ;
        } else if (strcmp(tok, "net_bridge") == 0) {
            caps |= CAP_NET_BRIDGE;
        } else if (strcmp(tok, "disk_raw") == 0) {
            caps |= CAP_DISK_RAW;
        } else if (strcmp(tok, "all") == 0) {
            caps |= CAP_ALL;
        } else if (strcmp(tok, "none") == 0) {
            /* явное "ничего" -- ничего не добавляем, но и не ошибка */
        } else {
            kprintf("container: unknown capability '%s' in manifest\n", tok);
            return FALSE;
        }

        if (saved == '\0') break;
        p++; /* пропускаем запятую */
    }

    *out_caps = caps;
    return TRUE;
}

bool_t container_manifest_parse(const char *text, u32 len, container_manifest_t *out) {
    if (!text || !out) return FALSE;

    container_manifest_t m;
    memset(&m, 0, sizeof(m));
    m.caps = CAP_NONE; /* безопасный дефолт, если 'caps=' вообще не встретится */
    m.irq_line = CONTAINER_IRQ_NONE; /* 0 -- валидный номер линии (таймер!), нельзя оставлять как "дефолт после memset" */
    bool_t have_path = FALSE;

    /* Копируем в изменяемый, гарантированно NUL-терминированный буфер --
       text/len (как из fs_read_file()) не обязаны быть NUL-терминированы,
       а strchr()/трим() ниже этого требуют. */
    if (len >= CONTAINER_MANIFEST_MAX_SIZE) {
        kprintf("container: manifest too large (%u bytes)\n", len);
        return FALSE;
    }
    char *buf = (char *)kmalloc(len + 1);
    if (!buf) return FALSE;
    memcpy(buf, text, len);
    buf[len] = '\0';

    char *line = buf;
    bool_t ok = TRUE;
    while (line && *line) {
        char *nl = strchr(line, '\n');
        if (nl) *nl = '\0';

        char *raw = line;
        line = nl ? nl + 1 : 0;

        trim(raw);
        if (raw[0] == '\0' || raw[0] == '#') continue; /* пустая строка/комментарий */

        char *eq = strchr(raw, '=');
        if (!eq) {
            kprintf("container: malformed manifest line (no '='): %s\n", raw);
            ok = FALSE;
            break;
        }
        *eq = '\0';
        char *key = raw;
        char *value = eq + 1;
        trim(key);
        trim(value);
        for (char *c = key; *c; c++) *c = to_lower(*c);

        if (strcmp(key, "name") == 0) {
            strncpy(m.name, value, CONTAINER_NAME_MAX - 1);
            m.name[CONTAINER_NAME_MAX - 1] = '\0';
        } else if (strcmp(key, "path") == 0) {
            strncpy(m.path, value, CONTAINER_PATH_MAX - 1);
            m.path[CONTAINER_PATH_MAX - 1] = '\0';
            have_path = TRUE;
        } else if (strcmp(key, "caps") == 0) {
            if (!parse_caps_list(value, &m.caps)) { ok = FALSE; break; }
        } else if (strcmp(key, "io_base") == 0) {
            m.io_base = (u16)parse_manifest_u32(value);
        } else if (strcmp(key, "io_len") == 0) {
            m.io_len = (u16)parse_manifest_u32(value);
        } else if (strcmp(key, "irq") == 0) {
            m.irq_line = (u8)parse_manifest_u32(value);
        } else {
            kprintf("container: unknown manifest key '%s'\n", key);
            ok = FALSE;
            break;
        }
    }

    kfree(buf);
    if (!ok) return FALSE;

    if (!have_path) {
        kprintf("container: manifest is missing required 'path='\n");
        return FALSE;
    }
    if (m.name[0] == '\0') {
        /* 'name' необязателен -- по умолчанию берём 'path', как имя
           процесса в 'ps' (см. task_create_process()) для запуска без
           контейнера. */
        strncpy(m.name, m.path, CONTAINER_NAME_MAX - 1);
        m.name[CONTAINER_NAME_MAX - 1] = '\0';
    }

    if (!(m.caps & CAP_IO)) {
        /* caps -- источник истины: 'io_base='/'io_len=' в файле без
           'caps=io' ни на что не влияют, а не "почти работают". Так же,
           как gdt_apply_io_permissions() трактует io_len == 0 как полный
           запрет (см. include/gdt.h), а не "нет диапазона, но раз он не
           нулевой -- может, дать доступ". */
        m.io_base = 0;
        m.io_len = 0;
    }

    if (!(m.caps & CAP_IRQ)) {
        m.irq_line = CONTAINER_IRQ_NONE; /* тот же принцип, что и у io_base/io_len выше */
    } else if (m.irq_line > 15) {
        kprintf("container: manifest requests invalid irq line %u (must be 0-15)\n", m.irq_line);
        return FALSE;
    }

    *out = m;
    return TRUE;
}

bool_t container_manifest_load(const char *manifest_path, container_manifest_t *out) {
    if (!manifest_path || !out) return FALSE;

    char *buf = (char *)kmalloc(CONTAINER_MANIFEST_MAX_SIZE);
    if (!buf) return FALSE;

    u32 size = 0;
    if (!fs_read_file(manifest_path, buf, CONTAINER_MANIFEST_MAX_SIZE, &size)) {
        kprintf("container: failed to read manifest: %s\n", manifest_path);
        kfree(buf);
        return FALSE;
    }

    bool_t ok = container_manifest_parse(buf, size, out);
    kfree(buf);
    return ok;
}

s32 container_spawn(const container_manifest_t *manifest, u32 argc, char **argv) {
    if (!manifest || manifest->path[0] == '\0') return -1;

    if (fs_get_app(manifest->path) != RAMFS_APP_BINARY) {
        kprintf("container: not an executable binary: %s\n", manifest->path);
        return -1;
    }

    void *buf = kmalloc(RAMFS_FILE_DATA_SIZE);
    if (!buf) {
        kprintf("container: out of memory for read buffer\n");
        return -1;
    }

    u32 size = 0;
    if (!fs_read_file(manifest->path, buf, RAMFS_FILE_DATA_SIZE, &size) || size == 0) {
        kprintf("container: failed to read binary: %s\n", manifest->path);
        kfree(buf);
        return -1;
    }

    kprintf("container: starting '%s' (%s), caps=0x%x",
            manifest->name, manifest->path, manifest->caps);
    if (manifest->caps & CAP_IO) {
        kprintf(", io=[0x%x,0x%x)", manifest->io_base, (u32)manifest->io_base + manifest->io_len);
    }
    if (manifest->caps & CAP_IRQ) {
        kprintf(", irq=%u", manifest->irq_line);
    }
    kprintf("\n");

    s32 ret = process_run_sandboxed_caps(buf, size, argc, argv, manifest->caps,
                                          manifest->io_base, manifest->io_len, manifest->irq_line);
    kfree(buf);
    return ret;
}
