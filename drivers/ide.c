#include "ide.h"
#include "storage.h"
#include "io.h"
#include "string.h"

#define IDE_MAX_DRIVES 4

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

/* Приватный (для этого драйвера) контекст на каждый обнаруженный диск --
   именно на него указывает storage_dev_t.drv_ctx для дисков с
   bus == STORAGE_BUS_IDE. Верхние слои этот тип не видят. */
typedef struct {
    bool_t is_slave;
    u16    io_base;
    u16    ctrl_base;
} ide_drive_ctx_t;

static ide_drive_ctx_t ide_ctx[IDE_MAX_DRIVES];
static u32 ide_ctx_count = 0;

static void ata_io_wait(u16 io_base) {
    inb(io_base + ATA_REG_STATUS);
    inb(io_base + ATA_REG_STATUS);
    inb(io_base + ATA_REG_STATUS);
    inb(io_base + ATA_REG_STATUS);
}

/* Таймауты busy-wait ниже (изначально 100000, поднято ATOMIC_PLAN.md,
 * Фаза 6) -- живьём пойманный, воспроизводимый случай: на однопроцессорном
 * (или сильно нагруженном/виртуализированном -- см. общий комментарий у
 * ata_poll_bsy_clear/ata_poll_drq в drivers/ide.c) хосте QEMU-поток,
 * эмулирующий гостевой vCPU (который сам крутит эти же busy-wait циклы,
 * ничего не отдавая планировщику), может настолько задавить поток,
 * реально выполняющий запись в файл-образ диска, что 100000 итераций
 * `inb` истекают раньше, чем WRITE PIO успевает завершиться -- РЕАЛЬНО
 * наблюдалось при живом тестировании install-os (ATOMIC_PLAN.md, Фаза 6):
 * первая запись (protective MBR, LBA 0) успевала пройти, а СЛЕДУЮЩАЯ
 * (GPT-заголовок, LBA 1) уже стабильно упиралась в таймаут (status
 * оставался 0x80/0xc0 -- BSY навсегда) -- не логическая ошибка в
 * протоколе (тот байт в байт verified правильный), чисто нехватка
 * бюджета ожидания под нагруженный хост. 500-кратный запас (50000000)
 * не бесплатен (на РЕАЛЬНО неисправном/отключённом диске столько же
 * дольше зависать), но у этого дерева и так нет иного способа сообщить
 * об ошибке диска, кроме зависания (см. общий комментарий в
 * boot/stage2.s про ту же философию) -- корректность важнее скорости
 * диагностики отказа диска, которого здесь и так почти не бывает. */
static bool_t ata_poll_bsy_clear(u16 io_base) {
    u32 timeout = 50000000;
    while ((inb(io_base + ATA_REG_STATUS) & ATA_SR_BSY) && timeout--) ;
    return timeout > 0;
}

static bool_t ata_poll_drq(u16 io_base) {
    u32 timeout = 50000000;
    u8 status;
    while (timeout--) {
        status = inb(io_base + ATA_REG_STATUS);
        if (status & ATA_SR_ERR) return FALSE;
        if (status & ATA_SR_DRQ) return TRUE;
    }
    return FALSE;
}

static bool_t ide_read_sectors(storage_dev_t *dev, u32 lba, u8 count, void *buf) {
    ide_drive_ctx_t *ctx = (ide_drive_ctx_t *)dev->drv_ctx;
    u16 io_base = ctx->io_base;
    u16 *out = (u16 *)buf;

    if (!ata_poll_bsy_clear(io_base)) return FALSE;

    outb(io_base + ATA_REG_HDDEVSEL, (ctx->is_slave ? 0xF0 : 0xE0) | ((lba >> 24) & 0x0F));
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

static bool_t ide_write_sectors(storage_dev_t *dev, u32 lba, u8 count, const void *buf) {
    ide_drive_ctx_t *ctx = (ide_drive_ctx_t *)dev->drv_ctx;
    u16 io_base = ctx->io_base;
    const u16 *in = (const u16 *)buf;

    if (!ata_poll_bsy_clear(io_base)) return FALSE;

    outb(io_base + ATA_REG_HDDEVSEL, (ctx->is_slave ? 0xF0 : 0xE0) | ((lba >> 24) & 0x0F));
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

static const storage_ops_t ide_ops = {
    .read_sectors = ide_read_sectors,
    .write_sectors = ide_write_sectors,
};

static bool_t identify_drive(u16 io_base, u16 ctrl_base, bool_t slave,
                              u32 *out_sectors, char *out_model) {
    (void)ctrl_base;
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

    *out_sectors = storage_identify_get_sectors(identify_buf);
    storage_identify_get_model(identify_buf, out_model);

    return TRUE;
}

void ide_init(void) {
    ide_ctx_count = 0;
    memset(ide_ctx, 0, sizeof(ide_ctx));

    struct { u16 io; u16 ctrl; } channels[2] = {
        { ATA_PRIMARY_IO, ATA_PRIMARY_CTRL },
        { ATA_SECONDARY_IO, ATA_SECONDARY_CTRL }
    };

    for (int ch = 0; ch < 2; ch++) {
        for (int sl = 0; sl < 2; sl++) {
            if (ide_ctx_count >= IDE_MAX_DRIVES) return;

            u32 sectors = 0;
            char model[41];
            bool_t slave = (sl == 1);

            if (!identify_drive(channels[ch].io, channels[ch].ctrl, slave, &sectors, model)) {
                continue;
            }

            ide_drive_ctx_t *ctx = &ide_ctx[ide_ctx_count++];
            ctx->is_slave = slave;
            ctx->io_base = channels[ch].io;
            ctx->ctrl_base = channels[ch].ctrl;

            char location[24];
            location[0] = 'I'; location[1] = 'D'; location[2] = 'E';
            location[3] = (char)('0' + ch);
            location[4] = ' ';
            strcpy(location + 5, slave ? "slave" : "master");

            storage_register(STORAGE_BUS_IDE, sectors, model, location, &ide_ops, ctx);
        }
    }
}
