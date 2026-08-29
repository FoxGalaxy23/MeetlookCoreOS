#include "ata.h"
#include "io.h"
#include "string.h"

#define ATA_PRIMARY_IO    0x1F0
#define ATA_PRIMARY_CTRL  0x3F6
#define ATA_SECONDARY_IO  0x170
#define ATA_SECONDARY_CTRL 0x376

#define ATA_REG_DATA      0x00
#define ATA_REG_ERROR     0x01
#define ATA_REG_SECCOUNT0 0x02
#define ATA_REG_LBA0      0x03
#define ATA_REG_LBA1      0x04
#define ATA_REG_LBA2      0x05
#define ATA_REG_HDDEVSEL  0x06
#define ATA_REG_COMMAND   0x07
#define ATA_REG_STATUS    0x07

#define ATA_SR_BSY  0x80
#define ATA_SR_DRDY 0x40
#define ATA_SR_DRQ  0x08
#define ATA_SR_ERR  0x01

#define ATA_CMD_READ_PIO   0x20
#define ATA_CMD_WRITE_PIO  0x30
#define ATA_CMD_IDENTIFY   0xEC
#define ATA_CMD_CACHE_FLUSH 0xE7

static ata_drive_t drives[ATA_MAX_DRIVES];
static u32 drive_count = 0;

static void ata_io_wait(u16 io_base) {
    inb(io_base + ATA_REG_STATUS);
    inb(io_base + ATA_REG_STATUS);
    inb(io_base + ATA_REG_STATUS);
    inb(io_base + ATA_REG_STATUS);
}

static bool_t ata_poll_bsy_clear(u16 io_base) {
    u32 timeout = 100000;
    while ((inb(io_base + ATA_REG_STATUS) & ATA_SR_BSY) && timeout--) ;
    return timeout > 0;
}

static bool_t ata_poll_drq(u16 io_base) {
    u32 timeout = 100000;
    u8 status;
    while (timeout--) {
        status = inb(io_base + ATA_REG_STATUS);
        if (status & ATA_SR_ERR) return FALSE;
        if (status & ATA_SR_DRQ) return TRUE;
    }
    return FALSE;
}

static bool_t identify_drive(u16 io_base, u16 ctrl_base, bool_t slave, ata_drive_t *d) {
    outb(io_base + ATA_REG_HDDEVSEL, slave ? 0xB0 : 0xA0);
    io_wait();
    outb(io_base + ATA_REG_SECCOUNT0, 0);
    outb(io_base + ATA_REG_LBA0, 0);
    outb(io_base + ATA_REG_LBA1, 0);
    outb(io_base + ATA_REG_LBA2, 0);
    outb(io_base + ATA_REG_COMMAND, ATA_CMD_IDENTIFY);
    io_wait();

    u8 status = inb(io_base + ATA_REG_STATUS);
    if (status == 0) return FALSE; /* нет накопителя */

    if (!ata_poll_bsy_clear(io_base)) return FALSE;

    /* Проверка на не-ATA устройство (ATAPI и т.п.) через LBA1/LBA2 */
    u8 lba1 = inb(io_base + ATA_REG_LBA1);
    u8 lba2 = inb(io_base + ATA_REG_LBA2);
    if (lba1 != 0 || lba2 != 0) return FALSE;

    if (!ata_poll_drq(io_base)) return FALSE;

    u16 identify_buf[256];
    for (int i = 0; i < 256; i++) identify_buf[i] = inw(io_base + ATA_REG_DATA);

    u32 sectors = ((u32)identify_buf[61] << 16) | identify_buf[60];
    d->present = TRUE;
    d->is_slave = slave;
    d->io_base = io_base;
    d->ctrl_base = ctrl_base;
    d->sectors = sectors;

    /* Модель диска (слова 27-46, байты переставлены местами) */
    for (int i = 0; i < 20; i++) {
        u16 w = identify_buf[27 + i];
        d->model[i*2]   = (char)(w >> 8);
        d->model[i*2+1] = (char)(w & 0xFF);
    }
    d->model[40] = '\0';
    trim(d->model);

    return TRUE;
}

