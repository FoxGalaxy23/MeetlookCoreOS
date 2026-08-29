#ifndef AHCI_H
#define AHCI_H

/* ahci.h -- драйвер SATA-накопителей через контроллер AHCI.
 *
 * Как и ide.h, экспортирует только одну функцию: ahci_init() находит
 * PCI-контроллер AHCI (class 0x01 / subclass 0x06 / prog-if 0x01),
 * поднимает его, обходит порты, и каждый найденный SATA-диск
 * регистрирует через storage_register() (см. storage.h) -- так же,
 * как это делает ide_init() для IDE. Если контроллер AHCI не найден
 * (например в системе его просто нет), ahci_init() тихо ничего не
 * делает -- это не ошибка.
 */

void ahci_init(void);

#endif
