#ifndef MBR_H
#define MBR_H

#include "types.h"

/* mbr.h -- сборка/разбор классического MBR (Master Boot Record) в
 * памяти. См. INSTALLER_PLAN.md, Фаза 1.
 *
 * НИКАКОГО I/O здесь нет -- ровно тот же принцип, что и у lib/mldb.c:
 * вся логика работает над буфером mbr_sector_t в памяти вызывающего.
 * Кто и когда пишет результат на диск (через sys_disk_write_raw(),
 * см. include/usyscall.h) -- решает вызывающий код (apps/install-os.c
 * в будущей Фазе 4), а не эта библиотека. Такое разделение позволяет
 * (и уже используется в apps/mbrtest.c) собрать и проверить MBR
 * hex-дампом, ни разу не тронув настоящий диск.
 *
 * ФОРМАТ. Классический (не GPT) MBR -- ровно один сектор (512 байт):
 *   байты   0..445  -- код загрузчика (у нас сюда кладётся GRUB boot.img,
 *                       см. Фазу 2 плана; сам mbr.c его не разбирает и не
 *                       генерирует -- это чужой, заранее собранный blob)
 *   байты 446..509  -- 4 записи по 16 байт, таблица разделов
 *   байты 510..511  -- сигнатура 0x55 0xAA (little-endian u16 = 0xAA55)
 *
 * Мы поддерживаем только классические, не расширенные (extended/logical)
 * разделы -- для инсталлятора одной ОС на весь диск (Фаза 1 плана: одна
 * primary-партиция FAT32 на весь диск) этого достаточно с большим
 * запасом. CHS-поля (chs_start/chs_end) заполняются заведомо
 * "переполненным" значением 0xFE/0xFF/0xFF, сигнализирующим BIOS'у и
 * ОС "используйте LBA-поля, CHS не считайте" -- ровно так делает
 * современный fdisk/GRUB при разметке диска в LBA-режиме. */

#define MBR_SECTOR_SIZE      512u
#define MBR_BOOTCODE_SIZE    446u  /* байты 0..445 */
#define MBR_PARTITION_COUNT  4u
#define MBR_SIGNATURE        0xAA55u /* на диске -- байты 0x55, 0xAA (little-endian u16) */

/* Типы разделов (только те, что нужны инсталлятору; полный список типов
 * MBR исторически огромен и здесь не воспроизводится). */
#define MBR_PART_TYPE_EMPTY     0x00u
#define MBR_PART_TYPE_FAT32_LBA 0x0Cu /* FAT32, LBA-адресация (INT 13h extensions) -- тип раздела для нашего инсталлятора */
#define MBR_PART_TYPE_GPT_PROTECTIVE 0xEEu /* "protective MBR" перед настоящим GPT (UEFI_PLAN.md,
 * Фаза A; ATOMIC_PLAN.md, Фаза 0/2) -- один раздел этого типа,
 * покрывающий весь диск (starting_lba=1, sectors=disk_sectors-1,
 * с насыщением до 0xFFFFFFFF на дисках больше ~2 TiB по спецификации),
 * не bootable. Нужен ТОЛЬКО для совместимости со старыми
 * MBR-инструментами ("не трогай этот диск, я его не понимаю") --
 * сам mbr.c его не разбирает по-особому, это обычная запись типа
 * mbr_partition_set(&p, FALSE, MBR_PART_TYPE_GPT_PROTECTIVE, 1, sectors). */

