#include "bootmeta.h"
#include "crc32.h"
#include "string.h"

/* См. include/bootmeta.h. */

void bootmeta_init(bootmeta_t *bm) {
    bm->magic = BOOTMETA_MAGIC;
    bm->active_slot = BOOTMETA_SLOT_A;
    bm->boot_tries_left = 0;   /* только что установленная система -- не в процессе апдейта */
    bm->boot_confirmed = 1;    /* первая установка считается сразу подтверждённой -- нет
                                   "предыдущего рабочего слота", на который можно откатиться */
    bm->reserved = 0;
}

/* Общая для serialize/parse раскладка первых 8 байт -- вынесено, чтобы
 * не продублировать смещения по ошибке в двух местах. */
static void pack_fields(const bootmeta_t *bm, u8 buf[8]) {
    buf[0] = (u8)(bm->magic & 0xFF);
    buf[1] = (u8)((bm->magic >> 8) & 0xFF);
    buf[2] = (u8)((bm->magic >> 16) & 0xFF);
    buf[3] = (u8)((bm->magic >> 24) & 0xFF);
    buf[4] = bm->active_slot;
    buf[5] = bm->boot_tries_left;
    buf[6] = bm->boot_confirmed;
    buf[7] = bm->reserved;
}

void bootmeta_serialize(const bootmeta_t *bm, u8 out[BOOTMETA_SECTOR_SIZE]) {
    memset(out, 0, BOOTMETA_SECTOR_SIZE);

    u8 fields[8];
    pack_fields(bm, fields);
    memcpy(out, fields, 8);

    u32 crc = crc32(0, fields, 8);
    out[8]  = (u8)(crc & 0xFF);
    out[9]  = (u8)((crc >> 8) & 0xFF);
    out[10] = (u8)((crc >> 16) & 0xFF);
    out[11] = (u8)((crc >> 24) & 0xFF);
    /* offset 12..511 уже нули из memset выше. */
}

bool_t bootmeta_parse(const u8 sector[BOOTMETA_SECTOR_SIZE], bootmeta_t *out) {
    u32 magic = (u32)sector[0] | ((u32)sector[1] << 8) |
                ((u32)sector[2] << 16) | ((u32)sector[3] << 24);
    if (magic != BOOTMETA_MAGIC) return FALSE;

    u32 stored_crc = (u32)sector[8] | ((u32)sector[9] << 8) |
                      ((u32)sector[10] << 16) | ((u32)sector[11] << 24);
    u32 actual_crc = crc32(0, sector, 8);
    if (stored_crc != actual_crc) return FALSE;

    out->magic = magic;
    out->active_slot = sector[4];
    out->boot_tries_left = sector[5];
    out->boot_confirmed = sector[6];
    out->reserved = sector[7];
    return TRUE;
}

u8 bootmeta_other_slot(u8 slot) {
    return (slot == BOOTMETA_SLOT_A) ? BOOTMETA_SLOT_B : BOOTMETA_SLOT_A;
}

void bootmeta_mark_confirmed(bootmeta_t *bm) {
    bm->boot_confirmed = 1;
    bm->boot_tries_left = 0;
}
