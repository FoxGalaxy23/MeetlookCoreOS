#include "ufs.h"
#include "storage.h"
#include "string.h"
#include "kprintf.h"
#include "mbr.h"
#include "cryptoblk.h"

#define SECTOR_SIZE       512
#define SECTORS_PER_BLOCK 8               /* 8 секторов = 4 KiB на блок, как у fat32.c (spc=8) */
#define BLOCK_SIZE        (SECTOR_SIZE * SECTORS_PER_BLOCK)
#define MAX_BLOCK_SIZE    4096            /* верхняя граница для статических буферов */

#define MAX_DEPTH   16
#define UFS_MAGIC   0x3546554Du           /* маркер суперблока этого драйвера ("U5F", в little-endian) */

#define UFS_NUM_DIRECT 12                 /* прямые указатели на блоки данных, как в классическом UFS/ext2 */
#define INODE_SIZE     64

#define UFS_MODE_FREE 0
#define UFS_MODE_FILE 1
#define UFS_MODE_DIR  2

#define UFS_NAME_MAX        27
#define UFS_DIRENT_DELETED  0xFFFFFFFFu   /* удалённая, но переиспользуемая запись каталога (аналог 0xE5 в FAT) */

/* ---------- Формат структур на диске ---------- */

typedef struct __attribute__((packed)) {
    u32 magic;
    u32 block_size;          /* байт на блок */
    u32 sectors_per_block;
    u32 total_blocks;
    u32 inode_count;
    u32 inode_table_start;   /* номер блока */
    u32 inode_table_blocks;
    u32 bitmap_start;        /* номер блока */
    u32 bitmap_blocks;
    u32 data_start;          /* номер блока, с которого начинаются блоки данных */
    u32 root_inode;          /* всегда 1 */
    u8  volume_label[16];
} ufs_superblock_t;

/* inode не хранит имя -- оно живёт в записи каталога. Прямые указатели
   (direct) покрывают маленькие файлы без обращения к диску за косвенным
   блоком; одного indirect-блока (массив u32-указателей) хватает на файлы
   размером в несколько мегабайт -- этого более чем достаточно для этой
   игрушечной ОС, а двойная/тройная косвенность (как в настоящем UFS) уже
   избыточна. */
typedef struct __attribute__((packed)) {
    u16 mode;                /* UFS_MODE_FREE / UFS_MODE_FILE / UFS_MODE_DIR */
    u16 reserved;
    u32 size;                /* размер файла в байтах (для каталогов не используется) */
    u32 direct[UFS_NUM_DIRECT];
    u32 indirect;            /* блок с ещё UFS_NUM_DIRECT..(block_size/4-1) указателями, либо 0 */
    u32 reserved2;
} ufs_inode_t;                /* ровно 64 байта */

typedef struct __attribute__((packed)) {
    u32 inode;                /* 0 = слот никогда не использовался (конец каталога, как 0x00 в FAT) */
                               /* UFS_DIRENT_DELETED = слот освобождён, можно переиспользовать */
    char name[UFS_NAME_MAX + 1];
} ufs_dirent_t;                /* ровно 32 байта */

/* ---------- Состояние смонтированной ФС ---------- */

static bool_t mounted = FALSE;
static u32 cur_drive = 0;

/* ---------- Подключаемый бэкенд ввода-вывода (ATOMIC_PLAN.md, Фаза 4)
 * ----------
 *
 * По умолчанию -- прямые storage_read_sectors()/write_sectors() (как и
 * раньше, без изменений в поведении). Для монтирования ТЕЛА
 * зашифрованного пользовательского образа (include/userimg.h, Фаза 3)
 * -- ufs_mount_encrypted() ниже -- подменяет io_read_fn/io_write_fn на
 * обёртки поверх lib/cryptoblk.c (Фаза 4): весь остальной код этого
 * файла (bitmap/inode/directory-логика) ничего не знает про
 * шифрование, читает/пишет "просто секторы" через disk_read_sectors()/
 * write_sectors() ниже, как и раньше -- шифрование целиком спрятано в
 * io_read_fn/io_write_fn.
 *
 * io_ctx -- непрозрачный указатель, передаётся адаптерам как есть
 * (см. включ. cryptoblk.h за тем же приёмом) -- для дефолтного бэкенда
 * не нужен (использует cur_drive напрямую), для крипто-бэкенда
 * указывает на g_crypto_dev ниже. */
typedef bool_t (*ufs_io_read_fn)(void *io_ctx, u32 lba, u32 count, void *buf);
typedef bool_t (*ufs_io_write_fn)(void *io_ctx, u32 lba, u32 count, const void *buf);

static bool_t default_io_read(void *ctx, u32 lba, u32 count, void *buf) {
    (void)ctx; /* дефолтный бэкенд берёт диск из cur_drive, не из ctx */
    return storage_read_sectors(cur_drive, lba, (u8)count, buf); /* count тут всегда <=255 -- гарантирует внешний цикл-чанкер в disk_read_sectors() ниже */
}
static bool_t default_io_write(void *ctx, u32 lba, u32 count, const void *buf) {
    (void)ctx;
    return storage_write_sectors(cur_drive, lba, (u8)count, buf);
}

static ufs_io_read_fn  io_read_fn  = default_io_read;
static ufs_io_write_fn io_write_fn = default_io_write;
static void *io_ctx = NULL;