void ata_init(void) {
    drive_count = 0;
    memset(drives, 0, sizeof(drives));

    struct { u16 io; u16 ctrl; } channels[2] = {
        { ATA_PRIMARY_IO, ATA_PRIMARY_CTRL },
        { ATA_SECONDARY_IO, ATA_SECONDARY_CTRL }
    };

    for (int ch = 0; ch < 2; ch++) {
        for (int sl = 0; sl < 2; sl++) {
            ata_drive_t tmp;
            memset(&tmp, 0, sizeof(tmp));
            if (identify_drive(channels[ch].io, channels[ch].ctrl, sl == 1, &tmp)) {
                drives[drive_count++] = tmp;
            }
        }
    }
}

u32 ata_drive_count(void) {
    return drive_count;
}

ata_drive_t *ata_get_drive(u32 index) {
    if (index >= drive_count) return NULL;
    return &drives[index];
}

bool_t ata_read_sectors(u32 drive_index, u32 lba, u8 count, void *buf) {
    if (drive_index >= drive_count) return FALSE;
    ata_drive_t *d = &drives[drive_index];
    u16 io_base = d->io_base;
    u16 *out = (u16*)buf;

    if (!ata_poll_bsy_clear(io_base)) return FALSE;

    outb(io_base + ATA_REG_HDDEVSEL, (d->is_slave ? 0xF0 : 0xE0) | ((lba >> 24) & 0x0F));
    outb(io_base + ATA_REG_SECCOUNT0, count);
    outb(io_base + ATA_REG_LBA0, (u8)(lba & 0xFF));
    outb(io_base + ATA_REG_LBA1, (u8)((lba >> 8) & 0xFF));
    outb(io_base + ATA_REG_LBA2, (u8)((lba >> 16) & 0xFF));
    outb(io_base + ATA_REG_COMMAND, ATA_CMD_READ_PIO);

    for (u8 s = 0; s < count; s++) {
        if (!ata_poll_bsy_clear(io_base)) return FALSE;
        if (!ata_poll_drq(io_base)) return FALSE;
        for (int i = 0; i < 256; i++) {
            out[s * 256 + i] = inw(io_base + ATA_REG_DATA);
        }
        ata_io_wait(io_base);
    }
    return TRUE;
}

bool_t ata_write_sectors(u32 drive_index, u32 lba, u8 count, const void *buf) {
    if (drive_index >= drive_count) return FALSE;
    ata_drive_t *d = &drives[drive_index];
    u16 io_base = d->io_base;
    const u16 *in = (const u16*)buf;

    if (!ata_poll_bsy_clear(io_base)) return FALSE;

    outb(io_base + ATA_REG_HDDEVSEL, (d->is_slave ? 0xF0 : 0xE0) | ((lba >> 24) & 0x0F));
    outb(io_base + ATA_REG_SECCOUNT0, count);
    outb(io_base + ATA_REG_LBA0, (u8)(lba & 0xFF));
    outb(io_base + ATA_REG_LBA1, (u8)((lba >> 8) & 0xFF));
    outb(io_base + ATA_REG_LBA2, (u8)((lba >> 16) & 0xFF));
    outb(io_base + ATA_REG_COMMAND, ATA_CMD_WRITE_PIO);

    for (u8 s = 0; s < count; s++) {
        if (!ata_poll_bsy_clear(io_base)) return FALSE;
        if (!ata_poll_drq(io_base)) return FALSE;
        for (int i = 0; i < 256; i++) {
            outw(io_base + ATA_REG_DATA, in[s * 256 + i]);
        }
        ata_io_wait(io_base);
    }
    outb(io_base + ATA_REG_COMMAND, ATA_CMD_CACHE_FLUSH);
    ata_poll_bsy_clear(io_base);
    return TRUE;
}
