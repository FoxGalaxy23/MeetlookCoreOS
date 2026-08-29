/* gpt.c -- реализация gpt.h. Чистая логика над буферами в памяти, без
 * единой операции ввода-вывода -- см. общий комментарий в gpt.h. */

#include "gpt.h"
#include "crc32.h"
#include "string.h"

_Static_assert(sizeof(gpt_header_t) == 92,
    "gpt_header_t должна быть ровно 92 байта -- формат GPT это жёстко фиксирует");
_Static_assert(sizeof(gpt_partition_entry_t) == GPT_PARTITION_ENTRY_SIZE,
    "gpt_partition_entry_t должна быть ровно GPT_PARTITION_ENTRY_SIZE (128) байт");

gpt_guid_t gpt_guid_from_fields(u32 data1, u16 data2, u16 data3, const u8 data4[8]) {
    gpt_guid_t g;
    /* data1/data2/data3 -- little-endian; data4 -- как есть (big-endian
     * массив байт по спецификации) -- см. общий комментарий в gpt.h про
     * mixed-endian представление GUID. */
    g.bytes[0] = (u8)(data1 & 0xFF);
    g.bytes[1] = (u8)((data1 >> 8) & 0xFF);
    g.bytes[2] = (u8)((data1 >> 16) & 0xFF);
    g.bytes[3] = (u8)((data1 >> 24) & 0xFF);
    g.bytes[4] = (u8)(data2 & 0xFF);
    g.bytes[5] = (u8)((data2 >> 8) & 0xFF);
    g.bytes[6] = (u8)(data3 & 0xFF);
    g.bytes[7] = (u8)((data3 >> 8) & 0xFF);
    for (u32 i = 0; i < 8; i++) g.bytes[8 + i] = data4[i];
    return g;
}

const gpt_guid_t GPT_GUID_UNUSED = { { 0 } };

/* C12A7328-F81F-11D2-BA4B-00A0C93EC93B -- стандартный, задокументированный
 * в UEFI-спецификации GUID EFI System Partition, см. общий комментарий
 * в gpt.h. */
const gpt_guid_t GPT_GUID_ESP = {
    { 0x28, 0x73, 0x2A, 0xC1, 0x1F, 0xF8, 0xD2, 0x11,
      0xBA, 0x4B, 0x00, 0xA0, 0xC9, 0x3E, 0xC9, 0x3B }
};

/* F5CCA6C1-17EC-4F9F-96B6-A4B2921784FE -- собственный, случайно
 * сгенерированный (uuid4) GUID проекта, зафиксированный раз и навсегда
 * -- см. общий комментарий в gpt.h. */
const gpt_guid_t GPT_GUID_MEETLOOK_UFS = {
    { 0xC1, 0xA6, 0xCC, 0xF5, 0xEC, 0x17, 0x9F, 0x4F,
      0x96, 0xB6, 0xA4, 0xB2, 0x92, 0x17, 0x84, 0xFE }
};

/* ---- ATOMIC_PLAN.md, Фаза 2 -- см. общий комментарий в gpt.h ---- */

/* 3426A86F-A1A8-49CF-A488-E00596D07C0B -- собственный uuid4, boot-meta. */
const gpt_guid_t GPT_GUID_MEETLOOK_BOOTMETA = {
    { 0x6F, 0xA8, 0x26, 0x34, 0xA8, 0xA1, 0xCF, 0x49,
      0xA4, 0x88, 0xE0, 0x05, 0x96, 0xD0, 0x7C, 0x0B }
};

/* 18E40BE3-88BE-4A0F-AFAE-9BA471354A80 -- собственный uuid4, os-slot (A/B). */
const gpt_guid_t GPT_GUID_MEETLOOK_OS_SLOT = {
    { 0xE3, 0x0B, 0xE4, 0x18, 0xBE, 0x88, 0x0F, 0x4A,
      0xAF, 0xAE, 0x9B, 0xA4, 0x71, 0x35, 0x4A, 0x80 }
};