/* g_readonly -- см. общий комментарий у ufs_set_readonly() ниже. Раз
   отдельно от "mounted", т.к. должен переживать между операциями, но
   сбрасываться в FALSE при каждом новом ufs_mount()/ufs_format() --
   свежее монтирование по умолчанию RW, пока кто-то явно не попросил
   иначе (см. вызовы ufs_set_readonly() из fs/vfs.c). */
static bool_t g_readonly = FALSE;

/* Смещение (в секторах) начала используемого раздела относительно
   начала физического диска -- см. detect_partition_range() ниже и
   fs/fat32.c (то же самое, дословно, только продублировано здесь --
   mbr.c намеренно не занимается вводом-выводом, см. общий комментарий
   в lib/mbr.c, поэтому I/O-обёртка живёт в каждом драйвере ФС отдельно,
   ровно как disk_read_sectors/disk_write_sectors ниже уже дублируются
   между fat32.c и этим файлом). 0, если раздела нет (весь диск -- одна
   UFS-файловая система с сектора 0, обратная совместимость). */
static u32 partition_lba_offset = 0;

static u32 block_size;
static u32 sectors_per_block;
static u32 total_blocks;
static u32 inode_count;
static u32 inode_table_start;
static u32 inode_table_blocks;
static u32 bitmap_start;
static u32 bitmap_blocks;
static u32 data_start;
static u32 root_inode;

static u32  dir_inode_stack[MAX_DEPTH];
static char dir_name_stack[MAX_DEPTH][UFS_NAME_MAX + 1];
static u32  stack_depth = 1;

static u8 sb_sector_buf[SECTOR_SIZE];
static u8 zero_buf[MAX_BLOCK_SIZE];
static u8 bitmap_buf[MAX_BLOCK_SIZE];
static u8 inode_blk_buf[MAX_BLOCK_SIZE];
static u8 indirect_buf[MAX_BLOCK_SIZE];
static u8 data_buf[MAX_BLOCK_SIZE];

/* ---------- Определение раздела (см. fs/fat32.c -- дословно та же
   логика; INSTALLER_PLAN.md, Фаза 3) ---------- */

static void detect_partition_range(u32 drive_index, u32 whole_disk_sectors,
                                     u32 *out_lba_offset, u32 *out_sector_count) {
    *out_lba_offset = 0;
    *out_sector_count = whole_disk_sectors;

    u8 mbr_raw[SECTOR_SIZE];
    if (!storage_read_sectors(drive_index, 0, 1, mbr_raw)) return;

    const mbr_sector_t *sector = (const mbr_sector_t *)mbr_raw;
    mbr_partition_t parts[MBR_PARTITION_COUNT];
    if (!mbr_parse(sector, parts)) return;

    for (u32 i = 0; i < MBR_PARTITION_COUNT; i++) {
        if (mbr_partition_is_used(&parts[i])) {
            *out_lba_offset = parts[i].lba_start;
            *out_sector_count = parts[i].sectors;
            return;
        }
    }
}

/* ---------- Низкоуровневые операции с диском ---------- */

static bool_t disk_read_sectors(u32 lba, u32 count, void *buf) {
    lba += partition_lba_offset;
    u8 *b = (u8 *)buf;
    while (count > 0) {
        u8 chunk = (count > 255) ? 255 : (u8)count;
        if (!io_read_fn(io_ctx, lba, chunk, b)) return FALSE;
        lba += chunk;
        b += (u32)chunk * SECTOR_SIZE;
        count -= chunk;
    }
    return TRUE;
}

static bool_t disk_write_sectors(u32 lba, u32 count, const void *buf) {
    lba += partition_lba_offset;
    const u8 *b = (const u8 *)buf;
    while (count > 0) {
        u8 chunk = (count > 255) ? 255 : (u8)count;
        if (!io_write_fn(io_ctx, lba, chunk, b)) return FALSE;
        lba += chunk;
        b += (u32)chunk * SECTOR_SIZE;
        count -= chunk;
    }
    return TRUE;
}

static bool_t read_block(u32 block, void *buf) {
    return disk_read_sectors(block * sectors_per_block, sectors_per_block, buf);
}

static bool_t write_block(u32 block, const void *buf) {
    return disk_write_sectors(block * sectors_per_block, sectors_per_block, buf);
}

/* ---------- Битовая карта свободных блоков данных ---------- */

static bool_t bitmap_get(u32 data_bit_index) {
    u32 bits_per_block = block_size * 8;
    u32 blk = data_bit_index / bits_per_block;
    u32 bit = data_bit_index % bits_per_block;
    read_block(bitmap_start + blk, bitmap_buf);
    return (bitmap_buf[bit / 8] >> (bit % 8)) & 1;
}

static void bitmap_set(u32 data_bit_index, bool_t val) {
    u32 bits_per_block = block_size * 8;
    u32 blk = data_bit_index / bits_per_block;
    u32 bit = data_bit_index % bits_per_block;
    read_block(bitmap_start + blk, bitmap_buf);
    if (val) bitmap_buf[bit / 8] |= (u8)(1u << (bit % 8));
    else     bitmap_buf[bit / 8] &= (u8)~(1u << (bit % 8));
    write_block(bitmap_start + blk, bitmap_buf);
}

static u32 alloc_block(void) {
    u32 data_blocks = total_blocks - data_start;
    for (u32 i = 0; i < data_blocks; i++) {
        if (!bitmap_get(i)) {
            bitmap_set(i, TRUE);
            u32 blk = data_start + i;
            memset(zero_buf, 0, block_size);
            write_block(blk, zero_buf);
            return blk;
        }
    }
    return 0; /* нет места */
}

