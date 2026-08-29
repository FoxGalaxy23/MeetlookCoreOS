/* mbr.c -- реализация mbr.h. Чистая логика над буфером в памяти, без
 * единой операции ввода-вывода -- см. общий комментарий в mbr.h. */

#include "mbr.h"
#include "string.h"

_Static_assert(sizeof(mbr_partition_t) == 16,
    "mbr_partition_t должна быть ровно 16 байт -- формат MBR это жёстко фиксирует");
_Static_assert(sizeof(mbr_sector_t) == MBR_SECTOR_SIZE,
    "mbr_sector_t должна быть ровно 512 байт -- иначе она не совпадает с сектором диска");

void mbr_partition_set(mbr_partition_t *p, bool_t bootable, u8 type,
                        u32 lba_start, u32 sectors) {
    p->boot_flag = bootable ? MBR_PART_BOOTABLE : MBR_PART_NONBOOTABLE;
    /* 0xFE/0xFF/0xFF -- заведомо "переполненное" CHS-значение, тот же
     * приём, каким современные fdisk/GRUB помечают "не считайте CHS,
     * это чистый LBA-раздел" (см. общий комментарий в mbr.h). */
    p->chs_start[0] = 0xFE; p->chs_start[1] = 0xFF; p->chs_start[2] = 0xFF;
    p->chs_end[0]   = 0xFE; p->chs_end[1]   = 0xFF; p->chs_end[2]   = 0xFF;
    p->type = type;
    p->lba_start = lba_start;
    p->sectors = sectors;
}

void mbr_build(mbr_sector_t *out, const void *boot_code, u32 boot_code_len,
               const mbr_partition_t partitions[MBR_PARTITION_COUNT]) {
    memset(out, 0, sizeof(*out));

    u32 copy_len = boot_code_len < MBR_BOOTCODE_SIZE ? boot_code_len : MBR_BOOTCODE_SIZE;
    if (boot_code && copy_len > 0) memcpy(out->boot_code, boot_code, copy_len);
    /* остаток out->boot_code (если boot_code короче MBR_BOOTCODE_SIZE)
     * уже нулевой из-за memset() выше */

    for (u32 i = 0; i < MBR_PARTITION_COUNT; i++) out->partitions[i] = partitions[i];

    out->signature = (u16)MBR_SIGNATURE;
}

bool_t mbr_parse(const mbr_sector_t *sector, mbr_partition_t out_partitions[MBR_PARTITION_COUNT]) {
    if (sector->signature != (u16)MBR_SIGNATURE) return FALSE;
    for (u32 i = 0; i < MBR_PARTITION_COUNT; i++) out_partitions[i] = sector->partitions[i];
    return TRUE;
}

bool_t mbr_partition_is_used(const mbr_partition_t *p) {
    return p->type != MBR_PART_TYPE_EMPTY && p->sectors != 0;
}
