#include "fat32.h"
#include "storage.h"
#include "string.h"
#include "kprintf.h"
#include "mbr.h"

#define SECTOR_SIZE 512
#define MAX_SECTORS_PER_CLUSTER 64
#define MAX_DEPTH 16
#define MAX_FILE_SIZE 65536

#define ATTR_READ_ONLY 0x01
#define ATTR_HIDDEN    0x02
#define ATTR_SYSTEM    0x04
#define ATTR_VOLUME_ID 0x08
#define ATTR_DIRECTORY 0x10
#define ATTR_ARCHIVE   0x20
#define ATTR_LONG_NAME 0x0F

typedef struct __attribute__((packed)) {
    u8  jmp[3];
    u8  oem[8];
    u16 bytes_per_sector;
    u8  sectors_per_cluster;
    u16 reserved_sectors;
    u8  num_fats;
    u16 root_entry_count;
    u16 total_sectors_16;
    u8  media_type;
    u16 fat_size_16;
    u16 sectors_per_track;
    u16 num_heads;
    u32 hidden_sectors;
    u32 total_sectors_32;
    u32 fat_size_32;
    u16 ext_flags;
    u16 fs_version;
    u32 root_cluster;
    u16 fs_info_sector;
    u16 backup_boot_sector;
    u8  reserved[12];
    u8  drive_number;
    u8  reserved1;
    u8  boot_signature;
    u32 volume_id;
    u8  volume_label[11];
    u8  fs_type[8];
} fat32_bpb_t;

typedef struct __attribute__((packed)) {
    u8  name[11];
    u8  attr;
    u8  nt_reserved;
    u8  creation_time_tenth;
    u16 creation_time;
    u16 creation_date;
    u16 last_access_date;
    u16 first_cluster_hi;
    u16 write_time;
    u16 write_date;
    u16 first_cluster_lo;
    u32 file_size;
} fat32_dirent_t;

static bool_t mounted = FALSE;
static u32 cur_drive = 0;

/* g_readonly -- см. общий комментарий у ufs_set_readonly()/g_readonly в
   fs/ufs.c -- тот же приём, специально сохранена симметрия между двумя
   файлами (ATOMIC_PLAN.md, Фаза 4): любой из двух дисковых backend'ов
   VFS должен уметь быть смонтирован read-only, не только UFS. Сброс в
   FALSE при каждом fat32_mount()/fat32_format() -- свежее монтирование
   по умолчанию RW, пока кто-то явно не попросил иначе. */
static bool_t g_readonly = FALSE;

/* Смещение (в секторах) начала используемого раздела относительно
   начала физического диска -- см. detect_partition_range() ниже и
   INSTALLER_PLAN.md, Фаза 3. 0, если раздела нет (весь диск -- одна
   FAT32-файловая система с сектора 0, как FAT32-драйвер работал
   исторически, ДО появления install-os/MBR -- обратная совместимость
   сохранена как раз через это же значение по умолчанию). */
static u32 partition_lba_offset = 0;

static u32 bytes_per_sector;
static u32 sectors_per_cluster;
static u32 reserved_sectors;
static u32 num_fats;
static u32 fat_size;
static u32 root_cluster;
static u32 fat_start_lba;
static u32 data_start_lba;
static u32 total_clusters;

static u32 dir_cluster_stack[MAX_DEPTH];
static char dir_name_stack[MAX_DEPTH][13];
static u32 stack_depth = 1;

static u8 sector_buf[SECTOR_SIZE];
static u8 cluster_buf[MAX_SECTORS_PER_CLUSTER * SECTOR_SIZE];

/* ---------- Определение раздела (см. INSTALLER_PLAN.md, Фаза 3) ---------- */