static void free_block(u32 block) {
    if (block < data_start) return;
    bitmap_set(block - data_start, FALSE);
}

/* ---------- inode'ы ---------- */

static void inode_location(u32 inum, u32 *out_block, u32 *out_offset) {
    u32 idx = inum - 1;
    u32 per_block = block_size / INODE_SIZE;
    *out_block = inode_table_start + idx / per_block;
    *out_offset = (idx % per_block) * INODE_SIZE;
}

static void read_inode(u32 inum, ufs_inode_t *out) {
    u32 blk, off;
    inode_location(inum, &blk, &off);
    read_block(blk, inode_blk_buf);
    memcpy(out, inode_blk_buf + off, sizeof(ufs_inode_t));
}

static void write_inode(u32 inum, const ufs_inode_t *in) {
    u32 blk, off;
    inode_location(inum, &blk, &off);
    read_block(blk, inode_blk_buf);
    memcpy(inode_blk_buf + off, in, sizeof(ufs_inode_t));
    write_block(blk, inode_blk_buf);
}

static u32 alloc_inode(void) {
    ufs_inode_t tmp;
    /* inode 1 всегда занят корневым каталогом, поэтому начинаем с 2 */
    for (u32 i = 2; i <= inode_count; i++) {
        read_inode(i, &tmp);
        if (tmp.mode == UFS_MODE_FREE) return i;
    }
    return 0;
}

/* Возвращает номер физического блока данных для логического блока
   номер logical_index внутри файла/каталога, представленного inode.
   Если такого блока ещё нет и alloc == TRUE -- выделяет его (и, если
   нужно, косвенный блок) и записывает указатель в *inode (вызывающий
   код обязан после этого сохранить inode через write_inode). Если
   alloc == FALSE и блока нет -- возвращает 0. */
static u32 inode_get_block(ufs_inode_t *inode, u32 logical_index, bool_t alloc) {
    if (logical_index < UFS_NUM_DIRECT) {
        if (inode->direct[logical_index] == 0) {
            if (!alloc) return 0;
            u32 nb = alloc_block();
            if (nb == 0) return 0;
            inode->direct[logical_index] = nb;
        }
        return inode->direct[logical_index];
    }

    u32 idx2 = logical_index - UFS_NUM_DIRECT;
    u32 ptrs_per_block = block_size / 4;
    if (idx2 >= ptrs_per_block) return 0; /* файл больше поддерживаемого максимума */

    if (inode->indirect == 0) {
        if (!alloc) return 0;
        u32 nb = alloc_block();
        if (nb == 0) return 0;
        inode->indirect = nb;
    }

    read_block(inode->indirect, indirect_buf);
    u32 *ptrs = (u32 *)indirect_buf;
    if (ptrs[idx2] == 0) {
        if (!alloc) return 0;
        u32 nb2 = alloc_block();
        if (nb2 == 0) return 0;
        ptrs[idx2] = nb2;
        write_block(inode->indirect, indirect_buf);
    }
    return ptrs[idx2];
}

/* Освобождает все блоки данных файла/каталога (прямые + косвенный со
   всем его содержимым) и обнуляет соответствующие поля inode. */
static void free_inode_data(ufs_inode_t *inode) {
    for (u32 i = 0; i < UFS_NUM_DIRECT; i++) {
        if (inode->direct[i]) {
            free_block(inode->direct[i]);
            inode->direct[i] = 0;
        }
    }
    if (inode->indirect) {
        read_block(inode->indirect, indirect_buf);
        u32 *ptrs = (u32 *)indirect_buf;
        u32 ptrs_per_block = block_size / 4;
        for (u32 i = 0; i < ptrs_per_block; i++) {
            if (ptrs[i]) free_block(ptrs[i]);
        }
        free_block(inode->indirect);
        inode->indirect = 0;
    }
    inode->size = 0;
}

/* ---------- Обход записей каталога ---------- */

typedef bool_t (*ufs_dirent_visitor_t)(ufs_dirent_t *e, u32 block, u32 index, void *ctx);

/* Просто перебирает все логические блоки каталога и все записи внутри
   них, вызывая visitor на каждую. Сам walk_directory не решает, где
   "конец" каталога -- это решает конкретный visitor (возвращая TRUE),
   ровно как в fs/fat32.c: там за это отвечают find_visitor/ls_visitor,
   а не walk_directory. */
static void walk_directory(u32 dir_inum, ufs_dirent_visitor_t visitor, void *ctx) {
    ufs_inode_t dir;
    read_inode(dir_inum, &dir);
    u32 entries_per_block = block_size / sizeof(ufs_dirent_t);
    u32 logical = 0;
    for (;;) {
        u32 blk = inode_get_block(&dir, logical, FALSE);
        if (blk == 0) return; /* больше выделенных блоков нет */
        read_block(blk, data_buf);
        ufs_dirent_t *entries = (ufs_dirent_t *)data_buf;
        for (u32 i = 0; i < entries_per_block; i++) {
            if (visitor(&entries[i], blk, i, ctx)) return;
        }
        logical++;
    }
}

typedef struct {
    const char *target;
    bool_t found;
    ufs_dirent_t entry;
    u32 blk;
    u32 idx;
} ufs_find_ctx_t;

static bool_t find_visitor(ufs_dirent_t *e, u32 blk, u32 idx, void *ctx_) {
    ufs_find_ctx_t *ctx = (ufs_find_ctx_t *)ctx_;
    if (e->inode == 0) return TRUE; /* конец каталога, не нашли */
    if (e->inode == UFS_DIRENT_DELETED) return FALSE;
    if (strcmp(e->name, ctx->target) == 0) {
        ctx->found = TRUE;
        ctx->entry = *e;
        ctx->blk = blk;
        ctx->idx = idx;
        return TRUE;
    }
    return FALSE;
}

