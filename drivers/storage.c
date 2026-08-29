#include "storage.h"
#include "ide.h"
#include "ahci.h"
#include "string.h"
#include "kprintf.h"

static storage_dev_t drives[STORAGE_MAX_DRIVES];
static u32 drive_count = 0;

void storage_init(void) {
    drive_count = 0;
    memset(drives, 0, sizeof(drives));

    /* Сначала старый IDE (PATA), затем SATA через AHCI -- порядок
       определяет только то, в каком порядке диски получат свои индексы
       (совпадает с прежним поведением, когда был только IDE). */
    ide_init();
    ahci_init();
}

s32 storage_register(storage_bus_t bus, u32 sectors, const char *model,
                      const char *location, const storage_ops_t *ops, void *drv_ctx) {
    if (drive_count >= STORAGE_MAX_DRIVES) return -1;

    storage_dev_t *d = &drives[drive_count];
    d->present = TRUE;
    d->bus = bus;
    d->sectors = sectors;
    strncpy(d->model, model, sizeof(d->model) - 1);
    d->model[sizeof(d->model) - 1] = '\0';
    strncpy(d->location, location, sizeof(d->location) - 1);
    d->location[sizeof(d->location) - 1] = '\0';
    d->ops = ops;
    d->drv_ctx = drv_ctx;

    return (s32)(drive_count++);
}

u32 storage_drive_count(void) {
    return drive_count;
}

storage_dev_t *storage_get_drive(u32 index) {
    if (index >= drive_count) return NULL;
    return &drives[index];
}

bool_t storage_read_sectors(u32 drive_index, u32 lba, u8 count, void *buf) {
    if (drive_index >= drive_count) return FALSE;
    storage_dev_t *d = &drives[drive_index];
    return d->ops->read_sectors(d, lba, count, buf);
}

bool_t storage_write_sectors(u32 drive_index, u32 lba, u8 count, const void *buf) {
    if (drive_index >= drive_count) return FALSE;
    storage_dev_t *d = &drives[drive_index];
    return d->ops->write_sectors(d, lba, count, buf);
}

u32 storage_identify_get_sectors(const u16 *identify_buf) {
    u32 sectors28 = ((u32)identify_buf[61] << 16) | identify_buf[60];

    bool_t lba48_supported = (identify_buf[83] & (1 << 10)) != 0;
    if (!lba48_supported) return sectors28;

    u64 sectors48 = ((u64)identify_buf[103] << 48) | ((u64)identify_buf[102] << 32)
                   | ((u64)identify_buf[101] << 16) | (u64)identify_buf[100];
    if (sectors48 == 0) return sectors28; /* контроллер не заполнил -- откат к 28-битному */
    if (sectors48 > 0xFFFFFFFFu) return 0xFFFFFFFFu; /* storage_dev_t.sectors -- u32 */
    return (u32)sectors48;
}

void storage_identify_get_model(const u16 *identify_buf, char *out) {
    for (int i = 0; i < 20; i++) {
        u16 w = identify_buf[27 + i];
        out[i * 2]     = (char)(w >> 8);
        out[i * 2 + 1] = (char)(w & 0xFF);
    }
    out[40] = '\0';
    trim(out);
}
