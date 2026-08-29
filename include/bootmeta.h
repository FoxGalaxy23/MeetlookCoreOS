#ifndef BOOTMETA_H
#define BOOTMETA_H

#include "types.h"

/* bootmeta.h -- формат служебного сектора "boot-meta" (ATOMIC_PLAN.md,
 * Фаза 2 и Фаза 6): какой из двух read-only слотов ОС (A/B) сейчас
 * активен, сколько попыток загрузки осталось и подтверждена ли текущая
 * загрузка. Читается/пишется НАПРЯМУЮ по фиксированному LBA (см.
 * bootmeta_layout.h после Фазы 2 -- пока разметка целиком в
 * ATOMIC_PLAN.md), БЕЗ какой-либо файловой системы поверх -- это
 * первое, что "живой" stage2 (Фаза 6) обязан прочитать, чтобы решить,
 * из какого слота грузить kernel.flat, а до этого момента ни VFS, ни
 * даже сам протокол GPT ещё не разобраны.
 *
 * Как и lib/mbr.c/lib/gpt.c -- никакого I/O здесь нет, чистая логика
 * над буфером в памяти вызывающего (sys_disk_read_raw()/
 * sys_disk_write_raw() -- дело вызывающего кода, apps/install-os.c и,
 * позже, самого stage2). */

#define BOOTMETA_SECTOR_SIZE 512u
#define BOOTMETA_MAGIC        0x4D4C4254u /* "MLBT" (MeetLook BooT), little-endian при записи u32 */

#define BOOTMETA_SLOT_A 0u
#define BOOTMETA_SLOT_B 1u

/* Значение по умолчанию для boot_tries_left при переключении на новый
 * слот (Фаза 7, update-os) -- см. общий комментарий в ATOMIC_PLAN.md,
 * Фаза 6: даёт три попытки успешно догрузиться и вызвать
 * bootmeta_mark_confirmed()/SYS_BOOT_CONFIRM, прежде чем stage2
 * автоматически откатится на прежний слот. */
#define BOOTMETA_DEFAULT_TRIES 3u

typedef struct {
    u32 magic;            /* BOOTMETA_MAGIC -- отличить "размечено" от "мусор/чистый диск" */
    u8  active_slot;       /* BOOTMETA_SLOT_A либо BOOTMETA_SLOT_B */
    u8  boot_tries_left;    /* декрементируется stage2 на каждую НЕподтверждённую попытку */
    u8  boot_confirmed;     /* 0 -- ещё не подтверждено (см. bootmeta_mark_confirmed()), 1 -- подтверждено */
    u8  reserved;          /* всегда 0, выравнивание/запас на будущее */
} bootmeta_t; /* 8 байт полезной нагрузки; на диске дополняется нулями
                  и CRC32 до BOOTMETA_SECTOR_SIZE -- см. bootmeta_serialize() */

/* Заполняет bm значениями "только что установленная система, слот A
 * активен, ничего ещё не откатывалось" -- отправная точка для
 * install-os (Фаза 3/4 плана инсталлятора) при первой установке. */
void bootmeta_init(bootmeta_t *bm);

/* Сериализует bm в ровно BOOTMETA_SECTOR_SIZE байт, готовых к записи
 * по фиксированному LBA (sys_disk_write_raw()). Раскладка:
 *   offset 0   : magic (u32 LE)
 *   offset 4   : active_slot (u8)
 *   offset 5   : boot_tries_left (u8)
 *   offset 6   : boot_confirmed (u8)
 *   offset 7   : reserved (u8)
 *   offset 8   : crc32 (u32 LE) -- CRC32 байт [0..8) по crc32.h
 *   offset 12..511 : нули (запас на будущее -- новые поля добавлять
 *                    ПОСЛЕ reserved, не трогая уже занятые смещения,
 *                    чтобы старый bootmeta_parse() не спотыкался на
 *                    новом формате раньше времени) */
void bootmeta_serialize(const bootmeta_t *bm, u8 out[BOOTMETA_SECTOR_SIZE]);

/* Разбирает и ПРОВЕРЯЕТ (magic, CRC32) сектор. Возвращает FALSE (bm не
 * трогается), если magic не совпал или CRC32 неверен -- то есть сектор,
 * скорее всего, не boot-meta вовсе (чистый/неразмеченный диск, мусор) --
 * ровно тот же принцип, что gpt_parse() в gpt.h. */
bool_t bootmeta_parse(const u8 sector[BOOTMETA_SECTOR_SIZE], bootmeta_t *out);

/* Возвращает номер противоположного слота (A<->B) -- используется и
 * update-os (Фаза 7, переключение на неактивный слот перед записью
 * обновления), и stage2 (Фаза 6, автооткат при исчерпании попыток). */
u8 bootmeta_other_slot(u8 slot);

/* Помечает bm подтверждённым -- boot_confirmed=1, boot_tries_left=0 (та
 * же комбинация, что bootmeta_init() задаёт для первой установки: нет
 * смысла беречь оставшиеся попытки ПОСЛЕ подтверждения -- следующее
 * обновление (Фаза 7, update-os) само выставит новый boot_tries_left,
 * когда переключит active_slot на другой). active_slot/magic/reserved
 * НЕ трогает -- вызывающий код (kernel/bootconfirm.c, Фаза 6) отвечает
 * за то, что bm уже разобран из реального сектора через
 * bootmeta_parse(), не собран с нуля. */
void bootmeta_mark_confirmed(bootmeta_t *bm);

#endif