static bool_t find_entry(u32 dir_inum, const char *name, ufs_find_ctx_t *out) {
    memset(out, 0, sizeof(*out));
    out->target = name;
    walk_directory(dir_inum, find_visitor, out);
    return out->found;
}

typedef struct {
    bool_t found;
    u32 blk;
    u32 idx;
} ufs_slot_ctx_t;

static bool_t slot_visitor(ufs_dirent_t *e, u32 blk, u32 idx, void *ctx_) {
    ufs_slot_ctx_t *ctx = (ufs_slot_ctx_t *)ctx_;
    if (e->inode == 0 || e->inode == UFS_DIRENT_DELETED) {
        ctx->found = TRUE;
        ctx->blk = blk;
        ctx->idx = idx;
        return TRUE;
    }
    return FALSE;
}

/* Находит свободный слот в каталоге (никогда не использованный либо
   освобождённый после rm), расширяя каталог новым блоком при
   необходимости -- аналог alloc_dir_slot() в fs/fat32.c. */
static bool_t alloc_dir_slot(u32 dir_inum, u32 *out_blk, u32 *out_idx) {
    ufs_slot_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    walk_directory(dir_inum, slot_visitor, &ctx);
    if (ctx.found) {
        *out_blk = ctx.blk;
        *out_idx = ctx.idx;
        return TRUE;
    }

    ufs_inode_t dir;
    read_inode(dir_inum, &dir);
    u32 logical = 0;
    while (inode_get_block(&dir, logical, FALSE) != 0) logical++;
    u32 newblk = inode_get_block(&dir, logical, TRUE);
    if (newblk == 0) return FALSE;
    write_inode(dir_inum, &dir); /* сохраняем новый указатель на блок (direct[] или indirect) */
    *out_blk = newblk;
    *out_idx = 0;
    return TRUE;
}

typedef struct { u32 count; } ufs_count_ctx_t;

static bool_t count_visitor(ufs_dirent_t *e, u32 blk, u32 idx, void *ctx_) {
    (void)blk; (void)idx;
    if (e->inode == 0) return TRUE;
    if (e->inode != UFS_DIRENT_DELETED) ((ufs_count_ctx_t *)ctx_)->count++;
    return FALSE;
}

typedef struct { u32 count; } ufs_ls_ctx_t;

static bool_t ls_visitor(ufs_dirent_t *e, u32 blk, u32 idx, void *ctx_) {
    (void)blk; (void)idx;
    if (e->inode == 0) return TRUE;
    if (e->inode == UFS_DIRENT_DELETED) return FALSE;

    ufs_inode_t entry_inode;
    read_inode(e->inode, &entry_inode);
    if (entry_inode.mode == UFS_MODE_DIR) {
        kprintf("  <DIR>  %s\n", e->name);
    } else {
        kprintf("  %u\t%s\n", entry_inode.size, e->name);
    }
    ((ufs_ls_ctx_t *)ctx_)->count++;
    return FALSE;
}

/* ---------- Монтирование ---------- */

/* Общее ядро монтирования -- читает суперблок с LBA 0 (относительно
   того, что уже настроено в io_read_fn/io_write_fn/partition_lba_offset
   вызывающим кодом) и валидирует/раскладывает его в статические поля.
   Не трогает io_read_fn/io_write_fn/io_ctx/cur_drive/partition_lba_offset
   -- это ответственность caller'а (ufs_mount()/ufs_mount_encrypted()
   ниже), mount_core() только читает то, что уже настроено. */
static bool_t mount_core(void) {
    if (!disk_read_sectors(0, 1, sb_sector_buf)) { mounted = FALSE; return FALSE; }

    ufs_superblock_t *sb = (ufs_superblock_t *)sb_sector_buf;
    if (sb->magic != UFS_MAGIC) {
        mounted = FALSE;
        return FALSE; /* не похоже на валидный UFS-раздел этого драйвера */
    }
    if (sb->block_size == 0 || sb->block_size > MAX_BLOCK_SIZE ||
        sb->sectors_per_block == 0 || sb->sectors_per_block > (MAX_BLOCK_SIZE / SECTOR_SIZE) ||
        sb->inode_count == 0 || sb->inode_table_start == 0 ||
        sb->data_start == 0 || sb->data_start >= sb->total_blocks || sb->root_inode == 0) {
        mounted = FALSE;
        return FALSE;
    }

    block_size = sb->block_size;
    sectors_per_block = sb->sectors_per_block;
    total_blocks = sb->total_blocks;
    inode_count = sb->inode_count;
    inode_table_start = sb->inode_table_start;
    inode_table_blocks = sb->inode_table_blocks;
    bitmap_start = sb->bitmap_start;
    bitmap_blocks = sb->bitmap_blocks;
    data_start = sb->data_start;
    root_inode = sb->root_inode;

    stack_depth = 1;
    dir_inode_stack[0] = root_inode;
    strcpy(dir_name_stack[0], "/");

    mounted = TRUE;
    return TRUE;
}