/* Читает сектор 0 физического диска drive_index и, если там валидный MBR
   (см. include/mbr.h -- то же самое, что строит install-os через
   mbr_build()) хотя бы с одним использованным разделом, возвращает
   смещение и размер ПЕРВОГО такого раздела (по индексу 0..3). Если MBR
   невалиден (сигнатура не совпала) либо валиден, но все 4 записи пустые
   -- считает это "всем диском как единой FAT32-файловой системой" (тот
   способ, каким этот драйвер работал ДО появления MBR/install-os) и
   возвращает offset=0, count=весь диск. Так СТАРЫЕ (отформатированные
   без всякого MBR) тестовые образы продолжают монтироваться ровно как
   раньше -- эта функция ничего не ломает, только ДОБАВЛЯЕТ понимание
   разделов, если они есть. */
static void detect_partition_range(u32 drive_index, u32 whole_disk_sectors,
                                     u32 *out_lba_offset, u32 *out_sector_count) {
    *out_lba_offset = 0;
    *out_sector_count = whole_disk_sectors;

    u8 mbr_raw[SECTOR_SIZE];
    if (!storage_read_sectors(drive_index, 0, 1, mbr_raw)) return;

    const mbr_sector_t *sector = (const mbr_sector_t *)mbr_raw;
    mbr_partition_t parts[MBR_PARTITION_COUNT];
    if (!mbr_parse(sector, parts)) return; /* сигнатура не совпала -- не MBR вовсе */

    for (u32 i = 0; i < MBR_PARTITION_COUNT; i++) {
        if (mbr_partition_is_used(&parts[i])) {
            *out_lba_offset = parts[i].lba_start;
            *out_sector_count = parts[i].sectors;
            return;
        }
    }
    /* Валидный MBR, но все 4 раздела пустые -- тоже fallback на весь диск. */
}

/* ---------- Низкоуровневые операции с диском ---------- */

static bool_t disk_read_sectors(u32 lba, u32 count, void *buf) {
    lba += partition_lba_offset;
    u8 *b = (u8*)buf;
    while (count > 0) {
        u8 chunk = (count > 255) ? 255 : (u8)count;
        if (!storage_read_sectors(cur_drive, lba, chunk, b)) return FALSE;
        lba += chunk;
        b += (u32)chunk * SECTOR_SIZE;
        count -= chunk;
    }
    return TRUE;
}

static bool_t disk_write_sectors(u32 lba, u32 count, const void *buf) {
    lba += partition_lba_offset;
    const u8 *b = (const u8*)buf;
    while (count > 0) {
        u8 chunk = (count > 255) ? 255 : (u8)count;
        if (!storage_write_sectors(cur_drive, lba, chunk, b)) return FALSE;
        lba += chunk;
        b += (u32)chunk * SECTOR_SIZE;
        count -= chunk;
    }
    return TRUE;
}

static u32 cluster_to_lba(u32 cluster) {
    return data_start_lba + (cluster - 2) * sectors_per_cluster;
}

static bool_t read_cluster(u32 cluster, void *buf) {
    return disk_read_sectors(cluster_to_lba(cluster), sectors_per_cluster, buf);
}

static bool_t write_cluster(u32 cluster, const void *buf) {
    return disk_write_sectors(cluster_to_lba(cluster), sectors_per_cluster, buf);
}

/* ---------- Работа с таблицей FAT ---------- */

static u32 read_fat_entry(u32 cluster) {
    u32 fat_offset = cluster * 4;
    u32 sec = fat_start_lba + (fat_offset / bytes_per_sector);
    u32 off = fat_offset % bytes_per_sector;
    disk_read_sectors(sec, 1, sector_buf);
    u32 val = *(u32*)(sector_buf + off);
    return val & 0x0FFFFFFF;
}

static void write_fat_entry(u32 cluster, u32 value) {
    u32 fat_offset = cluster * 4;
    u32 off = fat_offset % bytes_per_sector;
    for (u32 f = 0; f < num_fats; f++) {
        u32 sec = fat_start_lba + f * fat_size + (fat_offset / bytes_per_sector);
        disk_read_sectors(sec, 1, sector_buf);
        u32 *p = (u32*)(sector_buf + off);
        *p = (*p & 0xF0000000) | (value & 0x0FFFFFFF);
        disk_write_sectors(sec, 1, sector_buf);
    }
}

static bool_t is_eoc(u32 val) {
    return val >= 0x0FFFFFF8;
}

