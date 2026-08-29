#include "usb_msd.h"
#include "storage.h"
#include "string.h"
#include "kprintf.h"
#include "task.h"

#define MSD_SUBCLASS_SCSI 0x06
#define MSD_PROTOCOL_BOT  0x50

#define CBW_SIGNATURE 0x43425355u /* "USBC" (little-endian на проводе) */
#define CSW_SIGNATURE 0x53425355u /* "USBS" */

typedef struct __attribute__((packed)) {
    u32 signature;
    u32 tag;
    u32 data_transfer_length;
    u8  flags;      /* бит 7: 1=IN (данные к хосту), 0=OUT */
    u8  lun;        /* биты 0-3 */
    u8  cb_length;  /* длина CDB, 1-16 */
    u8  cb[16];
} cbw_t; /* 31 байт */

typedef struct __attribute__((packed)) {
    u32 signature;
    u32 tag;
    u32 data_residue;
    u8  status; /* 0 = success, 1 = failed, 2 = phase error */
} csw_t; /* 13 байт */

#define USB_MSD_MAX_DRIVES 4

typedef struct {
    usb_device_t   *dev;
    usb_endpoint_t *ep_in;
    usb_endpoint_t *ep_out;
    u32             tag_counter;
} msd_ctx_t;

static msd_ctx_t g_msd_ctx[USB_MSD_MAX_DRIVES];
static u32        g_msd_count = 0;

/* Таймауты щедрые -- флеш-накопители (особенно дешёвые) могут заметно
   тормозить перед первым ответом после подключения, а запись сектора
   иногда требует ощутимого времени на стирание блока. */
#define MSD_CBW_TIMEOUT_MS   1000
#define MSD_DATA_TIMEOUT_MS  3000
#define MSD_CSW_TIMEOUT_MS   1000

/* Отправляет один Command Block Wrapper, опционально гоняет фазу
   данных (IN или OUT, len байт) и читает Command Status Wrapper.
   Возвращает TRUE только если CSW пришёл с ожидаемым tag'ом и
   status == 0 (success). */
static bool_t msd_command(msd_ctx_t *ctx, const u8 *cdb, u8 cdb_len,
                           void *data, u32 data_len, bool_t data_in) {
    cbw_t cbw;
    memset(&cbw, 0, sizeof(cbw));
    cbw.signature = CBW_SIGNATURE;
    cbw.tag = ++ctx->tag_counter;
    cbw.data_transfer_length = data_len;
    cbw.flags = data_in ? 0x80 : 0x00;
    cbw.lun = 0;
    cbw.cb_length = cdb_len;
    memcpy(cbw.cb, cdb, cdb_len);

    if (usb_data_transfer(ctx->dev, ctx->ep_out, &cbw, sizeof(cbw), MSD_CBW_TIMEOUT_MS) != (s32)sizeof(cbw)) {
        return FALSE;
    }

    if (data_len > 0) {
        usb_endpoint_t *ep = data_in ? ctx->ep_in : ctx->ep_out;
        s32 got = usb_data_transfer(ctx->dev, ep, data, data_len, MSD_DATA_TIMEOUT_MS);
        if (got < 0) return FALSE;
        /* Короткое чтение (got < data_len) допустимо -- некоторые команды
           (например INQUIRY) сознательно запрашивают больше, чем
           устройство готово вернуть; статус разбираем по CSW ниже. */
    }

    csw_t csw;
    memset(&csw, 0, sizeof(csw));
    if (usb_data_transfer(ctx->dev, ctx->ep_in, &csw, sizeof(csw), MSD_CSW_TIMEOUT_MS) != (s32)sizeof(csw)) {
        return FALSE;
    }
    if (csw.signature != CSW_SIGNATURE || csw.tag != cbw.tag) return FALSE;
    return csw.status == 0;
}

static bool_t usb_msd_read_sectors(storage_dev_t *dev, u32 lba, u8 count, void *buf) {
    msd_ctx_t *ctx = (msd_ctx_t *)dev->drv_ctx;
    u8 cdb[10];
    memset(cdb, 0, sizeof(cdb));
    cdb[0] = 0x28; /* READ(10) */
    cdb[2] = (u8)(lba >> 24); cdb[3] = (u8)(lba >> 16);
    cdb[4] = (u8)(lba >> 8);  cdb[5] = (u8)lba;
    cdb[7] = 0; cdb[8] = count; /* transfer length в блоках, старший байт всегда 0 (count -- u8) */
    return msd_command(ctx, cdb, 10, buf, (u32)count * 512u, TRUE);
}

static bool_t usb_msd_write_sectors(storage_dev_t *dev, u32 lba, u8 count, const void *buf) {
    msd_ctx_t *ctx = (msd_ctx_t *)dev->drv_ctx;
    u8 cdb[10];
    memset(cdb, 0, sizeof(cdb));
    cdb[0] = 0x2A; /* WRITE(10) */
    cdb[2] = (u8)(lba >> 24); cdb[3] = (u8)(lba >> 16);
    cdb[4] = (u8)(lba >> 8);  cdb[5] = (u8)lba;
    cdb[7] = 0; cdb[8] = count;
    return msd_command(ctx, cdb, 10, (void *)buf, (u32)count * 512u, FALSE);
}