bool_t ufs_mount(u32 drive_index) {
    storage_dev_t *d = storage_get_drive(drive_index);
    if (!d) return FALSE;

    u32 saved_drive = cur_drive;
    cur_drive = drive_index;
    io_read_fn = default_io_read;   /* на случай, если до этого был смонтирован encrypted-бэкенд -- вернуться к обычному диску */
    io_write_fn = default_io_write;
    io_ctx = NULL;
    g_readonly = FALSE;             /* свежее монтирование -- по умолчанию RW, см. общий комментарий у g_readonly выше */

    u32 unused_partition_sectors;
    detect_partition_range(drive_index, d->sectors, &partition_lba_offset, &unused_partition_sectors);

    if (!mount_core()) { cur_drive = saved_drive; return FALSE; }
    cur_drive = drive_index;
    return TRUE;
}

/* Монтирует ТЕЛО зашифрованного пользовательского образа (Фаза 3) --
 * НЕ через detect_partition_range()/MBR (там его нет и быть не может --
 * тело слота начинается сразу с суперблока UFS, БЕЗ MBR, см. общий
 * комментарий в apps/create-user.c за тем, где именно у слота начало
 * тела) -- вместо этого io_read_fn/io_write_fn подменяются на обёртки
 * поверх lib/cryptoblk.c, а partition_lba_offset = 0 (смещение слота
 * на физическом диске уже "зашито" в io_ctx ниже, ЭТОТ файл про него
 * ничего не знает и знать не должен -- см. общий комментарий у
 * подключаемого бэкенда выше).
 *
 * master_key -- РАСШИФРОВАННЫЙ ключ, уже полученный вызывающим кодом
 * через userimg_verify_password() (include/userimg.h, Фаза 3) --
 * ЭТОТ файл паролей не проверяет и не хранит их вообще, только уже
 * готовый ключ. readonly -- см. ufs_set_readonly() ниже: для "просто
 * посмотреть" сценариев (например, Фаза 5 могла бы захотеть смонтировать
 * чужой -- точнее, ДРУГОЙ, но своим же паролем разблокированный --
 * образ только для чтения, хотя основной сценарий тут -- обычный RW
 * доступ владельца к собственному дому).
 *
 * ВАЖНО -- статический g_crypto_dev (единственный) означает, что этот
 * ЖЕ файл не может ОДНОВРЕМЕННО держать смонтированными и обычный
 * диск, и зашифрованный образ (как и раньше -- один активный UFS-
 * маунт за раз, см. общий комментарий в ATOMIC_PLAN.md, Фаза 4, за
 * тем, почему это сознательно не переросло в полноценный многоточечный
 * VFS в этом заходе). */
typedef struct {
    u32 drive_index;
    u32 slot_body_lba_start;
} ufs_crypto_io_ctx_t;

static ufs_crypto_io_ctx_t g_crypto_io_ctx;
static cryptoblk_dev_t g_crypto_dev;

static bool_t crypto_backing_read(void *ctx, u32 lba, u32 count, void *buf) {
    ufs_crypto_io_ctx_t *c = (ufs_crypto_io_ctx_t *)ctx;
    return storage_read_sectors(c->drive_index, c->slot_body_lba_start + lba, (u8)count, buf); /* count<=CRYPTOBLK_MAX_BATCH_SECTORS(32), см. lib/cryptoblk.c -- влезает в u8 с запасом */
}
static bool_t crypto_backing_write(void *ctx, u32 lba, u32 count, const void *buf) {
    ufs_crypto_io_ctx_t *c = (ufs_crypto_io_ctx_t *)ctx;
    return storage_write_sectors(c->drive_index, c->slot_body_lba_start + lba, (u8)count, buf);
}

static bool_t crypto_ufs_io_read(void *ctx, u32 lba, u32 count, void *buf) {
    return cryptoblk_read((cryptoblk_dev_t *)ctx, lba, count, buf);
}
static bool_t crypto_ufs_io_write(void *ctx, u32 lba, u32 count, const void *buf) {
    return cryptoblk_write((cryptoblk_dev_t *)ctx, lba, count, buf);
}

bool_t ufs_mount_encrypted(u32 drive_index, u32 slot_body_lba_start,
                            const u8 master_key[UFS_MASTER_KEY_LEN], bool_t readonly) {
    g_crypto_io_ctx.drive_index = drive_index;
    g_crypto_io_ctx.slot_body_lba_start = slot_body_lba_start;
    cryptoblk_init(&g_crypto_dev, master_key, crypto_backing_read, crypto_backing_write, &g_crypto_io_ctx);

    u32 saved_drive = cur_drive;
    cur_drive = drive_index;
    partition_lba_offset = 0; /* см. общий комментарий у функции выше -- тело слота начинается с локального LBA 0, без MBR */
    io_read_fn = crypto_ufs_io_read;
    io_write_fn = crypto_ufs_io_write;
    io_ctx = &g_crypto_dev;

    if (!mount_core()) {
        cur_drive = saved_drive;
        io_read_fn = default_io_read; /* не оставляем систему смонтированной на "наполовину" крипто-бэкенде после неудачи */
        io_write_fn = default_io_write;
        io_ctx = NULL;
        return FALSE;
    }
    cur_drive = drive_index;
    g_readonly = readonly;
    return TRUE;
}

/* Ограничивает запись на уже смонтированной ФС, НЕ трогая сам
 * маунт -- см. вызовы из ufs_mkdir()/ufs_rm()/ufs_write_file() ниже.
 * Отдельная функция (а не параметр ufs_mount()) -- потому что удобный
 * момент решить "можно ли писать" не всегда совпадает с моментом
 * монтирования (см. fs/vfs.c, fs_mount() -- он монтирует ОБЫЧНЫЙ диск
 * и сам не знает, должен ли результат быть read-only, это решает уже
 * вызывающий код повыше, например install-os для os-slot-a/b). Не
 * влияет на ufs_format() -- форматирование НОВОЙ файловой системы
 * осознанно вне этой проверки, это отдельная, доверенная операция
 * (создание с нуля, а не обычная работа с уже созданной ФС), которую
 * сегодня вызывают только install-os/create-user напрямую. */