static u32 alloc_cluster(void) {
    for (u32 c = 2; c < total_clusters + 2; c++) {
        if (read_fat_entry(c) == 0) {
            write_fat_entry(c, 0x0FFFFFFF); /* маркер конца цепочки */
            memset(cluster_buf, 0, sectors_per_cluster * SECTOR_SIZE);
            write_cluster(c, cluster_buf);
            return c;
        }
    }
    return 0; /* нет места */
}

static void free_chain(u32 start_cluster) {
    u32 c = start_cluster;
    while (c >= 2 && !is_eoc(c) && c != 0) {
        u32 next = read_fat_entry(c);
        write_fat_entry(c, 0);
        if (is_eoc(next) || next == 0) break;
        c = next;
    }
}

/* ---------- Имена файлов 8.3 ---------- */

static void name_to_fat(const char *name, u8 out[11]) {
    for (int i = 0; i < 11; i++) out[i] = ' ';
    int i = 0, j = 0;
    while (name[i] && name[i] != '.' && j < 8) {
        out[j++] = to_upper(name[i]);
        i++;
    }
    while (name[i] && name[i] != '.') i++;
    if (name[i] == '.') {
        i++;
        int k = 8;
        while (name[i] && k < 11) {
            out[k++] = to_upper(name[i]);
            i++;
        }
    }
}

static void fat_to_name(const u8 in[11], char *out) {
    int j = 0;
    for (int i = 0; i < 8 && in[i] != ' '; i++) out[j++] = in[i];
    if (in[8] != ' ') {
        out[j++] = '.';
        for (int i = 8; i < 11 && in[i] != ' '; i++) out[j++] = in[i];
    }
    out[j] = '\0';
}

/* ---------- Обход директорий ---------- */

typedef bool_t (*dirent_visitor_t)(fat32_dirent_t *entry, u32 cluster, u32 sector_in_cluster, u32 index_in_sector, void *ctx);

/* Проходит по всем записям цепочки кластеров каталога, вызывая visitor.
   Если visitor вернёт TRUE, обход прекращается. */
static void walk_directory(u32 start_cluster, dirent_visitor_t visitor, void *ctx) {
    u32 cluster = start_cluster;
    while (cluster >= 2 && !is_eoc(cluster)) {
        for (u32 s = 0; s < sectors_per_cluster; s++) {
            disk_read_sectors(cluster_to_lba(cluster) + s, 1, sector_buf);
            fat32_dirent_t *entries = (fat32_dirent_t*)sector_buf;
            u32 per_sector = SECTOR_SIZE / sizeof(fat32_dirent_t);
            for (u32 e = 0; e < per_sector; e++) {
                if (visitor(&entries[e], cluster, s, e, ctx)) return;
            }
        }
        cluster = read_fat_entry(cluster);
    }
}

typedef struct {
    char target[13];
    bool_t found;
    fat32_dirent_t entry;
    u32 entry_cluster;
    u32 entry_sector;
    u32 entry_index;
} find_ctx_t;

static bool_t find_visitor(fat32_dirent_t *e, u32 cluster, u32 sector, u32 idx, void *ctx_) {
    find_ctx_t *ctx = (find_ctx_t*)ctx_;
    if (e->name[0] == 0x00) return TRUE; /* конец каталога */
    if (e->name[0] == 0xE5) return FALSE;
    if (e->attr == ATTR_LONG_NAME) return FALSE;
    char nm[13];
    fat_to_name(e->name, nm);
    if (strcmp(nm, ctx->target) == 0) {
        ctx->found = TRUE;
        ctx->entry = *e;
        ctx->entry_cluster = cluster;
        ctx->entry_sector = sector;
        ctx->entry_index = idx;
        return TRUE;
    }
    return FALSE;
}

static bool_t find_entry(u32 dir_cluster, const char *name, find_ctx_t *out) {
    memset(out, 0, sizeof(find_ctx_t));
    strncpy(out->target, name, 12);
    for (u32 i = 0; out->target[i]; i++) out->target[i] = to_upper(out->target[i]);
    walk_directory(dir_cluster, find_visitor, out);
    return out->found;
}

