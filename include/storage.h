#ifndef STORAGE_H
#define STORAGE_H

/* storage.h -- общая абстракция блочных накопителей.
 *
 * Идея: верхние слои (FAT32, shell, kapi для пользовательских бинарников)
 * не должны знать, сидит ли диск на старом IDE-контроллере (PATA, порты
 * 0x1F0/0x170) или на SATA-контроллере через AHCI. Они работают только
 * с storage_dev_t и storage_read_sectors()/storage_write_sectors().
 *
 * Каждая шина (drivers/ide.c для IDE, drivers/ahci.c для SATA) реализует
 * свой storage_ops_t и регистрирует найденные диски через
 * storage_register(). Вся общая часть логики (реестр дисков, разбор
 * данных IDENTIFY) находится здесь, в drivers/storage.c -- поэтому
 * добавить поддержку ещё одного вида контроллера в будущем significantly
 * проще: нужно только реализовать storage_ops_t для него.
 */

#include "types.h"

/* IDE_MAX_DRIVES(4) + AHCI_MAX_PORTS(4) + USB_MSD_MAX_DRIVES(4) */
#define STORAGE_MAX_DRIVES 12

typedef enum {
    STORAGE_BUS_IDE  = 0,  /* PATA/IDE, порты ввода-вывода */
    STORAGE_BUS_SATA = 1,  /* SATA через AHCI, MMIO */
    STORAGE_BUS_USB  = 2,  /* USB Mass Storage, Bulk-Only Transport (см. usb_msd.h) */
} storage_bus_t;

typedef struct storage_dev storage_dev_t;

/* Таблица операций, которую предоставляет драйвер конкретной шины.
   dev->drv_ctx -- приватный контекст этого драйвera для данного диска
   (например указатель на структуру с io_base/is_slave для IDE, или на
   структуру с номером AHCI-порта для SATA); верхние слои его не трогают. */
typedef struct {
    bool_t (*read_sectors)(storage_dev_t *dev, u32 lba, u8 count, void *buf);
    bool_t (*write_sectors)(storage_dev_t *dev, u32 lba, u8 count, const void *buf);
} storage_ops_t;

struct storage_dev {
    bool_t         present;
    storage_bus_t  bus;
    u32            sectors;     /* всего секторов по 512 байт */
    char           model[41];
    char           location[24]; /* для вывода в shell, напр. "IDE0 master", "SATA port 2" */
    const storage_ops_t *ops;
    void          *drv_ctx;
};

/* Вызывает инициализацию всех известных шин (IDE, затем SATA/AHCI) и
   заполняет общий реестр дисков. Вызывается один раз при старте ядра. */
void storage_init(void);

/* Используется драйверами шин (ide.c, ahci.c) во время их *_init(), чтобы
   добавить найденный диск в общий реестр. Возвращает индекс диска в
   реестре или -1, если реестр заполнен (STORAGE_MAX_DRIVES). */
s32 storage_register(storage_bus_t bus, u32 sectors, const char *model,
                      const char *location, const storage_ops_t *ops, void *drv_ctx);

u32            storage_drive_count(void);
storage_dev_t *storage_get_drive(u32 index);
bool_t         storage_read_sectors(u32 drive_index, u32 lba, u8 count, void *buf);
bool_t         storage_write_sectors(u32 drive_index, u32 lba, u8 count, const void *buf);

/* ---- Общие для IDE и SATA хелперы разбора ответа команды IDENTIFY ----
   Формат слова IDENTIFY DEVICE (ATA-8) одинаков независимо от того, как
   именно команда была отправлена диску (через порты IDE или через FIS
   в AHCI), поэтому разбор буфера имеет смысл держать в одном месте. */

/* Извлекает количество секторов диска (28-битное поле, слова 60-61, либо,
   если диск поддерживает 48-битный LBA (бит 10 слова 83), 48-битное поле
   слов 100-103, обрезанное до 32 бит). */
u32 storage_identify_get_sectors(const u16 *identify_buf);

/* Извлекает модель диска (слова 27-46, байты в каждом слове переставлены
   местами) в буфер out размера минимум 41 байт (40 символов + '\0'),
   с обрезкой конечных пробелов. */
void storage_identify_get_model(const u16 *identify_buf, char *out);

#endif
