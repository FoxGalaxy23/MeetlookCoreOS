#ifndef IDE_H
#define IDE_H

/* ide.h -- драйвер старого IDE/PATA-контроллера (порты 0x1F0/0x170).
 *
 * Единственная публичная функция -- ide_init(): она сама обнаруживает
 * диски на обоих каналах (primary/secondary, master/slave) и
 * регистрирует каждый найденный диск через storage_register()
 * (см. storage.h). Остальной код ядра (fat32, shell, kapi) не должен
 * ничего знать про IDE напрямую -- он работает через storage_* API.
 */

void ide_init(void);

#endif