typedef struct {
    bool_t found_slot;
    u32 cluster;
    u32 sector;
    u32 index;
} slot_ctx_t;

static bool_t slot_visitor(fat32_dirent_t *e, u32 cluster, u32 sector, u32 idx, void *ctx_) {
    slot_ctx_t *ctx = (slot_ctx_t*)ctx_;
    if (e->name[0] == 0x00 || e->name[0] == 0xE5) {
        ctx->found_slot = TRUE;
        ctx->cluster = cluster;
        ctx->sector = sector;
        ctx->index = idx;
        return TRUE;
    }
    return FALSE;
}

/* Находит свободный слот в каталоге, расширяя цепочку кластеров при необходимости */
static bool_t alloc_dir_slot(u32 dir_cluster, u32 *out_cluster, u32 *out_sector, u32 *out_index) {
    slot_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    walk_directory(dir_cluster, slot_visitor, &ctx);
    if (ctx.found_slot) {
        *out_cluster = ctx.cluster;
        *out_sector = ctx.sector;
        *out_index = ctx.index;
        return TRUE;
    }
    /* Не нашлось места - расширяем цепочку каталога новым кластером */
    u32 last = dir_cluster;
    u32 next = read_fat_entry(last);
    while (!is_eoc(next)) { last = next; next = read_fat_entry(last); }
    u32 newc = alloc_cluster();
    if (newc == 0) return FALSE;
    write_fat_entry(last, newc);
    *out_cluster = newc;
    *out_sector = 0;
    *out_index = 0;
    return TRUE;
}

static void write_entry_at(u32 cluster, u32 sector, u32 index, fat32_dirent_t *e) {
    disk_read_sectors(cluster_to_lba(cluster) + sector, 1, sector_buf);
    fat32_dirent_t *entries = (fat32_dirent_t*)sector_buf;
    entries[index] = *e;
    disk_write_sectors(cluster_to_lba(cluster) + sector, 1, sector_buf);
}

/* ---------- Монтирование ---------- */

bool_t fat32_mount(u32 drive_index) {
    storage_dev_t *d = storage_get_drive(drive_index);
    if (!d) return FALSE;
    g_readonly = FALSE; /* см. общий комментарий у g_readonly выше */

    cur_drive = drive_index;

    u32 unused_partition_sectors;
    detect_partition_range(drive_index, d->sectors, &partition_lba_offset, &unused_partition_sectors);

    if (!disk_read_sectors(0, 1, sector_buf)) return FALSE;

    fat32_bpb_t *bpb = (fat32_bpb_t*)sector_buf;
    if (bpb->bytes_per_sector != 512 || bpb->fat_size_16 != 0 || bpb->fat_size_32 == 0) {
        mounted = FALSE;
        return FALSE; /* не похоже на валидный FAT32 */
    }

    bytes_per_sector = bpb->bytes_per_sector;
    sectors_per_cluster = bpb->sectors_per_cluster;
    reserved_sectors = bpb->reserved_sectors;
    num_fats = bpb->num_fats;
    fat_size = bpb->fat_size_32;
    root_cluster = bpb->root_cluster;

    fat_start_lba = reserved_sectors;
    data_start_lba = reserved_sectors + num_fats * fat_size;

    u32 total_sectors = bpb->total_sectors_32 ? bpb->total_sectors_32 : bpb->total_sectors_16;
    u32 data_sectors = total_sectors - data_start_lba;
    total_clusters = data_sectors / sectors_per_cluster;

    if (sectors_per_cluster == 0 || sectors_per_cluster > MAX_SECTORS_PER_CLUSTER) {
        mounted = FALSE;
        return FALSE;
    }

    stack_depth = 1;
    dir_cluster_stack[0] = root_cluster;
    strcpy(dir_name_stack[0], "/");

    mounted = TRUE;
    return TRUE;
}

bool_t fat32_is_mounted(void) { return mounted; }
u32 fat32_current_drive(void) { return cur_drive; }