void ufs_set_readonly(bool_t readonly) { g_readonly = readonly; }
bool_t ufs_is_readonly(void) { return g_readonly; }

bool_t ufs_is_mounted(void) { return mounted; }
u32 ufs_current_drive(void) { return cur_drive; }

/* ---------- Форматирование ---------- */

/* Общее ядро форматирования -- принимает уже известный total_sectors
   (посчитанный вызывающим по-разному для обычного диска и для тела
   зашифрованного слота, см. ufs_format()/ufs_format_encrypted() ниже)
   и делает всю остальную работу: считает геометрию, пишет суперблок,
   обнуляет bitmap/inode-таблицу, создаёт корневой каталог. Не трогает
   io_read_fn/io_write_fn/io_ctx/cur_drive/partition_lba_offset -- это
   ответственность caller'а, ровно как и у mount_core() выше. */
static bool_t format_core(u32 total_sectors) {
    if (total_sectors < 4096) return FALSE; /* слишком маленький раздел/диск/тело слота */

    u32 tb = total_sectors / SECTORS_PER_BLOCK;
    if (tb < 32) return FALSE;

    /* Одна inode примерно на каждые 4 блока данных, с разумными границами */
    u32 icount = tb / 4;
    if (icount < 64) icount = 64;
    if (icount > 4096) icount = 4096;

    u32 inodes_per_block = BLOCK_SIZE / INODE_SIZE;
    u32 itb = (icount + inodes_per_block - 1) / inodes_per_block;

    /* Приблизительный расчёт размера битовой карты (с небольшим запасом,
       как и оценка размера FAT в fat32_format) */
    u32 approx_data_blocks = tb - 1 - itb;
    u32 bits_per_block = BLOCK_SIZE * 8;
    u32 bmb = (approx_data_blocks + bits_per_block - 1) / bits_per_block;

    u32 dstart = 1 + bmb + itb;
    if (dstart >= tb) return FALSE;

    memset(sb_sector_buf, 0, SECTOR_SIZE);
    ufs_superblock_t *sb = (ufs_superblock_t *)sb_sector_buf;
    sb->magic = UFS_MAGIC;
    sb->block_size = BLOCK_SIZE;
    sb->sectors_per_block = SECTORS_PER_BLOCK;
    sb->total_blocks = tb;
    sb->inode_count = icount;
    sb->inode_table_start = 1 + bmb;
    sb->inode_table_blocks = itb;
    sb->bitmap_start = 1;
    sb->bitmap_blocks = bmb;
    sb->data_start = dstart;
    sb->root_inode = 1;
    memcpy(sb->volume_label, "MEETLOOKUFS", 11);
    disk_write_sectors(0, 1, sb_sector_buf);

    block_size = BLOCK_SIZE;
    sectors_per_block = SECTORS_PER_BLOCK;
    total_blocks = tb;
    inode_count = icount;
    inode_table_start = sb->inode_table_start;
    inode_table_blocks = itb;
    bitmap_start = 1;
    bitmap_blocks = bmb;
    data_start = dstart;
    root_inode = 1;

    /* Обнуляем битовую карту и таблицу inode'ов */
    memset(zero_buf, 0, block_size);
    for (u32 i = 0; i < bmb; i++) write_block(bitmap_start + i, zero_buf);
    for (u32 i = 0; i < itb; i++) write_block(inode_table_start + i, zero_buf);

    /* Создаём корневой каталог: inode #1, пустой, без блоков данных */
    ufs_inode_t root;
    memset(&root, 0, sizeof(root));
    root.mode = UFS_MODE_DIR;
    root.size = 0;
    write_inode(1, &root);

    mounted = TRUE;
    stack_depth = 1;
    dir_inode_stack[0] = 1;
    strcpy(dir_name_stack[0], "/");

    return TRUE;
}

bool_t ufs_format(u32 drive_index) {
    storage_dev_t *d = storage_get_drive(drive_index);
    if (!d) return FALSE;
    u32 saved_drive = cur_drive;
    cur_drive = drive_index;
    io_read_fn = default_io_read;  /* см. тот же сброс в ufs_mount() выше за объяснением */
    io_write_fn = default_io_write;
    io_ctx = NULL;
    g_readonly = FALSE;

    u32 total_sectors;
    detect_partition_range(drive_index, d->sectors, &partition_lba_offset, &total_sectors);

    if (!format_core(total_sectors)) { cur_drive = saved_drive; return FALSE; }
    cur_drive = drive_index;
    return TRUE;
}

/* Форматирует НОВЫЙ UFS поверх ТЕЛА зашифрованного пользовательского
 * образа (include/userimg.h, Фаза 3) -- та же идея, что
 * ufs_mount_encrypted() выше (io_read_fn/write_fn -> lib/cryptoblk.c,
 * partition_lba_offset=0, без MBR), но для СОЗДАНИЯ файловой системы
 * с нуля, а не монтирования уже существующей. Вызывается ОДИН раз, в
 * момент создания пользователя (apps/create-user.c, через
 * SYS_FS_FORMAT_HOME_ENCRYPTED) -- ДО того, как этим образом вообще
 * можно будет воспользоваться (сам вход в систему, чтение/запись
 * файлов -- уже отдельно, через ufs_mount_encrypted(), Фаза 5).
 *
 * slot_body_sectors -- сколько секторов доступно ПОД САМ UFS (не
 * считая заголовка userimg.h -- caller передаёт границы тела, а не
 * границы всего слота целиком, см. apps/create-user.c за тем, как это
 * считается). РАЗРУШАЕТ всё, что было в этом диапазоне -- та же
 * природа, что и у обычного ufs_format(). */