/* 22DDBCB3-CB7C-46C0-A746-640C6A53C803 -- собственный uuid4, user-images. */
const gpt_guid_t GPT_GUID_MEETLOOK_USER_IMAGES = {
    { 0xB3, 0xBC, 0xDD, 0x22, 0x7C, 0xCB, 0xC0, 0x46,
      0xA7, 0x46, 0x64, 0x0C, 0x6A, 0x53, 0xC8, 0x03 }
};

/* 9B2E7A44-5D1F-4E22-8C3A-1F6B0D9E4471 -- собственный uuid4, bios-boot
 * (Фаза 6) -- см. общий комментарий в gpt.h. */
const gpt_guid_t GPT_GUID_MEETLOOK_BIOS_BOOT = {
    { 0x44, 0x7A, 0x2E, 0x9B, 0x1F, 0x5D, 0x22, 0x4E,
      0x8C, 0x3A, 0x1F, 0x6B, 0x0D, 0x9E, 0x44, 0x71 }
};

static bool_t guid_equal(const gpt_guid_t *a, const gpt_guid_t *b) {
    return memcmp(a->bytes, b->bytes, 16) == 0;
}

/* Заполняет unique_partition_guid детерминированно (не криптографически
 * случайно -- см. общий комментарий у gpt_partition_entry_set() в
 * gpt.h): достаточно, чтобы РАЗЛИЧАТЬ разделы одного диска, полноценная
 * непредсказуемость здесь не требуется и не нужна. */
static gpt_guid_t derive_unique_guid(const gpt_guid_t *type_guid, u64 starting_lba, u64 ending_lba) {
    u32 seed_a = crc32(0, type_guid->bytes, 16);
    seed_a = crc32(seed_a, &starting_lba, sizeof(starting_lba));
    u32 seed_b = crc32(0xFFFFFFFFu, &ending_lba, sizeof(ending_lba));
    seed_b = crc32(seed_b, type_guid->bytes, 16);

    u8 data4[8];
    data4[0] = (u8)(seed_b & 0xFF);
    data4[1] = (u8)((seed_b >> 8) & 0xFF);
    data4[2] = (u8)((seed_b >> 16) & 0xFF);
    data4[3] = (u8)((seed_b >> 24) & 0xFF);
    data4[4] = (u8)(starting_lba & 0xFF);
    data4[5] = (u8)(ending_lba & 0xFF);
    data4[6] = (u8)((starting_lba >> 8) & 0xFF);
    data4[7] = (u8)((ending_lba >> 8) & 0xFF);

    return gpt_guid_from_fields(seed_a, (u16)(seed_b & 0xFFFFu),
                                 (u16)((seed_b >> 16) & 0xFFFFu), data4);
}

void gpt_partition_entry_set(gpt_partition_entry_t *entry, gpt_guid_t type_guid,
                              u64 starting_lba, u64 ending_lba, const char *name) {
    memset(entry, 0, sizeof(*entry));
    entry->partition_type_guid = type_guid;
    entry->unique_partition_guid = derive_unique_guid(&type_guid, starting_lba, ending_lba);
    entry->starting_lba = starting_lba;
    entry->ending_lba = ending_lba;
    entry->attributes = 0;

    if (name) {
        u32 i = 0;
        for (; name[i] != '\0' && i < 35; i++) entry->name[i] = (u16)(u8)name[i];
        entry->name[i] = 0;
    }
}

bool_t gpt_partition_is_used(const gpt_partition_entry_t *entry) {
    return !guid_equal(&entry->partition_type_guid, &GPT_GUID_UNUSED);
}

/* Сериализует один заголовок (92 байта полезной нагрузки) в 512-байтный
 * секторный буфер, дополняя нулями, с уже посчитанными CRC32 (header_crc32
 * считается по буферу С ОБНУЛЁННЫМ этим же полем -- стандартное
 * "self-referential" правило GPT, см. спецификацию). */
static void serialize_header(const gpt_header_t *hdr, u8 out[GPT_SECTOR_SIZE]) {
    memset(out, 0, GPT_SECTOR_SIZE);
    memcpy(out, hdr, sizeof(*hdr));
}