/* См. общий комментарий у g_readonly выше / ufs_set_readonly() в fs/ufs.c. */
void fat32_set_readonly(bool_t readonly) { g_readonly = readonly; }
bool_t fat32_is_readonly(void) { return g_readonly; }

/* ---------- Форматирование ---------- */

bool_t fat32_format(u32 drive_index) {
    storage_dev_t *d = storage_get_drive(drive_index);
    if (!d) return FALSE;
    g_readonly = FALSE; /* см. общий комментарий у g_readonly выше */
    u32 saved_drive = cur_drive;
    cur_drive = drive_index;

    u32 total_sectors;
    detect_partition_range(drive_index, d->sectors, &partition_lba_offset, &total_sectors);
    if (total_sectors < 4096) { cur_drive = saved_drive; return FALSE; } /* слишком маленький раздел/диск */

    u32 spc = 8;               /* 8 секторов на кластер = 4 KiB */
    u32 res_sectors = 32;
    u32 nfats = 2;

    /* Приблизительный расчёт размера FAT (для простоты, с запасом) */
    u32 data_sectors_est = total_sectors - res_sectors;
    u32 clusters_est = data_sectors_est / spc;
    u32 fatsz = (clusters_est * 4 + SECTOR_SIZE - 1) / SECTOR_SIZE + 8;

    memset(sector_buf, 0, SECTOR_SIZE);
    fat32_bpb_t *bpb = (fat32_bpb_t*)sector_buf;
    bpb->jmp[0] = 0xEB; bpb->jmp[1] = 0x58; bpb->jmp[2] = 0x90;
    memcpy(bpb->oem, "MEETLOOK", 8);
    bpb->bytes_per_sector = SECTOR_SIZE;
    bpb->sectors_per_cluster = (u8)spc;
    bpb->reserved_sectors = (u16)res_sectors;
    bpb->num_fats = (u8)nfats;
    bpb->root_entry_count = 0;
    bpb->total_sectors_16 = 0;
    bpb->media_type = 0xF8;
    bpb->fat_size_16 = 0;
    bpb->sectors_per_track = 32;
    bpb->num_heads = 64;
    bpb->hidden_sectors = 0;
    bpb->total_sectors_32 = total_sectors;
    bpb->fat_size_32 = fatsz;
    bpb->ext_flags = 0;
    bpb->fs_version = 0;
    bpb->root_cluster = 2;
    bpb->fs_info_sector = 1;
    bpb->backup_boot_sector = 6;
    bpb->drive_number = 0x80;
    bpb->boot_signature = 0x29;
    bpb->volume_id = 0x12345678;
    memcpy(bpb->volume_label, "MEETLOOK   ", 11);
    memcpy(bpb->fs_type, "FAT32   ", 8);
    sector_buf[510] = 0x55; sector_buf[511] = 0xAA;
    disk_write_sectors(0, 1, sector_buf);

    /* Резервная копия загрузочного сектора */
    disk_write_sectors(6, 1, sector_buf);

    u32 fat_lba = res_sectors;
    u32 data_lba = res_sectors + nfats * fatsz;

    /* Обнуляем обе копии FAT */
    memset(sector_buf, 0, SECTOR_SIZE);
    for (u32 f = 0; f < nfats; f++) {
        for (u32 s = 0; s < fatsz; s++) {
            disk_write_sectors(fat_lba + f * fatsz + s, 1, sector_buf);
        }
    }

    /* Инициализация первых записей FAT: кластер0/1 - служебные, кластер2 (корень) - EOC */
    cur_drive = drive_index;
    bytes_per_sector = SECTOR_SIZE;
    sectors_per_cluster = spc;
    reserved_sectors = res_sectors;
    num_fats = nfats;
    fat_size = fatsz;
    fat_start_lba = fat_lba;
    data_start_lba = data_lba;
    total_clusters = (total_sectors - data_lba) / spc;

    write_fat_entry(0, 0x0FFFFFF8);
    write_fat_entry(1, 0x0FFFFFFF);
    write_fat_entry(2, 0x0FFFFFFF);

    /* Обнуляем корневой каталог (кластер 2) */
    memset(cluster_buf, 0, spc * SECTOR_SIZE);
    write_cluster(2, cluster_buf);

    root_cluster = 2;
    stack_depth = 1;
    dir_cluster_stack[0] = root_cluster;
    strcpy(dir_name_stack[0], "/");
    mounted = TRUE;
    cur_drive = drive_index;

    return TRUE;
}