bool_t ufs_format_encrypted(u32 drive_index, u32 slot_body_lba_start, u32 slot_body_sectors,
                             const u8 master_key[UFS_MASTER_KEY_LEN]) {
    g_crypto_io_ctx.drive_index = drive_index;
    g_crypto_io_ctx.slot_body_lba_start = slot_body_lba_start;
    cryptoblk_init(&g_crypto_dev, master_key, crypto_backing_read, crypto_backing_write, &g_crypto_io_ctx);

    u32 saved_drive = cur_drive;
    cur_drive = drive_index;
    partition_lba_offset = 0;
    io_read_fn = crypto_ufs_io_read;
    io_write_fn = crypto_ufs_io_write;
    io_ctx = &g_crypto_dev;
    g_readonly = FALSE;

    if (!format_core(slot_body_sectors)) {
        cur_drive = saved_drive;
        io_read_fn = default_io_read; /* не оставляем систему смонтированной на "наполовину" крипто-бэкенде после неудачи -- см. тот же приём в ufs_mount_encrypted() выше */
        io_write_fn = default_io_write;
        io_ctx = NULL;
        return FALSE;
    }
    cur_drive = drive_index;
    return TRUE;
}

/* ---------- Команды файловой системы ---------- */

static u32 current_dir(void) { return dir_inode_stack[stack_depth - 1]; }

void ufs_ls(void) {
    if (!mounted) { kprintf("No disk mounted. Use select.\n"); return; }
    ufs_ls_ctx_t ctx = {0};
    walk_directory(current_dir(), ls_visitor, &ctx);
    if (ctx.count == 0) kprintf("  (empty)\n");
}

/* См. fs_list(), include/vfs.h. owner_uid/mode -- UFS не знает понятия
   владельца узла, заполняются нулями (см. общий комментарий у
   fs_dirent_t, include/ramfs.h). */
typedef struct { fs_dirent_t *out; u32 max_entries; u32 count; } ufs_ls_collect_ctx_t;

static bool_t ls_collect_visitor(ufs_dirent_t *e, u32 blk, u32 idx, void *ctx_) {
    (void)blk; (void)idx;
    if (e->inode == 0) return TRUE;
    if (e->inode == UFS_DIRENT_DELETED) return FALSE;

    ufs_ls_collect_ctx_t *ctx = (ufs_ls_collect_ctx_t *)ctx_;
    if (ctx->count < ctx->max_entries) {
        ufs_inode_t entry_inode;
        read_inode(e->inode, &entry_inode);

        fs_dirent_t *d = &ctx->out[ctx->count];
        strncpy(d->name, e->name, FS_DIRENT_NAME_MAX - 1);
        d->name[FS_DIRENT_NAME_MAX - 1] = '\0';
        d->size = entry_inode.size;
        d->owner_uid = 0;
        d->mode = 0;
        d->is_dir = (entry_inode.mode == UFS_MODE_DIR);
        d->is_app = FALSE;
    }
    ctx->count++;
    return FALSE;
}

bool_t ufs_list(fs_dirent_t *out, u32 max_entries, u32 *out_count) {
    if (!mounted) return FALSE;
    ufs_ls_collect_ctx_t ctx = { out, max_entries, 0 };
    walk_directory(current_dir(), ls_collect_visitor, &ctx);
    if (out_count) *out_count = ctx.count;
    return TRUE;
}

bool_t ufs_cd(const char *name) {
    if (!mounted) return FALSE;
    if (strcmp(name, "/") == 0) { stack_depth = 1; return TRUE; }
    if (strcmp(name, "..") == 0) {
        if (stack_depth > 1) stack_depth--;
        return TRUE;
    }
    if (strcmp(name, ".") == 0) return TRUE;

    ufs_find_ctx_t fc;
    if (!find_entry(current_dir(), name, &fc)) return FALSE;

    ufs_inode_t target;
    read_inode(fc.entry.inode, &target);
    if (target.mode != UFS_MODE_DIR) return FALSE;

    if (stack_depth >= MAX_DEPTH) return FALSE;
    dir_inode_stack[stack_depth] = fc.entry.inode;
    strncpy(dir_name_stack[stack_depth], name, UFS_NAME_MAX);
    dir_name_stack[stack_depth][UFS_NAME_MAX] = '\0';
    stack_depth++;
    return TRUE;
}

bool_t ufs_mkdir(const char *name) {
    if (!mounted) return FALSE;
    if (g_readonly) return FALSE; /* см. ufs_set_readonly(), ATOMIC_PLAN.md Фаза 4 */
    if (strlen(name) > UFS_NAME_MAX) return FALSE;

    ufs_find_ctx_t fc;
    if (find_entry(current_dir(), name, &fc)) return FALSE; /* уже существует */

    u32 newinum = alloc_inode();
    if (newinum == 0) return FALSE;

    ufs_inode_t ni;
    memset(&ni, 0, sizeof(ni));
    ni.mode = UFS_MODE_DIR;
    ni.size = 0;
    write_inode(newinum, &ni);

    u32 blk, idx;
    if (!alloc_dir_slot(current_dir(), &blk, &idx)) return FALSE;

    read_block(blk, data_buf);
    ufs_dirent_t *entries = (ufs_dirent_t *)data_buf;
    entries[idx].inode = newinum;
    strncpy(entries[idx].name, name, UFS_NAME_MAX);
    entries[idx].name[UFS_NAME_MAX] = '\0';
    write_block(blk, data_buf);

    return TRUE;
}

