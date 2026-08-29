#ifndef USB_H
#define USB_H

#include "types.h"

/* usb.h -- ядро USB-подсистемы.
 *
 * Идея та же, что у storage.h: верхние слои (класс-драйверы usb_hid.c
 * и usb_msd.c) не должны знать деталей конкретного хост-контроллера --
 * они работают только с usb_device_t и usb_control_transfer()/
 * usb_data_transfer(). Единственный поддерживаемый сейчас контроллер --
 * UHCI (см. usb_uhci.h), это дефолтный USB-хост стандартной машины
 * QEMU (piix3-usb-uhci), поэтому вложений под несколько типов HC пока
 * нет -- как и AHCI/IDE, добавление ещё одного контроллера в будущем
 * потребует только новой реализации транспортных функций.
 *
 * Модель работы синхронная и однопоточная относительно самого
 * контроллера (как и весь остальной код драйверов в этом ядре):
 * usb_control_transfer()/usb_data_transfer() -- блокирующие вызовы,
 * которые крутятся (poll) внутри uhci_transfer_packet(), пока
 * контроллер не подтвердит завершение пакета либо не истечёт таймаут.
 *
 * Поддерживается только один уровень конфигурации на устройство и
 * только ПЕРВЫЙ интерфейс этой конфигурации (упрощение в духе
 * остального кода -- ровно как ahci.c ограничивается шиной 0, а ide.c
 * -- только двумя классическими каналами). Для клавиатур, мышей и
 * флеш-накопителей (по одному интерфейсу на устройство) этого более
 * чем достаточно; составные USB-устройства с несколькими функциями
 * не поддерживаются.
 */

#define USB_MAX_DEVICES 8
#define USB_MAX_ENDPOINTS 4  /* ep[0] = control, ep[1..3] = до 3 доп. точек */

typedef enum {
    USB_SPEED_LOW  = 0,   /* 1.5 Mbit/s -- как правило клавиатуры/мыши */
    USB_SPEED_FULL = 1,   /* 12 Mbit/s  -- как правило флеш-накопители */
} usb_speed_t;

/* ---- Стандартные дескрипторы USB 2.0 (глава 9.6), как есть с провода ---- */

typedef struct __attribute__((packed)) {
    u8  bLength;
    u8  bDescriptorType;
    u16 bcdUSB;
    u8  bDeviceClass;
    u8  bDeviceSubClass;
    u8  bDeviceProtocol;
    u8  bMaxPacketSize0;
    u16 idVendor;
    u16 idProduct;
    u16 bcdDevice;
    u8  iManufacturer;
    u8  iProduct;
    u8  iSerialNumber;
    u8  bNumConfigurations;
} usb_device_descriptor_t;

typedef struct __attribute__((packed)) {
    u8  bLength;
    u8  bDescriptorType;
    u16 wTotalLength;
    u8  bNumInterfaces;
    u8  bConfigurationValue;
    u8  iConfiguration;
    u8  bmAttributes;
    u8  bMaxPower;
} usb_config_descriptor_t;

typedef struct __attribute__((packed)) {
    u8 bLength;
    u8 bDescriptorType;
    u8 bInterfaceNumber;
    u8 bAlternateSetting;
    u8 bNumEndpoints;
    u8 bInterfaceClass;
    u8 bInterfaceSubClass;
    u8 bInterfaceProtocol;
    u8 iInterface;
} usb_interface_descriptor_t;

typedef struct __attribute__((packed)) {
    u8  bLength;
    u8  bDescriptorType;
    u8  bEndpointAddress; /* бит 7: 1=IN, 0=OUT; биты 0-3: номер точки */
    u8  bmAttributes;     /* биты 0-1: 0=control,1=iso,2=bulk,3=interrupt */
    u16 wMaxPacketSize;
    u8  bInterval;
} usb_endpoint_descriptor_t;

typedef struct __attribute__((packed)) {
    u8  bmRequestType;
    u8  bRequest;
    u16 wValue;
    u16 wIndex;
    u16 wLength;
} usb_setup_packet_t;

#define USB_DESC_DEVICE 0x01
#define USB_DESC_CONFIG 0x02

#define USB_REQ_GET_DESCRIPTOR 0x06
#define USB_REQ_SET_ADDRESS    0x05
#define USB_REQ_SET_CONFIG     0x09

#define USB_CLASS_HID           0x03
#define USB_CLASS_MASS_STORAGE  0x08

/* Тип передачи точки (bmAttributes & 0x03 дескриптора эндпоинта). */
#define USB_XFER_CONTROL     0
#define USB_XFER_ISOCHRONOUS 1
#define USB_XFER_BULK        2
#define USB_XFER_INTERRUPT   3

typedef struct {
    u8     address;        /* номер точки 0-15, без бита направления */
    bool_t is_in;
    u8     transfer_type;  /* USB_XFER_* */
    u16    max_packet;
    u8     interval;
    u8     toggle;         /* текущий бит DATA0/DATA1 для этой точки */
} usb_endpoint_t;

typedef struct usb_device usb_device_t;

struct usb_device {
    bool_t      present;
    u8          uhci_port;   /* номер порта корневого хаба UHCI */
    u8          address;     /* назначенный USB-адрес устройства (1..127) */
    usb_speed_t speed;

    u16 vendor_id, product_id;
    u8  dev_class, dev_subclass, dev_protocol;

    u8  if_number;
    u8  if_class, if_subclass, if_protocol;

    usb_endpoint_t ep[USB_MAX_ENDPOINTS];
    u8             ep_count;
};

/* Обнаруживает хост-контроллер (UHCI), сбрасывает и включает подключённые
   порты корневого хаба, назначает адреса и конфигурирует найденные
   устройства, затем передаёт их драйверам классов (usb_hid.c для HID,
   usb_msd.c для Mass Storage). Вызывается один раз при старте ядра,
   ПОСЛЕ storage_init() -- см. общий комментарий в kernel/kernel.c. */
void usb_init(void);

u32           usb_device_count(void);
usb_device_t *usb_get_device(u32 index);

/* ---- Транспорт (реализация в drivers/usb_uhci.c) ----
   Единственные функции, которые нужны драйверам классов. */

/* Control-передача на точку 0 (SETUP + опциональная Data-стадия +
   Status-стадия), направление данных определяется битом 7
   bmRequestType (0x80 = устройство->хост). */
bool_t usb_control_transfer(usb_device_t *dev, u8 bmRequestType, u8 bRequest,
                             u16 wValue, u16 wIndex, void *buf, u16 wLength);

/* Bulk/interrupt-передача на произвольную (не control) точку из
   dev->ep[]. Дробит len на пакеты по ep->max_packet и опрашивает
   контроллер, пока все пакеты не будут переданы (для OUT) или пока не
   придёт короткий пакет / не наберётся len байт (для IN). timeout_ms --
   сколько ждать КАЖДЫЙ отдельный пакет, прежде чем считать его
   неудавшимся (иначе устройство, которому нечего сказать -- например
   мышь, по которой не двигали -- будет вечно возвращать NAK).
   Возвращает число реально переданных байт, либо -1 при ошибке. */
s32 usb_data_transfer(usb_device_t *dev, usb_endpoint_t *ep, void *buf,
                       u32 len, u32 timeout_ms);

#endif