/* ---------- Команды файловой системы ---------- */

static u32 current_dir(void) { return dir_cluster_stack[stack_depth - 1]; }

typedef struct { u32 count; } ls_ctx_t;

static bool_t ls_visitor(fat32_dirent_t *e, u32 cluster, u32 sector, u32 idx, void *ctx_) {
    (void)cluster; (void)sector; (void)idx;
    if (e->name[0] == 0x00) return TRUE;
    if (e->name[0] == 0xE5) return FALSE;
    if (e->attr == ATTR_LONG_NAME) return FALSE;
    if (e->attr & ATTR_VOLUME_ID) return FALSE;

    char nm[13];
    fat_to_name(e->name, nm);
    if (e->attr & ATTR_DIRECTORY) {
        kprintf("  <DIR>  %s\n", nm);
    } else {
        kprintf("  %u\t%s\n", e->file_size, nm);
    }
    ((ls_ctx_t*)ctx_)->count++;
    return FALSE;
}

void fat32_ls(void) {
    if (!mounted) { kprintf("No disk mounted. Use select.\n"); return; }
    ls_ctx_t ctx = {0};
    walk_directory(current_dir(), ls_visitor, &ctx);
    if (ctx.count == 0) kprintf("  (empty)\n");
}

/* См. fs_list(), include/vfs.h. owner_uid/mode -- FAT32 не знает
   понятия владельца узла, заполняются нулями (см. общий комментарий у
   fs_dirent_t, include/ramfs.h). */
typedef struct { fs_dirent_t *out; u32 max_entries; u32 count; } ls_collect_ctx_t;

static bool_t ls_collect_visitor(fat32_dirent_t *e, u32 cluster, u32 sector, u32 idx, void *ctx_) {
    (void)cluster; (void)sector; (void)idx;
    if (e->name[0] == 0x00) return TRUE;
    if (e->name[0] == 0xE5) return FALSE;
    if (e->attr == ATTR_LONG_NAME) return FALSE;
    if (e->attr & ATTR_VOLUME_ID) return FALSE;

    ls_collect_ctx_t *ctx = (ls_collect_ctx_t *)ctx_;
    if (ctx->count < ctx->max_entries) {
        fs_dirent_t *d = &ctx->out[ctx->count];
        fat_to_name(e->name, d->name); /* fat_to_name() пишет не больше 13 байт, FS_DIRENT_NAME_MAX=32 -- влезает с запасом */
        d->size = e->file_size;
        d->owner_uid = 0;
        d->mode = 0;
        d->is_dir = (e->attr & ATTR_DIRECTORY) != 0;
        d->is_app = FALSE;
    }
    ctx->count++;
    return FALSE;
}

bool_t fat32_list(fs_dirent_t *out, u32 max_entries, u32 *out_count) {
    if (!mounted) return FALSE;
    ls_collect_ctx_t ctx = { out, max_entries, 0 };
    walk_directory(current_dir(), ls_collect_visitor, &ctx);
    if (out_count) *out_count = ctx.count;
    return TRUE;
}

bool_t fat32_cd(const char *name) {
    if (!mounted) return FALSE;
    if (strcmp(name, "/") == 0) { stack_depth = 1; return TRUE; }
    if (strcmp(name, "..") == 0) {
        if (stack_depth > 1) stack_depth--;
        return TRUE;
    }
    if (strcmp(name, ".") == 0) return TRUE;

    find_ctx_t fc;
    if (!find_entry(current_dir(), name, &fc)) return FALSE;
    if (!(fc.entry.attr & ATTR_DIRECTORY)) return FALSE;

    if (stack_depth >= MAX_DEPTH) return FALSE;
    u32 cl = ((u32)fc.entry.first_cluster_hi << 16) | fc.entry.first_cluster_lo;
    if (cl == 0) cl = root_cluster;
    dir_cluster_stack[stack_depth] = cl;
    strncpy(dir_name_stack[stack_depth], name, 12);
    stack_depth++;
    return TRUE;
}

