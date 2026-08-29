#include "ahci.h"
#include "storage.h"
#include "pci.h"
#include "string.h"
#include "kprintf.h"

/* ahci.c -- минимальный синхронный (без прерываний, без NCQ) драйвер
 * контроллера AHCI. Задача та же, что у ide.c: найти диски и
 * зарегистрировать их через storage_register(), реализовав
 * storage_ops_t для чтения/записи секторов.
 *
 * Модель работы соответствует остальной части ядра: один поток
 * управления, всё синхронно через опрос регистров (poll), команды
 * шлются по одной (всегда через слот 0 -- очередь глубиной 1,
 * параллелизм NCQ не используется). Этого достаточно для FAT32 и
 * shell, которым не требуется высокая производительность диска.
 */

#define AHCI_MAX_PORTS 4  /* + IDE_MAX_DRIVES(4) = STORAGE_MAX_DRIVES(8) */

#define PCI_CLASS_MASS_STORAGE 0x01
#define PCI_SUBCLASS_SATA      0x06
#define PCI_PROGIF_AHCI        0x01

#define HBA_GHC_AE   (1u << 31)

#define HBA_PORT_CMD_ST   (1u << 0)
#define HBA_PORT_CMD_FRE  (1u << 4)
#define HBA_PORT_CMD_FR   (1u << 14)
#define HBA_PORT_CMD_CR   (1u << 15)

#define HBA_PORT_IS_TFES  (1u << 30)

#define SATA_SIG_ATA   0x00000101u

#define ATA_CMD_IDENTIFY      0xEC
#define ATA_CMD_READ_DMA_EXT  0x25
#define ATA_CMD_WRITE_DMA_EXT 0x35

#define ATA_STS_BSY 0x80
#define ATA_STS_DRQ 0x08

typedef struct __attribute__((packed)) {
    u32 clb, clbu;
    u32 fb, fbu;
    u32 is, ie;
    u32 cmd;
    u32 rsv0;
    u32 tfd;
    u32 sig;
    u32 ssts, sctl, serr, sact;
    u32 ci;
    u32 sntf;
    u32 fbs;
    u32 rsv1[11];
    u32 vendor[4];
} hba_port_t;

typedef struct __attribute__((packed)) {
    u32 cap, ghc, is, pi, vs;
    u32 ccc_ctl, ccc_pts, em_loc, em_ctl;
    u32 cap2, bohc;
    u8  rsv[0xA0 - 0x2C];
    u8  vendor[0x100 - 0xA0];
    hba_port_t ports[32];
} hba_mem_t;

typedef struct __attribute__((packed)) {
    u8 cfl_a_w_p;      /* bits: cfl(0-4), a(5), w(6), p(7) */
    u8 r_b_c_rsv_pmp;  /* bits: r(0), b(1), c(2), rsv(3), pmp(4-7) */
    u16 prdtl;
    u32 prdbc;
    u32 ctba, ctbau;
    u32 rsv1[4];
} hba_cmd_header_t;

typedef struct __attribute__((packed)) {
    u32 dba, dbau;
    u32 rsv0;
    u32 dbc_i; /* bits: byte count - 1 (0-21), rsv(22-30), interrupt on completion (31) */
} hba_prdt_entry_t;

typedef struct __attribute__((packed)) {
    u8 cfis[64];
    u8 acmd[16];
    u8 rsv[48];
    hba_prdt_entry_t prdt_entry[1]; /* нам достаточно одного дескриптора */
} hba_cmd_tbl_t;

typedef struct __attribute__((packed)) {
    u8  fis_type;      /* 0x27 = FIS_TYPE_REG_H2D */
    u8  pmport_c;       /* bits: pmport(0-3), rsv(4-6), c(7) */
    u8  command;
    u8  featurel;
    u8  lba0, lba1, lba2, device;
    u8  lba3, lba4, lba5, featureh;
    u8  countl, counth, icc, control;
    u8  rsv1[4];
} fis_reg_h2d_t;

/* Буферы под командные структуры каждого используемого порта.
   aligned(4096) избыточно (реальные требования: 1024/256/128 байт), но
   гарантированно удовлетворяет их все и не требует ручного выравнивания. */
static u8 g_cmd_list[AHCI_MAX_PORTS][1024] __attribute__((aligned(4096)));
static u8 g_fis_recv[AHCI_MAX_PORTS][256]   __attribute__((aligned(4096)));
static u8 g_cmd_tbl[AHCI_MAX_PORTS][256]    __attribute__((aligned(4096)));

typedef struct {
    volatile hba_port_t *port;
} ahci_drive_ctx_t;