static const storage_ops_t usb_msd_ops = {
    .read_sectors = usb_msd_read_sectors,
    .write_sectors = usb_msd_write_sectors,
};

void usb_msd_probe(usb_device_t *dev) {
    if (dev->if_subclass != MSD_SUBCLASS_SCSI || dev->if_protocol != MSD_PROTOCOL_BOT) {
        return; /* не SCSI/Bulk-Only -- не поддерживаем (см. usb_msd.h) */
    }
    if (g_msd_count >= USB_MSD_MAX_DRIVES) return;

    usb_endpoint_t *ep_in = NULL, *ep_out = NULL;
    for (u32 i = 1; i < dev->ep_count; i++) {
        if (dev->ep[i].transfer_type != USB_XFER_BULK) continue;
        if (dev->ep[i].is_in) ep_in = &dev->ep[i];
        else ep_out = &dev->ep[i];
    }
    if (!ep_in || !ep_out) return;

    msd_ctx_t *ctx = &g_msd_ctx[g_msd_count];
    memset(ctx, 0, sizeof(*ctx));
    ctx->dev = dev;
    ctx->ep_in = ep_in;
    ctx->ep_out = ep_out;

    /* Диск может не сразу сообщить "готов" сразу после подключения/
       сброса (типично для дешёвых флешек) -- пробуем TEST UNIT READY
       несколько раз, прежде чем сдаться. Неудача этой команды не
       фатальна сама по себе (некоторые накопители всегда отвечают "не
       готов" на первый запрос под управлением host controller reset),
       поэтому результат ниже не проверяется строго -- всё равно
       выясним реальную готовность по успеху READ CAPACITY. */
    u8 tur_cdb[6];
    memset(tur_cdb, 0, sizeof(tur_cdb)); /* TEST UNIT READY: весь CDB нулевой */
    for (int attempt = 0; attempt < 5; attempt++) {
        if (msd_command(ctx, tur_cdb, 6, NULL, 0, TRUE)) break;
        task_sleep(50);
    }

    u8 cap[8];
    memset(cap, 0, sizeof(cap));
    u8 cap_cdb[10];
    memset(cap_cdb, 0, sizeof(cap_cdb));
    cap_cdb[0] = 0x25; /* READ CAPACITY(10) */
    if (!msd_command(ctx, cap_cdb, 10, cap, sizeof(cap), TRUE)) {
        kprintf("USB MSD: устройство не ответило на READ CAPACITY, пропускаем.\n");
        return;
    }
    u32 last_lba = ((u32)cap[0] << 24) | ((u32)cap[1] << 16) | ((u32)cap[2] << 8) | cap[3];
    u32 block_size = ((u32)cap[4] << 24) | ((u32)cap[5] << 16) | ((u32)cap[6] << 8) | cap[7];
    if (block_size == 0) block_size = 512;

    u32 sectors_512;
    if (block_size == 512) {
        sectors_512 = last_lba + 1;
    } else {
        /* Приводим к условным 512-байтным секторам, как ожидает
           storage.h -- достаточно для дисков с блоком, кратным 512
           (обычная ситуация: 1024/2048/4096). Диски с "нестандартным"
           размером блока, не кратным 512, этим драйвером не
           поддерживаются. */
        sectors_512 = (u32)(((u64)(last_lba + 1) * block_size) / 512u);
    }

    u8 inquiry[36];
    memset(inquiry, 0, sizeof(inquiry));
    u8 inq_cdb[6];
    memset(inq_cdb, 0, sizeof(inq_cdb));
    inq_cdb[0] = 0x12; /* INQUIRY */
    inq_cdb[4] = sizeof(inquiry); /* allocation length */
    char model[41];
    memset(model, 0, sizeof(model));
    if (msd_command(ctx, inq_cdb, 6, inquiry, sizeof(inquiry), TRUE)) {
        /* Vendor ID: байты 8-15, Product ID: байты 16-31 (ASCII, дополнены пробелами). */
        char vendor[9], product[17];
        memcpy(vendor, &inquiry[8], 8);  vendor[8] = '\0';  trim(vendor);
        memcpy(product, &inquiry[16], 16); product[16] = '\0'; trim(product);
        strncpy(model, vendor, sizeof(model) - 1);
        model[sizeof(model) - 1] = '\0';
        /* vendor (<=8) + ' ' + product (<=16) <= 25 байт -- заведомо
           укладывается в 41-байтный model, strcat без риска переполнения. */
        if (vendor[0] && product[0]) strcat(model, " ");
        strcat(model, product);
    }
    if (model[0] == '\0') strcpy(model, "USB Mass Storage");

    char location[24];
    strcpy(location, "USB port ");
    char num[8];
    utoa(dev->uhci_port, num, 10);
    strcat(location, num);

    g_msd_count++;
    s32 idx = storage_register(STORAGE_BUS_USB, sectors_512, model, location, &usb_msd_ops, ctx);
    if (idx < 0) {
        kprintf("USB MSD: реестр накопителей полон, устройство не зарегистрировано.\n");
        return;
    }
    kprintf("USB MSD: диск зарегистрирован (%s, %u MB).\n",
            location, (u32)(((u64)sectors_512 * 512u) / (1024u * 1024u)));
}