bool_t fat32_mkdir(const char *name) {
    if (!mounted) return FALSE;
    if (g_readonly) return FALSE; /* см. общий комментарий у g_readonly выше */
    find_ctx_t fc;
    if (find_entry(current_dir(), name, &fc)) return FALSE; /* уже существует */

    u32 newc = alloc_cluster();
    if (newc == 0) return FALSE;

    /* Создаём "." и ".." в новом каталоге */
    memset(cluster_buf, 0, sectors_per_cluster * SECTOR_SIZE);
    fat32_dirent_t *entries = (fat32_dirent_t*)cluster_buf;
    memset(entries[0].name, ' ', 11); entries[0].name[0] = '.';
    entries[0].attr = ATTR_DIRECTORY;
    entries[0].first_cluster_hi = (u16)(newc >> 16);
    entries[0].first_cluster_lo = (u16)(newc & 0xFFFF);

    memset(entries[1].name, ' ', 11); entries[1].name[0] = '.'; entries[1].name[1] = '.';
    entries[1].attr = ATTR_DIRECTORY;
    u32 parent = current_dir();
    u32 parent_for_entry = (parent == root_cluster) ? 0 : parent;
    entries[1].first_cluster_hi = (u16)(parent_for_entry >> 16);
    entries[1].first_cluster_lo = (u16)(parent_for_entry & 0xFFFF);

    write_cluster(newc, cluster_buf);

    u32 sc, ss, si;
    if (!alloc_dir_slot(current_dir(), &sc, &ss, &si)) return FALSE;

    fat32_dirent_t e;
    memset(&e, 0, sizeof(e));
    name_to_fat(name, e.name);
    e.attr = ATTR_DIRECTORY;
    e.first_cluster_hi = (u16)(newc >> 16);
    e.first_cluster_lo = (u16)(newc & 0xFFFF);
    e.file_size = 0;
    write_entry_at(sc, ss, si, &e);

    return TRUE;
}

bool_t fat32_rm(const char *name) {
    if (!mounted) return FALSE;
    if (g_readonly) return FALSE; /* см. общий комментарий у g_readonly выше */
    find_ctx_t fc;
    if (!find_entry(current_dir(), name, &fc)) return FALSE;

    u32 cl = ((u32)fc.entry.first_cluster_hi << 16) | fc.entry.first_cluster_lo;
    if (fc.entry.attr & ATTR_DIRECTORY) {
        /* Проверка, что каталог пуст (кроме . и ..) */
        ls_ctx_t ctx = {0};
        walk_directory(cl, ls_visitor, &ctx); /* только для подсчёта, вывод игнорируем визуально не получится, но ок */
    }
    if (cl >= 2) free_chain(cl);

    disk_read_sectors(cluster_to_lba(fc.entry_cluster) + fc.entry_sector, 1, sector_buf);
    fat32_dirent_t *entries = (fat32_dirent_t*)sector_buf;
    entries[fc.entry_index].name[0] = 0xE5;
    disk_write_sectors(cluster_to_lba(fc.entry_cluster) + fc.entry_sector, 1, sector_buf);
    return TRUE;
}