static ahci_drive_ctx_t ahci_ctx[AHCI_MAX_PORTS];
static u32 ahci_ctx_count = 0;

static bool_t ahci_port_wait_not_busy(volatile hba_port_t *port) {
    u32 timeout = 1000000;
    while ((port->tfd & (ATA_STS_BSY | ATA_STS_DRQ)) && timeout--) ;
    return timeout > 0;
}

static void ahci_port_stop(volatile hba_port_t *port) {
    port->cmd &= ~HBA_PORT_CMD_ST;
    u32 timeout = 1000000;
    while ((port->cmd & HBA_PORT_CMD_CR) && timeout--) ;

    port->cmd &= ~HBA_PORT_CMD_FRE;
    timeout = 1000000;
    while ((port->cmd & HBA_PORT_CMD_FR) && timeout--) ;
}

static void ahci_port_start(volatile hba_port_t *port) {
    u32 timeout = 1000000;
    while ((port->cmd & HBA_PORT_CMD_CR) && timeout--) ;
    port->cmd |= HBA_PORT_CMD_FRE;
    port->cmd |= HBA_PORT_CMD_ST;
}

/* Привязывает командные структуры (буферы выше) к порту и переводит
   его в рабочее состояние. slot_idx -- индекс в g_cmd_list/g_fis_recv/
   g_cmd_tbl (не номер порта AHCI -- у нас на каждый *используемый*
   диск свой набор буферов, нумеруемый по порядку обнаружения). */
static void ahci_port_rebase(volatile hba_port_t *port, u32 slot_idx) {
    ahci_port_stop(port);

    memset(g_cmd_list[slot_idx], 0, sizeof(g_cmd_list[slot_idx]));
    memset(g_fis_recv[slot_idx], 0, sizeof(g_fis_recv[slot_idx]));
    memset(g_cmd_tbl[slot_idx], 0, sizeof(g_cmd_tbl[slot_idx]));

    port->clb  = (u32)(u64)g_cmd_list[slot_idx];
    port->clbu = 0;
    port->fb   = (u32)(u64)g_fis_recv[slot_idx];
    port->fbu  = 0;

    hba_cmd_header_t *hdr = (hba_cmd_header_t *)g_cmd_list[slot_idx];
    hdr[0].prdtl = 1;
    hdr[0].ctba = (u32)(u64)g_cmd_tbl[slot_idx];
    hdr[0].ctbau = 0;

    port->serr = 0xFFFFFFFF; /* очистить ошибки, накопленные до нас */
    port->is = 0xFFFFFFFF;

    ahci_port_start(port);
}

/* Общая отправка ATA-команды (identify / read / write) через слот 0.
   write == TRUE для WRITE_DMA_EXT, FALSE для READ_DMA_EXT/IDENTIFY. */
static bool_t ahci_send_command(volatile hba_port_t *port, u32 slot_idx,
                                 u8 ata_command, u32 lba, u16 count,
                                 void *buf, bool_t is_write) {
    if (!ahci_port_wait_not_busy(port)) return FALSE;

    hba_cmd_header_t *hdr = (hba_cmd_header_t *)g_cmd_list[slot_idx];
    hba_cmd_tbl_t *tbl = (hba_cmd_tbl_t *)g_cmd_tbl[slot_idx];
    memset(tbl, 0, sizeof(hba_cmd_tbl_t));

    u32 byte_count = (u32)count * 512;
    if (byte_count == 0) byte_count = 512; /* IDENTIFY: ровно один сектор данных */

    tbl->prdt_entry[0].dba = (u32)(u64)buf;
    tbl->prdt_entry[0].dbau = 0;
    tbl->prdt_entry[0].dbc_i = (byte_count - 1) & 0x3FFFFF; /* i=0: прерывания не используем */

    fis_reg_h2d_t *fis = (fis_reg_h2d_t *)tbl->cfis;
    fis->fis_type = 0x27;
    fis->pmport_c = 1 << 7; /* c=1: это команда (не обновление статуса) */
    fis->command = ata_command;
    fis->device = 1 << 6;   /* LBA mode */
    fis->lba0 = (u8)(lba & 0xFF);
    fis->lba1 = (u8)((lba >> 8) & 0xFF);
    fis->lba2 = (u8)((lba >> 16) & 0xFF);
    fis->lba3 = (u8)((lba >> 24) & 0xFF);
    fis->lba4 = 0;
    fis->lba5 = 0;
    fis->countl = (u8)(count & 0xFF);
    fis->counth = (u8)((count >> 8) & 0xFF);

    hdr[0].cfl_a_w_p = (u8)(5 /* dwords в cfis: sizeof(fis_reg_h2d_t)/4 */
                             | (is_write ? (1 << 6) : 0));
    hdr[0].prdtl = 1;
    hdr[0].prdbc = 0;

    port->is = 0xFFFFFFFF;
    port->ci |= 1u; /* слот 0 */

    u32 timeout = 5000000;
    while (port->ci & 1u) {
        if (port->is & HBA_PORT_IS_TFES) return FALSE;
        if (--timeout == 0) return FALSE;
    }
    if (port->is & HBA_PORT_IS_TFES) return FALSE;
    return TRUE;
}

