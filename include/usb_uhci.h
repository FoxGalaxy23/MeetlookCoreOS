#ifndef USB_UHCI_H
#define USB_UHCI_H

#include "types.h"

/* usb_uhci.h -- драйвер хост-контроллера UHCI (Universal Host
 * Controller Interface, USB 1.1). Единственный поддерживаемый в этом
 * ядре USB-хост-контроллер: это контроллер по умолчанию у стандартной
 * машины QEMU (piix3-usb-uhci), см. общий комментарий в usb.h.
 *
 * Работает синхронно и без прерываний, как и остальные драйверы этого
 * ядра: расписание передач состоит из ОДНОЙ общей очереди (Queue Head),
 * прописанной в каждый из 1024 слотов Frame List, так что контроллер
 * гарантированно посещает её на каждом кадре (每 1 мс). На каждую
 * отдельную передачу мы навешиваем на эту QH ровно один Transfer
 * Descriptor и крутимся (poll), пока контроллер не снимет бит Active --
 * то есть очередь глубиной 1, ровно как у ahci.c (там тоже один слот
 * команд и синхронное ожидание завершения). */

/* PID-коды для uhci_transfer_packet() -- совпадают со значениями,
   которые контроллер ожидает увидеть в поле Token TD. */
#define USB_PID_SETUP 0x2D
#define USB_PID_IN    0x69
#define USB_PID_OUT   0xE1

/* Обнаруживает PCI-контроллер UHCI (class 0x0C, subclass 0x03,
   prog-if 0x00), включает I/O space + bus master, глобально сбрасывает
   контроллер, готовит Frame List и общую Queue Head и запускает
   контроллер (Run/Stop=1). Возвращает FALSE, если контроллер не
   найден -- это не ошибка, просто в системе нет USB. */
bool_t uhci_init(void);

/* Число портов корневого хаба (у стандартного UHCI их всегда 2). */
u32 uhci_port_count(void);

/* Считывает бит Current Connect Status порта, без побочных эффектов. */
bool_t uhci_port_is_connected(u32 port);

/* Стандартная процедура сброса порта (нужна, чтобы устройство перешло
   в Default State и стало отвечать на адрес 0): удерживает Port Reset
   ~50 мс, снимает его, затем включает порт (Port Enable). *out_low_speed
   заполняется по биту Low Speed Device Attached. Возвращает TRUE, если
   порт в итоге включён (Port Enabled = 1). */
bool_t uhci_port_reset_and_enable(u32 port, bool_t *out_low_speed);

/* Передача РОВНО одного пакета (одной транзакции) по адресу устройства
   dev_addr/точке endpoint, с указанным PID (USB_PID_SETUP/IN/OUT).
   low_speed -- устройство подключено на низкой скорости (влияет на
   бит LS в TD). *toggle -- текущий бит DATA0/DATA1 этой точки,
   обновляется (инвертируется) после каждой УСПЕШНОЙ передачи -- вызывающий
   отвечает за то, чтобы держать свой toggle отдельно на каждую точку
   (см. usb_endpoint_t.toggle в usb.h). timeout_ms ограничивает, сколько
   реального времени (по timer_ticks(), не число итераций) контроллеру
   даётся на обработку с учётом автоматических аппаратных повторов при
   NAK (см. комментарий в .c). Возвращает число реально переданных байт
   (для IN, может быть меньше len -- короткий пакет) либо len (для
   OUT/SETUP), либо -1 при ошибке/STALL/таймауте. */
s32 uhci_transfer_packet(u8 dev_addr, u8 endpoint, u8 pid, bool_t low_speed,
                          void *buf, u16 len, u8 *toggle, u32 timeout_ms);

#endif