bool_t ufs_rm(const char *name) {
    if (!mounted) return FALSE;
    if (g_readonly) return FALSE; /* см. ufs_set_readonly(), ATOMIC_PLAN.md Фаза 4 */
    ufs_find_ctx_t fc;
    if (!find_entry(current_dir(), name, &fc)) return FALSE;

    ufs_inode_t target;
    read_inode(fc.entry.inode, &target);

    if (target.mode == UFS_MODE_DIR) {
        ufs_count_ctx_t cctx = {0};
        walk_directory(fc.entry.inode, count_visitor, &cctx);
        if (cctx.count > 0) return FALSE; /* каталог не пуст */
    }

    free_inode_data(&target);
    target.mode = UFS_MODE_FREE;
    write_inode(fc.entry.inode, &target);

    read_block(fc.blk, data_buf);
    ((ufs_dirent_t *)data_buf)[fc.idx].inode = UFS_DIRENT_DELETED;
    write_block(fc.blk, data_buf);
    return TRUE;
}

bool_t ufs_write_file(const char *name, const char *data, u32 len) {
    if (!mounted) return FALSE;
    if (g_readonly) return FALSE; /* см. ufs_set_readonly(), ATOMIC_PLAN.md Фаза 4 */
    if (strlen(name) > UFS_NAME_MAX) return FALSE;

    ufs_find_ctx_t fc;
    bool_t exists = find_entry(current_dir(), name, &fc);

    ufs_inode_t inode;
    u32 inum;
    if (exists) {
        inum = fc.entry.inode;
        read_inode(inum, &inode);
        if (inode.mode == UFS_MODE_DIR) return FALSE;
        free_inode_data(&inode); /* освобождаем старое содержимое перед перезаписью */
    } else {
        inum = alloc_inode();
        if (inum == 0) return FALSE;
        memset(&inode, 0, sizeof(inode));
        inode.mode = UFS_MODE_FILE;
    }

    u32 remaining = len;
    const u8 *src = (const u8 *)data;
    u32 logical = 0;

    while (remaining > 0) {
        u32 blk = inode_get_block(&inode, logical, TRUE);
        if (blk == 0) { write_inode(inum, &inode); return FALSE; } /* диск переполнен */

        u32 chunk = (remaining > block_size) ? block_size : remaining;
        memset(data_buf, 0, block_size);
        memcpy(data_buf, src, chunk);
        write_block(blk, data_buf);

        src += chunk;
        remaining -= chunk;
        logical++;
    }

    inode.size = len;
    write_inode(inum, &inode);

    if (!exists) {
        u32 blk, idx;
        if (!alloc_dir_slot(current_dir(), &blk, &idx)) return FALSE;
        read_block(blk, data_buf);
        ufs_dirent_t *entries = (ufs_dirent_t *)data_buf;
        entries[idx].inode = inum;
        strncpy(entries[idx].name, name, UFS_NAME_MAX);
        entries[idx].name[UFS_NAME_MAX] = '\0';
        write_block(blk, data_buf);
    }

    return TRUE;
}

bool_t ufs_cat(const char *name) {
    if (!mounted) return FALSE;
    ufs_find_ctx_t fc;
    if (!find_entry(current_dir(), name, &fc)) return FALSE;

    ufs_inode_t inode;
    read_inode(fc.entry.inode, &inode);
    if (inode.mode == UFS_MODE_DIR) return FALSE;

    u32 size = inode.size;
    u32 read_total = 0;
    u32 logical = 0;

    while (read_total < size) {
        u32 blk = inode_get_block(&inode, logical, FALSE);
        if (blk == 0) break;
        read_block(blk, data_buf);
        u32 chunk = size - read_total;
        if (chunk > block_size) chunk = block_size;
        for (u32 i = 0; i < chunk; i++) kprintf("%c", data_buf[i]);
        read_total += chunk;
        logical++;
    }
    kprintf("\n");
    return TRUE;
}

bool_t ufs_read_file(const char *name, void *buf, u32 bufsize, u32 *out_size) {
    if (!mounted) return FALSE;
    ufs_find_ctx_t fc;
    if (!find_entry(current_dir(), name, &fc)) return FALSE;

    ufs_inode_t inode;
    read_inode(fc.entry.inode, &inode);
    if (inode.mode == UFS_MODE_DIR) return FALSE;

    u32 size = inode.size;
    if (size > bufsize) size = bufsize;
    u32 read_total = 0;
    u32 logical = 0;
    u8 *dst = (u8 *)buf;

    while (read_total < size) {
        u32 blk = inode_get_block(&inode, logical, FALSE);
        if (blk == 0) break;
        read_block(blk, data_buf);
        u32 chunk = size - read_total;
        if (chunk > block_size) chunk = block_size;
        memcpy(dst + read_total, data_buf, chunk);
        read_total += chunk;
        logical++;
    }
    if (out_size) *out_size = read_total;
    return TRUE;
}

void ufs_pwd(char *buf, u32 bufsize) {
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