void gpt_build(u64 disk_sectors, gpt_guid_t disk_guid,
               const gpt_partition_entry_t partitions[GPT_MAX_PARTITIONS],
               u8 out_primary_header[GPT_SECTOR_SIZE],
               u8 out_primary_table[GPT_PARTITION_TABLE_SECTORS * GPT_SECTOR_SIZE],
               u8 out_backup_table[GPT_PARTITION_TABLE_SECTORS * GPT_SECTOR_SIZE],
               u8 out_backup_header[GPT_SECTOR_SIZE]) {
    const u32 table_bytes = GPT_PARTITION_ENTRIES_PER_TABLE * GPT_PARTITION_ENTRY_SIZE;

    /* Таблица разделов -- байт в байт одинаковая на обеих (первичной и
     * резервной) копиях, отличается только МЕСТО на диске. */
    memcpy(out_primary_table, partitions, table_bytes);
    memcpy(out_backup_table, partitions, table_bytes);
    u32 table_crc = crc32(0, out_primary_table, table_bytes);

    u64 primary_table_lba = 2u;
    u64 backup_table_lba = disk_sectors - 1u - GPT_PARTITION_TABLE_SECTORS;
    u64 first_usable_lba = primary_table_lba + GPT_PARTITION_TABLE_SECTORS; /* 34 */
    u64 last_usable_lba = backup_table_lba - 1u;                            /* disk_sectors - 34 */

    gpt_header_t primary;
    memset(&primary, 0, sizeof(primary));
    memcpy(primary.signature, GPT_HEADER_SIGNATURE, 8);
    primary.revision = GPT_HEADER_REVISION;
    primary.header_size = sizeof(gpt_header_t);
    primary.reserved = 0;
    primary.my_lba = 1u;
    primary.alternate_lba = disk_sectors - 1u;
    primary.first_usable_lba = first_usable_lba;
    primary.last_usable_lba = last_usable_lba;
    primary.disk_guid = disk_guid;
    primary.partition_entry_lba = primary_table_lba;
    primary.num_partition_entries = GPT_PARTITION_ENTRIES_PER_TABLE;
    primary.partition_entry_size = GPT_PARTITION_ENTRY_SIZE;
    primary.partition_entry_array_crc32 = table_crc;
    primary.header_crc32 = 0;
    primary.header_crc32 = crc32(0, &primary, sizeof(primary));
    serialize_header(&primary, out_primary_header);

    gpt_header_t backup = primary;
    backup.my_lba = primary.alternate_lba;
    backup.alternate_lba = primary.my_lba;
    backup.partition_entry_lba = backup_table_lba;
    backup.header_crc32 = 0;
    backup.header_crc32 = crc32(0, &backup, sizeof(backup));
    serialize_header(&backup, out_backup_header);
}

bool_t gpt_parse(const u8 header_sector[GPT_SECTOR_SIZE],
                  const u8 table[GPT_PARTITION_TABLE_SECTORS * GPT_SECTOR_SIZE],
                  gpt_partition_entry_t out_partitions[GPT_MAX_PARTITIONS]) {
    gpt_header_t hdr;
    memcpy(&hdr, header_sector, sizeof(hdr));

    if (memcmp(hdr.signature, GPT_HEADER_SIGNATURE, 8) != 0) return FALSE;
    if (hdr.revision != GPT_HEADER_REVISION) return FALSE;
    if (hdr.header_size != sizeof(gpt_header_t)) return FALSE;

    u32 stored_crc = hdr.header_crc32;
    gpt_header_t check = hdr;
    check.header_crc32 = 0;
    if (crc32(0, &check, sizeof(check)) != stored_crc) return FALSE;

    if (hdr.num_partition_entries == 0 ||
        hdr.num_partition_entries > GPT_MAX_PARTITIONS ||
        hdr.partition_entry_size != GPT_PARTITION_ENTRY_SIZE) return FALSE;

    u32 table_bytes = hdr.num_partition_entries * hdr.partition_entry_size;
    if (crc32(0, table, table_bytes) != hdr.partition_entry_array_crc32) return FALSE;

    memset(out_partitions, 0, GPT_MAX_PARTITIONS * sizeof(gpt_partition_entry_t));
    memcpy(out_partitions, table, table_bytes);
    return TRUE;
}
