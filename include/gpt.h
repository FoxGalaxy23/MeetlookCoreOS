#ifndef GPT_H
#define GPT_H

#include "types.h"

/* gpt.h -- сборка/разбор GUID Partition Table (GPT) в памяти. См.
 * UEFI_PLAN.md, Фаза A.
 *
 * ТОТ ЖЕ принцип, что и у lib/mbr.c (см. общий комментарий там же):
 * НИКАКОГО I/O здесь нет, вся логика работает над буферами в памяти
 * вызывающего. Кто и когда пишет результат на диск (через
 * sys_disk_write_raw(), include/usyscall.h) -- решает вызывающий код
 * (apps/install-os.c, Фаза D плана), не эта библиотека.
 *
 * ФОРМАТ (см. спецификацию UEFI, глава "GUID Partition Table (GPT) Disk
 * Layout" -- это открытый, документированный формат, в отличие от
 * boot_info_t это НЕ наша собственная придумка, и байтовая раскладка
 * структур ниже обязана совпадать с тем, что ожидает НАСТОЯЩАЯ
 * прошивка):
 *
 *   LBA 0             -- protective MBR (весь диск как один раздел
 *                         типа 0xEE -- см. mbr_build()/mbr_partition_set(),
 *                         lib/mbr.c, уже готово с INSTALLER_PLAN.md Фазы 1,
 *                         используется для GPT без изменений)
 *   LBA 1             -- первичный GPT-заголовок (gpt_header_t)
 *   LBA 2..33         -- первичная таблица разделов (128 записей по
 *                         128 байт = 16384 байт = 32 сектора)
 *   ...
 *   LBA N-33..N-2      -- резервная (backup) таблица разделов -- зеркало
 *                         первичной, у КОНЦА диска
 *   LBA N-1            -- резервный GPT-заголовок -- зеркало первичного,
 *                         с переставленными местами my_lba/alternate_lba
 *
 * CRC32 (include/crc32.h) -- НАСТОЯЩИЕ прошивки его действительно
 * проверяют (в отличие от MBR, где контрольных сумм нет вовсе) -- битый
 * CRC значит "диск считается неразмеченным". mbr.c этого не требовал. */

#define GPT_SECTOR_SIZE          512u
#define GPT_HEADER_SIGNATURE     "EFI PART" /* ровно 8 байт, без завершающего '\0' */
#define GPT_HEADER_REVISION      0x00010000u /* версия 1.0 */
#define GPT_PARTITION_ENTRY_SIZE 128u
#define GPT_PARTITION_ENTRIES_PER_TABLE 128u /* стандартное количество -- см. gpt_table_sectors() */
#define GPT_PARTITION_TABLE_SECTORS \
    ((GPT_PARTITION_ENTRIES_PER_TABLE * GPT_PARTITION_ENTRY_SIZE) / GPT_SECTOR_SIZE) /* 32 */
#define GPT_MAX_PARTITIONS       GPT_PARTITION_ENTRIES_PER_TABLE

/* 16-байтный GUID -- ХРАНИТСЯ в raw-байтах ТОЧНО в том порядке, в каком
 * его нужно записать на диск (mixed-endian по UEFI-спецификации: первые
 * 3 поля little-endian, последние 2 -- big-endian массивы байт) -- эта
 * структура НЕ содержит отдельных полей data1/data2/.../data4, чтобы
 * не провоцировать перепутать порядок байт при заполнении; используйте
 * gpt_guid_from_fields() ниже, а не ручную инициализацию массива. */
typedef struct {
    u8 bytes[16];
} gpt_guid_t;

/* Собирает gpt_guid_t из полей в том виде, в каком GUID обычно
 * записывают текстом ("AABBCCDD-EEFF-GGHH-IIJJ-KKLLMMNNOOPP") --
 * функция сама расставляет байты в нужном (mixed-endian) порядке, так
 * что вызывающему коду не нужно думать про endianness вообще. */
gpt_guid_t gpt_guid_from_fields(u32 data1, u16 data2, u16 data3, const u8 data4[8]);

/* Ноль-GUID -- "раздел не используется" (аналог MBR_PART_TYPE_EMPTY). */
extern const gpt_guid_t GPT_GUID_UNUSED;

/* EFI System Partition -- стандартный, задокументированный в UEFI-
 * спецификации GUID (C12A7328-F81F-11D2-BA4B-00A0C93EC93B), НЕ наша
 * придумка (в отличие от MBR_PART_TYPE_MEETLOOK_UFS) -- реальные
 * прошивки ищут раздел именно с этим типом, чтобы найти
 * \EFI\BOOT\BOOTX64.EFI (см. UEFI_PLAN.md, Фаза D). */
extern const gpt_guid_t GPT_GUID_ESP;

