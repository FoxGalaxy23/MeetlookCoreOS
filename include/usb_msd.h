#ifndef USB_MSD_H
#define USB_MSD_H

#include "usb.h"

/* usb_msd.h -- драйвер USB Mass Storage Class, транспорт Bulk-Only
 * Transport (BOT, "protocol 0x50") поверх набора команд SCSI
 * transparent command set (subclass 0x06) -- это ровно то, что
 * реализует подавляющее большинство настоящих флеш-накопителей и
 * виртуальное устройство `usb-storage` в QEMU.
 *
 * Как и ide.c/ahci.c, драйвер не выставляет наружу ничего, кроме
 * функции обнаружения: найденные накопители регистрируются через
 * storage_register() (см. storage.h, новый STORAGE_BUS_USB) и дальше
 * неотличимы для FAT32/shell от дисков на IDE или AHCI -- те же
 * 'drives'/'select'. */

/* Вызывается из usb.c для каждого найденного устройства с
   if_class == USB_CLASS_MASS_STORAGE. Проверяет subclass/protocol,
   находит bulk IN/OUT точки, опрашивает диск (READ CAPACITY, INQUIRY)
   и регистрирует его в общем реестре storage.h. Устройства с
   неподдерживаемым subclass/protocol (не BOT/не SCSI) тихо
   игнорируются. */
void usb_msd_probe(usb_device_t *dev);

#endif