static bool_t ahci_read_sectors(storage_dev_t *dev, u32 lba, u8 count, void *buf) {
    ahci_drive_ctx_t *ctx = (ahci_drive_ctx_t *)dev->drv_ctx;
    u32 slot_idx = (u32)(ctx - ahci_ctx);
    return ahci_send_command(ctx->port, slot_idx, ATA_CMD_READ_DMA_EXT, lba, count, buf, FALSE);
}

static bool_t ahci_write_sectors(storage_dev_t *dev, u32 lba, u8 count, const void *buf) {
    ahci_drive_ctx_t *ctx = (ahci_drive_ctx_t *)dev->drv_ctx;
    u32 slot_idx = (u32)(ctx - ahci_ctx);
    return ahci_send_command(ctx->port, slot_idx, ATA_CMD_WRITE_DMA_EXT, lba, count, (void *)buf, TRUE);
}

static const storage_ops_t ahci_ops = {
    .read_sectors = ahci_read_sectors,
    .write_sectors = ahci_write_sectors,
};

static bool_t ahci_port_has_drive(volatile hba_port_t *port) {
    u8 ipm = (u8)((port->ssts >> 8) & 0x0F);
    u8 det = (u8)(port->ssts & 0x0F);
    if (det != 3 || ipm != 1) return FALSE; /* устройство не подключено/PHY не готов */
    return port->sig == SATA_SIG_ATA;        /* игнорируем ATAPI/PM/SEMB */
}

void ahci_init(void) {
    ahci_ctx_count = 0;
    memset(ahci_ctx, 0, sizeof(ahci_ctx));

    pci_device_t pci_dev;
    if (!pci_find_device(PCI_CLASS_MASS_STORAGE, PCI_SUBCLASS_SATA, PCI_PROGIF_AHCI, &pci_dev)) {
        return; /* нет контроллера AHCI -- это нормально, просто пропускаем */
    }

    /* Включаем Memory Space и Bus Master в командном регистре PCI --
       без этого контроллер не будет отвечать на MMIO и не будет мастером
       шины для DMA-передач. */
    u16 pci_cmd = pci_config_read16(pci_dev.bus, pci_dev.device, pci_dev.function, 0x04);
    pci_cmd |= (1 << 1) | (1 << 2);
    pci_config_write16(pci_dev.bus, pci_dev.device, pci_dev.function, 0x04, pci_cmd);

    u32 abar = pci_dev.bar[5] & 0xFFFFFFF0u; /* BAR5 = ABAR, память, 32-битный */
    if (abar == 0) return;

    volatile hba_mem_t *hba = (volatile hba_mem_t *)(u64)abar;
    hba->ghc |= HBA_GHC_AE; /* сообщаем контроллеру, что им управляет AHCI-драйвер */

    for (u32 i = 0; i < 32 && ahci_ctx_count < AHCI_MAX_PORTS; i++) {
        if (!(hba->pi & (1u << i))) continue; /* порт не реализован контроллером */

        volatile hba_port_t *port = &hba->ports[i];
        if (!ahci_port_has_drive(port)) continue;

        u32 slot_idx = ahci_ctx_count;
        ahci_port_rebase(port, slot_idx);

        u16 identify_buf[256];
        memset(identify_buf, 0, sizeof(identify_buf));
        if (!ahci_send_command(port, slot_idx, ATA_CMD_IDENTIFY, 0, 0, identify_buf, FALSE)) {
            continue; /* диск засветился на шине, но не ответил -- пропускаем */
        }

        ahci_drive_ctx_t *ctx = &ahci_ctx[ahci_ctx_count++];
        ctx->port = port;

        u32 sectors = storage_identify_get_sectors(identify_buf);
        char model[41];
        storage_identify_get_model(identify_buf, model);

        char location[24];
        strcpy(location, "SATA port ");
        char num[8];
        utoa(i, num, 10);
        strcat(location, num);

        storage_register(STORAGE_BUS_SATA, sectors, model, location, &ahci_ops, ctx);
    }
}
