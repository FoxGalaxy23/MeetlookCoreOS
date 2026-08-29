#include "bootconfirm.h"
#include "bootmeta.h"
#include "storage.h"
#include "string.h"

/* См. include/bootconfirm.h. */

static bool_t s_valid = FALSE;
static u32 s_drive = 0;
static u32 s_bootmeta_lba = 0;
static u8  s_expected_slot = 0;

void bootconfirm_init(const boot_info_t *bi) {
    s_valid = bi->atomic_boot_valid;
    s_drive = bi->atomic_boot_drive;
    s_bootmeta_lba = bi->atomic_bootmeta_lba;
    s_expected_slot = bi->atomic_active_slot;
}

bool_t bootconfirm_mark_ok(void) {
    if (!s_valid) return FALSE; /* не Atomic-загрузка -- см. общий комментарий в bootconfirm.h */

    u8 sector[BOOTMETA_SECTOR_SIZE];
    if (!storage_read_sectors(s_drive, s_bootmeta_lba, 1u, sector)) return FALSE;

    bootmeta_t bm;
    if (!bootmeta_parse(sector, &bm)) return FALSE;

    /* Защита от TOCTOU -- см. общий комментарий у bootconfirm_mark_ok(),
       include/bootconfirm.h, за тем, почему это дёшево и не помешает. */
    if (bm.active_slot != s_expected_slot) return FALSE;

    if (bm.boot_confirmed) return TRUE; /* уже подтверждено -- писать нечего, не ошибка */

    bootmeta_mark_confirmed(&bm);

    u8 out[BOOTMETA_SECTOR_SIZE];
    bootmeta_serialize(&bm, out); /* зануляет и пересобирает ВЕСЬ сектор целиком сама, см. её
                                      комментарий, lib/bootmeta.c -- ничего домешивать из sector[]
                                      не нужно */
    if (!storage_write_sectors(s_drive, s_bootmeta_lba, 1u, out)) return FALSE;

    return TRUE;
}