#define MBR_PART_TYPE_MEETLOOK_UFS 0x7Fu /* MeetlookCoreOS UFS (fs/ufs.c, СВОЙ, НЕСТАНДАРТНЫЙ формат --
 * не путать с BSD/Solaris UFS, у которых официальный тип 0xA5. 0x7F
 * официально зарезервирован спецификацией MBR под "reserved for
 * individual/personal use" -- ровно наш случай: собственный, ничем не
 * совместимый формат нашего собственного драйвера, fs/ufs.c. Инструменты
 * сторонних ОС корректно покажут этот раздел как "неизвестный тип",
 * не попытаются его смонтировать как что-то другое -- то, что и нужно.
 * Реального различения типов при монтировании эта константа НЕ
 * определяет -- fs_mount()/detect_partition_range() (fs/fat32.c,
 * fs/ufs.c) находят раздел по факту "использован" (mbr_partition_is_used()),
 * а какая именно ФС на нём -- определяется сигнатурой при попытке
 * монтирования каждым драйвером по очереди (см. include/vfs.h,
 * fs_mount()). Тип в MBR -- для внешних инструментов/человека, не для
 * нашей собственной логики. */

#define MBR_PART_BOOTABLE    0x80u
#define MBR_PART_NONBOOTABLE 0x00u

typedef struct __attribute__((packed)) {
    u8  boot_flag;     /* MBR_PART_BOOTABLE или MBR_PART_NONBOOTABLE */
    u8  chs_start[3];  /* не используется -- см. общий комментарий выше про LBA-режим */
    u8  type;          /* MBR_PART_TYPE_* */
    u8  chs_end[3];    /* не используется */
    u32 lba_start;     /* первый сектор раздела, абсолютный LBA от начала диска */
    u32 sectors;       /* длина раздела в секторах */
} mbr_partition_t; /* ровно 16 байт -- см. _Static_assert в mbr.c */

typedef struct __attribute__((packed)) {
    u8              boot_code[MBR_BOOTCODE_SIZE];
    mbr_partition_t partitions[MBR_PARTITION_COUNT];
    u16             signature; /* всегда MBR_SIGNATURE после mbr_build() */
} mbr_sector_t; /* ровно MBR_SECTOR_SIZE (512) байт -- см. _Static_assert в mbr.c;
                    указатель на неё можно отдавать напрямую в
                    sys_disk_write_raw(drive, 0, 1, &sector) без какой-либо
                    доп. сериализации, т.к. вся структура packed и уже
                    в точности совпадает с байтами диска. */

/* Заполняет одну запись таблицы разделов. Ставит chs_start/chs_end в
 * "переполненное" LBA-заглушающее значение (см. общий комментарий выше)
 * -- вызывающему коду эти поля трогать не нужно. bootable/type/lba_start/
 * sectors -- ровно то, что попадёт на диск. */
void mbr_partition_set(mbr_partition_t *p, bool_t bootable, u8 type,
                        u32 lba_start, u32 sectors);

/* Собирает готовый сектор: копирует boot_code (длиной boot_code_len,
 * МАКСИМУМ MBR_BOOTCODE_SIZE байт -- лишнее обрезается, недостающее
 * дополняется нулями), копирует 4 записи partitions[] как есть (для
 * неиспользуемых слотов передайте запись, обнулённую memset()'ом --
 * boot_flag=0x00, type=MBR_PART_TYPE_EMPTY, lba_start=0, sectors=0),
 * прописывает сигнатуру 0xAA55. out полностью перезаписывается --
 * предварительно обнулять его не нужно. */
void mbr_build(mbr_sector_t *out, const void *boot_code, u32 boot_code_len,
               const mbr_partition_t partitions[MBR_PARTITION_COUNT]);

/* Разбирает уже прочитанный с диска (или собранный mbr_build()) сектор:
 * проверяет сигнатуру 0xAA55 и копирует 4 записи таблицы разделов в
 * out_partitions. Возвращает FALSE (out_partitions не трогается), если
 * сигнатура неверна -- т.е. сектор, скорее всего, не MBR (пустой диск,
 * GPT protective MBR с иным содержимым, мусор). */
bool_t mbr_parse(const mbr_sector_t *sector, mbr_partition_t out_partitions[MBR_PARTITION_COUNT]);

/* TRUE, если запись partitions[index] не пуста (type != MBR_PART_TYPE_EMPTY
 * И sectors != 0) -- удобный хелпер вместо ручной проверки обоих полей
 * в вызывающем коде. */
bool_t mbr_partition_is_used(const mbr_partition_t *p);

#endif /* MBR_H */
