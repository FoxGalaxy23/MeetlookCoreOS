#ifndef PCI_H
#define PCI_H

/* pci.h -- минимальный доступ к конфигурационному пространству PCI
 * (механизм #1, порты 0xCF8/0xCFC) и перебор устройств по шине.
 *
 * Это самостоятельный слой, не привязанный к дискам: любой будущий
 * драйвер PCI-устройства (сетевая карта, звук, ещё один контроллер
 * дисков и т.п.) может использовать pci_find_device()/pci_config_*
 * вместо того чтобы писать свой обход шины заново.
 */

#include "types.h"

typedef struct {
    u8  bus;
    u8  device;
    u8  function;
    u16 vendor_id;
    u16 device_id;
    u8  class_code;
    u8  subclass;
    u8  prog_if;
    u32 bar[6];   /* BAR0..BAR5, как есть, без разбора флагов (I/O vs MEM) */
} pci_device_t;

u32  pci_config_read32(u8 bus, u8 device, u8 function, u8 offset);
void pci_config_write32(u8 bus, u8 device, u8 function, u8 offset, u32 value);
u16  pci_config_read16(u8 bus, u8 device, u8 function, u8 offset);
void pci_config_write16(u8 bus, u8 device, u8 function, u8 offset, u16 value);

/* Ищет первое устройство с заданными class/subclass/prog_if
   (например 0x01/0x06/0x01 -- контроллер AHCI SATA).
   prog_if_mask позволяет игнорировать prog_if (передайте 0), если он не важен.
   Возвращает TRUE и заполняет *out, если устройство найдено. */
bool_t pci_find_device(u8 class_code, u8 subclass, u8 prog_if, pci_device_t *out);

/* Как pci_find_device(), но ищет по конкретной паре vendor/device id, а
   не по классу устройства -- нужно, когда важна не "какая-то карта
   такого-то рода", а РОВНО эта модель (например RTL8139 -- vendor
   0x10EC, device 0x8139 -- см. kernel/netdrv.c). Раньше эта же логика
   перебора шины была продублирована внутри drivers/rtl8139.c (см.
   CONTAINER_ARCHITECTURE.md, Фаза 4, подшаг 4); вынесена сюда, чтобы
   любой будущий драйвер конкретной модели устройства мог переиспользовать
   один и тот же обход шины, а не писать его заново. */
bool_t pci_find_device_by_id(u16 vendor_id, u16 device_id, pci_device_t *out);

#endif