/* MeetlookCoreOS UFS -- раздел с данными в UEFI/GPT-режиме, тот же
 * принцип, что и MBR_PART_TYPE_MEETLOOK_UFS (include/mbr.h): случайно
 * сгенерированный (не заимствованный у другой ОС) GUID, зафиксированный
 * здесь как константа проекта -- см. tools/gen_guid.py при желании
 * перегенерировать (не нужно -- GUID уже зафиксирован и не должен
 * меняться между сборками). */
extern const gpt_guid_t GPT_GUID_MEETLOOK_UFS;

/* ---- ATOMIC_PLAN.md, Фаза 2: типы разделов атомарной схемы ---- */

/* boot-meta -- служебный раздел (1 сектор + запас), см. bootmeta.h.
 * ОДИН тип на весь раздел, читается НАПРЯМУЮ по фиксированному LBA
 * (не через VFS, до какой-либо файловой системы) -- см. общий
 * комментарий в ATOMIC_PLAN.md, Фаза 0. */
extern const gpt_guid_t GPT_GUID_MEETLOOK_BOOTMETA;

/* os-slot -- read-only слот с базовой ОС (kernel.flat + rootfs).
 * ОДИН И ТОТ ЖЕ type_guid у слота A и слота B -- различаются не типом,
 * а позицией/именем раздела (см. gpt_partition_entry_t.name) и записью
 * active_slot в boot-meta, а не GUID'ом -- ровно как в ATOMIC_PLAN.md,
 * Фаза 0, задумано. */
extern const gpt_guid_t GPT_GUID_MEETLOOK_OS_SLOT;

/* user-images -- один большой раздел-пул, внутри которого install-os/
 * create-user (Фаза 3) нарезают фиксированные слоты под зашифрованные
 * per-user образы. Раздел один на весь пул, а не по одному на
 * пользователя -- число пользователей не известно на этапе разметки
 * диска. */
extern const gpt_guid_t GPT_GUID_MEETLOOK_USER_IMAGES;

/* bios-boot -- Фаза 6: место под boot-код для GPT/Atomic-дисков, а НЕ
 * файловая система (никогда не монтируется, никем, кроме stage1 в
 * protective MBR, не читается). Тот же приём, что "BIOS Boot Partition"
 * у GRUB на реальных GPT+legacy-BIOS дисках -- см. общий комментарий в
 * ATOMIC_PLAN.md, Фаза 6, за тем, зачем он вообще нужен: protective MBR
 * (LBA 0) отдаёт под boot_code всего 446 байт, а GPT-заголовок и
 * таблица разделов (LBA 1..33) занимают ровно то место, где раньше (в
 * NOGRUB-схеме без GPT) лежал stage2 -- на Atomic-дисках его больше
 * некуда положить, кроме отдельного, специально нарезанного раздела
 * сразу после первичной GPT-таблицы (LBA 34..96, ATOMIC_BIOSBOOT_SECTORS
 * в apps/install-os.c). */
extern const gpt_guid_t GPT_GUID_MEETLOOK_BIOS_BOOT;

typedef struct __attribute__((packed)) {
    u8  signature[8];      /* GPT_HEADER_SIGNATURE, без '\0' */
    u32 revision;          /* GPT_HEADER_REVISION */
    u32 header_size;       /* 92 -- размер ДО паддинга до конца сектора */
    u32 header_crc32;      /* CRC32 первых header_size байт ЭТОЙ структуры,
                               посчитанный с этим же полем, временно
                               обнулённым -- см. gpt_build()/gpt_verify() */
    u32 reserved;          /* всегда 0 */
    u64 my_lba;             /* LBA этого заголовка (1 для первичного) */
    u64 alternate_lba;      /* LBA второго (резервного) заголовка */
    u64 first_usable_lba;   /* первый LBA, доступный под разделы */
    u64 last_usable_lba;    /* последний LBA, доступный под разделы */
    gpt_guid_t disk_guid;
    u64 partition_entry_lba;      /* LBA начала таблицы разделов ЭТОЙ копии */
    u32 num_partition_entries;    /* GPT_PARTITION_ENTRIES_PER_TABLE */
    u32 partition_entry_size;     /* GPT_PARTITION_ENTRY_SIZE */
    u32 partition_entry_array_crc32; /* CRC32 ВСЕЙ таблицы разделов (всех
                                         num_partition_entries записей) */
} gpt_header_t; /* 92 байта полезной нагрузки; на диске дополняется
                    нулями до GPT_SECTOR_SIZE -- см. gpt_serialize_header() */