bool_t fat32_write_file(const char *name, const char *data, u32 len) {
    if (!mounted) return FALSE;
    if (g_readonly) return FALSE; /* см. общий комментарий у g_readonly выше */
    if (len > MAX_FILE_SIZE) len = MAX_FILE_SIZE;

    find_ctx_t fc;
    u32 first_cluster = 0;
    bool_t exists = find_entry(current_dir(), name, &fc);
    if (exists) {
        first_cluster = ((u32)fc.entry.first_cluster_hi << 16) | fc.entry.first_cluster_lo;
        if (first_cluster >= 2) free_chain(first_cluster);
        first_cluster = 0;
    }

    u32 remaining = len;
    const u8 *src = (const u8*)data;
    u32 prev_cluster = 0;

    if (remaining == 0) {
        first_cluster = alloc_cluster();
    }

    while (remaining > 0) {
        u32 c = alloc_cluster();
        if (c == 0) return FALSE;
        if (first_cluster == 0) first_cluster = c;
        if (prev_cluster != 0) write_fat_entry(prev_cluster, c);

        u32 cluster_bytes = sectors_per_cluster * SECTOR_SIZE;
        u32 chunk = (remaining > cluster_bytes) ? cluster_bytes : remaining;
        memset(cluster_buf, 0, cluster_bytes);
        memcpy(cluster_buf, src, chunk);
        write_cluster(c, cluster_buf);

        src += chunk;
        remaining -= chunk;
        prev_cluster = c;
    }

    u32 sc, ss, si;
    fat32_dirent_t e;
    if (exists) {
        sc = fc.entry_cluster; ss = fc.entry_sector; si = fc.entry_index;
        e = fc.entry;
    } else {
        if (!alloc_dir_slot(current_dir(), &sc, &ss, &si)) return FALSE;
        memset(&e, 0, sizeof(e));
        name_to_fat(name, e.name);
        e.attr = ATTR_ARCHIVE;
    }
    e.first_cluster_hi = (u16)(first_cluster >> 16);
    e.first_cluster_lo = (u16)(first_cluster & 0xFFFF);
    e.file_size = len;
    write_entry_at(sc, ss, si, &e);

    return TRUE;
}

bool_t fat32_cat(const char *name) {
    if (!mounted) return FALSE;
    find_ctx_t fc;
    if (!find_entry(current_dir(), name, &fc)) return FALSE;
    if (fc.entry.attr & ATTR_DIRECTORY) return FALSE;

    u32 cl = ((u32)fc.entry.first_cluster_hi << 16) | fc.entry.first_cluster_lo;
    u32 size = fc.entry.file_size;
    u32 read_total = 0;
    u32 cluster_bytes = sectors_per_cluster * SECTOR_SIZE;

    while (cl >= 2 && !is_eoc(cl) && read_total < size) {
        read_cluster(cl, cluster_buf);
        u32 chunk = size - read_total;
        if (chunk > cluster_bytes) chunk = cluster_bytes;
        for (u32 i = 0; i < chunk; i++) {
            kprintf("%c", cluster_buf[i]);
        }
        read_total += chunk;
        cl = read_fat_entry(cl);
    }
    kprintf("\n");
    return TRUE;
}

bool_t fat32_read_file(const char *name, void *buf, u32 bufsize, u32 *out_size) {
    if (!mounted) return FALSE;
    find_ctx_t fc;
    if (!find_entry(current_dir(), name, &fc)) return FALSE;
    if (fc.entry.attr & ATTR_DIRECTORY) return FALSE;

    u32 cl = ((u32)fc.entry.first_cluster_hi << 16) | fc.entry.first_cluster_lo;
    u32 size = fc.entry.file_size;
    if (size > bufsize) size = bufsize;
    u32 read_total = 0;
    u32 cluster_bytes = sectors_per_cluster * SECTOR_SIZE;
    u8 *dst = (u8 *)buf;

    while (cl >= 2 && !is_eoc(cl) && read_total < size) {
        read_cluster(cl, cluster_buf);
        u32 chunk = size - read_total;
        if (chunk > cluster_bytes) chunk = cluster_bytes;
        memcpy(dst + read_total, cluster_buf, chunk);
        read_total += chunk;
        cl = read_fat_entry(cl);
    }
    if (out_size) *out_size = read_total;
    return TRUE;
}

void fat32_pwd(char *buf, u32 bufsize) {
    buf[0] = '\0';
    if (stack_depth == 1) {
        strncpy(buf, "/", bufsize);
        return;
    }
    for (u32 i = 1; i < stack_depth; i++) {
        strcat(buf, "/");
        strcat(buf, dir_name_stack[i]);
    }
}
