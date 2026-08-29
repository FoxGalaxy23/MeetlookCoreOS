#ifndef ATA_H
#define ATA_H

#include "types.h"

#define ATA_MAX_DRIVES 4

typedef struct {
    bool_t present;
    bool_t is_slave;
    u16 io_base;
    u16 ctrl_base;
    u32 sectors;         /* всего секторов (LBA28) */
    char model[41];
} ata_drive_t;

void ata_init(void);
u32 ata_drive_count(void);
ata_drive_t *ata_get_drive(u32 index);
bool_t ata_read_sectors(u32 drive_index, u32 lba, u8 count, void *buf);
bool_t ata_write_sectors(u32 drive_index, u32 lba, u8 count, const void *buf);

#endif