typedef struct __attribute__((packed)) {
    gpt_guid_t partition_type_guid;   /* GPT_GUID_ESP / GPT_GUID_MEETLOOK_UFS / GPT_GUID_UNUSED */
    gpt_guid_t unique_partition_guid; /* уникальный для КОНКРЕТНОГО раздела, не типа */
    u64 starting_lba;
    u64 ending_lba;    /* включительно (не "первый свободный после") */
    u64 attributes;
    u16 name[36];      /* UTF-16LE, human-readable имя раздела; можно оставить нулевым */
} gpt_partition_entry_t; /* ровно GPT_PARTITION_ENTRY_SIZE (128) байт --
                             см. _Static_assert в lib/gpt.c */

/* Заполняет одну запись таблицы разделов. name -- ASCII-строка (может
 * быть NULL/пустой), конвертируется в UTF-16LE автоматически -- ровно
 * тот минимум, который нужен для читаемости в другом ОС/инструменте, не
 * полноценная поддержка Unicode (нам она не нужна). unique_guid -- см.
 * gpt_guid_from_fields() выше; если ноль-GUID передан явно как
 * unique_guid, gpt_partition_entry_set() СГЕНЕРИРУЕТ псевдослучайный на
 * основе starting_lba/ending_lba (детерминированно, без источника
 * случайности на этом уровне -- см. реализацию) -- unique_partition_guid
 * по спецификации обязан быть unique, но криптографическая случайность
 * здесь не требуется, только различимость между разделами ОДНОГО диска. */
void gpt_partition_entry_set(gpt_partition_entry_t *entry, gpt_guid_t type_guid,
                              u64 starting_lba, u64 ending_lba, const char *name);

/* TRUE, если запись занята (partition_type_guid != GPT_GUID_UNUSED). */
bool_t gpt_partition_is_used(const gpt_partition_entry_t *entry);

/* Собирает ПОЛНЫЙ комплект: первичный заголовок+таблицу и резервный
 * заголовок+таблицу (зеркало) для диска размером disk_sectors секторов.
 * partitions -- массив из GPT_MAX_PARTITIONS записей (незанятые -- см.
 * gpt_partition_is_used() -- заполняются нулями через memset вызывающим
 * кодом, как и с mbr_build()). disk_guid -- см. gpt_guid_from_fields();
 * можно передать один и тот же псевдослучайный GUID, сгенерированный
 * так же, как в gpt_partition_entry_set() для unique_partition_guid.
 *
 * Результат раскладывается в 4 буфера ровно по GPT_SECTOR_SIZE/
 * GPT_PARTITION_TABLE_SECTORS*GPT_SECTOR_SIZE байт каждый -- вызывающий
 * код сам решает, по каким LBA их записывать (обычно: out_primary_header
 * -> LBA 1, out_primary_table -> LBA 2, out_backup_table -> LBA
 * disk_sectors-33, out_backup_header -> LBA disk_sectors-1 -- см.
 * общий комментарий про разметку выше). */
void gpt_build(u64 disk_sectors, gpt_guid_t disk_guid,
               const gpt_partition_entry_t partitions[GPT_MAX_PARTITIONS],
               u8 out_primary_header[GPT_SECTOR_SIZE],
               u8 out_primary_table[GPT_PARTITION_TABLE_SECTORS * GPT_SECTOR_SIZE],
               u8 out_backup_table[GPT_PARTITION_TABLE_SECTORS * GPT_SECTOR_SIZE],
               u8 out_backup_header[GPT_SECTOR_SIZE]);

/* Разбирает и ПРОВЕРЯЕТ (сигнатура, revision, оба CRC32) первичный
 * GPT-заголовок (header_sector -- ровно GPT_SECTOR_SIZE байт, как
 * прочитан с диска) и связанную с ним таблицу разделов (table --
 * GPT_PARTITION_TABLE_SECTORS*GPT_SECTOR_SIZE байт, ГДЕ table уже
 * прочитана вызывающим кодом с LBA, указанного в
 * header->partition_entry_lba -- эта функция сама диск не читает).
 * Копирует до GPT_MAX_PARTITIONS записей в out_partitions. Возвращает
 * FALSE (out_partitions не трогается), если сигнатура/CRC не сошлись --
 * т.е. сектор, скорее всего, не GPT-заголовок (пустой диск, MBR-only
 * диск, мусор). НЕ проверяет резервную копию -- это забота вызывающего
 * кода при желании (см. UEFI_PLAN.md -- реальные прошивки при
 * расхождении первичной/резервной копии обычно предпочитают первичную,
 * если она валидна). */
bool_t gpt_parse(const u8 header_sector[GPT_SECTOR_SIZE],
                  const u8 table[GPT_PARTITION_TABLE_SECTORS * GPT_SECTOR_SIZE],
                  gpt_partition_entry_t out_partitions[GPT_MAX_PARTITIONS]);

#endif
