#include "usb.h"
#include "usb_uhci.h"
#include "usb_hid.h"
#include "usb_msd.h"
#include "string.h"
#include "kprintf.h"

/* usb.c -- см. общий комментарий в usb.h. Здесь -- вся логика,
   независимая от конкретного контроллера: сброс/включение портов,
   назначение адресов, чтение дескрипторов устройства/конфигурации и
   разбор первого интерфейса, передача найденного устройства нужному
   драйверу класса (HID или Mass Storage). */

static usb_device_t g_devices[USB_MAX_DEVICES];
static u32 g_device_count = 0;

u32 usb_device_count(void) { return g_device_count; }

usb_device_t *usb_get_device(u32 index) {
    if (index >= g_device_count) return NULL;
    return &g_devices[index];
}

bool_t usb_control_transfer(usb_device_t *dev, u8 bmRequestType, u8 bRequest,
                             u16 wValue, u16 wIndex, void *buf, u16 wLength) {
    bool_t low_speed = (dev->speed == USB_SPEED_LOW);
    bool_t is_in = (bmRequestType & 0x80) != 0;
    u16 mps0 = dev->ep[0].max_packet ? dev->ep[0].max_packet : 8;

    /* Control-передачи используются только во время перечисления и
       настройки устройств (не в горячих циклах опроса), поэтому можно
       позволить себе щедрый таймаут на пакет -- в т.ч. с запасом на
       медленные флеш-накопители, которые могут не сразу отвечать
       сразу после подключения/сброса. */
    const u32 timeout_ms = 1000;

    /* ---- SETUP-стадия (всегда DATA0, всегда OUT) ---- */
    usb_setup_packet_t setup;
    setup.bmRequestType = bmRequestType;
    setup.bRequest = bRequest;
    setup.wValue = wValue;
    setup.wIndex = wIndex;
    setup.wLength = wLength;

    u8 toggle = 0;
    if (uhci_transfer_packet(dev->address, 0, USB_PID_SETUP, low_speed,
                              &setup, sizeof(setup), &toggle, timeout_ms) < 0) {
        return FALSE;
    }

    /* ---- Data-стадия (опциональная, начинается с DATA1) ---- */
    toggle = 1;
    u8 *p = (u8 *)buf;
    u32 remaining = wLength;
    u32 offset = 0;
    while (remaining > 0) {
        u16 chunk = (remaining > mps0) ? mps0 : (u16)remaining;
        s32 got = uhci_transfer_packet(dev->address, 0, is_in ? USB_PID_IN : USB_PID_OUT,
                                        low_speed, p + offset, chunk, &toggle, timeout_ms);
        if (got < 0) return FALSE;
        offset += (u32)got;
        remaining -= chunk;
        if (is_in && (u32)got < chunk) break; /* короткий пакет -- данные кончились раньше */
    }

    /* ---- Status-стадия: противоположное направление данным, нулевой
       длины, всегда DATA1. Если данных не было вовсе (wLength == 0),
       статус всегда идёт как IN -- таково правило USB для
       control-передач без Data-стадии. ---- */
    bool_t status_is_in = (wLength == 0) ? TRUE : !is_in;
    u8 status_toggle = 1;
    if (uhci_transfer_packet(dev->address, 0, status_is_in ? USB_PID_IN : USB_PID_OUT,
                              low_speed, NULL, 0, &status_toggle, timeout_ms) < 0) {
        return FALSE;
    }

    return TRUE;
}

s32 usb_data_transfer(usb_device_t *dev, usb_endpoint_t *ep, void *buf,
                       u32 len, u32 timeout_ms) {
    bool_t low_speed = (dev->speed == USB_SPEED_LOW);
    u8 pid = ep->is_in ? USB_PID_IN : USB_PID_OUT;
    u16 mps = ep->max_packet ? ep->max_packet : 8;

    u8 *p = (u8 *)buf;
    u32 offset = 0;
    u32 remaining = len;

    while (remaining > 0) {
        u16 chunk = (remaining > mps) ? mps : (u16)remaining;
        s32 got = uhci_transfer_packet(dev->address, ep->address, pid, low_speed,
                                        p + offset, chunk, &ep->toggle, timeout_ms);
        if (got < 0) return (offset > 0) ? (s32)offset : -1;
        offset += (u32)got;
        remaining -= chunk;
        if (ep->is_in && (u32)got < chunk) break; /* короткий пакет -- конец данных */
    }
    return (s32)offset;
}

/* Разбирает тело конфигурационного дескриптора (после первых 9 байт
   самого usb_config_descriptor_t) и заполняет class/subclass/protocol
   и точки ПЕРВОГО встреченного интерфейса -- остальные интерфейсы (если
   есть) игнорируются, см. общий комментарий в usb.h. */
static void usb_parse_first_interface(usb_device_t *dev, u8 *cfgbuf, u16 total_len,
                                       u8 config_desc_len) {
    u8 *ptr = cfgbuf + config_desc_len;
    u8 *end = cfgbuf + total_len;

    bool_t have_if = FALSE;
    u8 first_if_num = 0;
    dev->ep_count = 1; /* ep[0] всегда контрольная точка */

    while (ptr + 2 <= end && ptr[0] > 0) {
        u8 dlen = ptr[0];
        u8 dtype = ptr[1];
        if (ptr + dlen > end) break;

        if (dtype == 0x04 /* INTERFACE */ && dlen >= sizeof(usb_interface_descriptor_t)) {
            usb_interface_descriptor_t *ifd = (usb_interface_descriptor_t *)ptr;
            if (!have_if) {
                have_if = TRUE;
                first_if_num = ifd->bInterfaceNumber;
                dev->if_number = ifd->bInterfaceNumber;
                dev->if_class = ifd->bInterfaceClass;
                dev->if_subclass = ifd->bInterfaceSubClass;
                dev->if_protocol = ifd->bInterfaceProtocol;
            }
        } else if (dtype == 0x05 /* ENDPOINT */ && dlen >= sizeof(usb_endpoint_descriptor_t)) {
            if (have_if && dev->if_number == first_if_num && dev->ep_count < USB_MAX_ENDPOINTS) {
                usb_endpoint_descriptor_t *epd = (usb_endpoint_descriptor_t *)ptr;
                usb_endpoint_t *ep = &dev->ep[dev->ep_count++];
                ep->address = epd->bEndpointAddress & 0x0F;
                ep->is_in = (epd->bEndpointAddress & 0x80) != 0;
                ep->transfer_type = epd->bmAttributes & 0x03;
                ep->max_packet = epd->wMaxPacketSize & 0x07FF;
                ep->interval = epd->bInterval;
                ep->toggle = 0;
            }
        }
        ptr += dlen;
    }
}

/* Пытается перечислить одно устройство на указанном порту (уже
   сброшенном/включённом uhci_port_reset_and_enable()). Возвращает
   TRUE, если устройство успешно адресовано и сконфигурировано. */
static bool_t usb_enumerate_device(u32 port, bool_t low_speed, u32 address) {
    if (g_device_count >= USB_MAX_DEVICES) return FALSE;

    usb_device_t dev;
    memset(&dev, 0, sizeof(dev));
    dev.uhci_port = (u8)port;
    dev.address = 0; /* пока не назначен -- работаем на адресе по умолчанию */
    dev.speed = low_speed ? USB_SPEED_LOW : USB_SPEED_FULL;
    dev.ep[0].address = 0;
    dev.ep[0].is_in = FALSE;
    dev.ep[0].transfer_type = USB_XFER_CONTROL;
    dev.ep[0].max_packet = 8; /* безопасный минимум для самого первого запроса */
    dev.ep_count = 1;

    /* Первые 8 байт Device Descriptor -- ровно чтобы узнать реальный
       bMaxPacketSize0, не рискуя запросить больше, чем контроллер
       умеет передать точке 0 до того, как мы это значение узнали. */
    usb_device_descriptor_t desc;
    memset(&desc, 0, sizeof(desc));
    if (!usb_control_transfer(&dev, 0x80, USB_REQ_GET_DESCRIPTOR,
                               (u16)(USB_DESC_DEVICE << 8), 0, &desc, 8)) {
        return FALSE;
    }
    if (desc.bMaxPacketSize0 != 0) dev.ep[0].max_packet = desc.bMaxPacketSize0;

    /* Назначаем постоянный адрес -- до этого момента ВСЕ операции шли
       по адресу 0 (единственное устройство, которое может сейчас
       находиться в Default State, т.к. мы включаем/адресуем порты по
       одному). */
    if (!usb_control_transfer(&dev, 0x00, USB_REQ_SET_ADDRESS, (u16)address, 0, NULL, 0)) {
        return FALSE;
    }
    dev.address = (u8)address;

    /* Дальше уже полный (18-байтный) Device Descriptor по новому адресу. */
    if (!usb_control_transfer(&dev, 0x80, USB_REQ_GET_DESCRIPTOR,
                               (u16)(USB_DESC_DEVICE << 8), 0, &desc, sizeof(desc))) {
        return FALSE;
    }
    dev.vendor_id = desc.idVendor;
    dev.product_id = desc.idProduct;
    dev.dev_class = desc.bDeviceClass;
    dev.dev_subclass = desc.bDeviceSubClass;
    dev.dev_protocol = desc.bDeviceProtocol;

    /* Сначала только заголовок Config Descriptor (9 байт), чтобы узнать
       wTotalLength -- полный размер (заголовок + все интерфейсы и точки). */
    u8 cfgbuf[256];
    memset(cfgbuf, 0, sizeof(cfgbuf));
    usb_config_descriptor_t *cfg = (usb_config_descriptor_t *)cfgbuf;
    if (!usb_control_transfer(&dev, 0x80, USB_REQ_GET_DESCRIPTOR,
                               (u16)(USB_DESC_CONFIG << 8), 0, cfg, sizeof(*cfg))) {
        return FALSE;
    }
    u16 total_len = cfg->wTotalLength;
    if (total_len > sizeof(cfgbuf)) total_len = sizeof(cfgbuf); /* обрежем -- нам хватит
        и части: первый интерфейс с его точками почти всегда укладывается
        в 256 байт, а более поздние интерфейсы (составные устройства) мы
        всё равно не разбираем, см. usb.h */
    if (total_len < sizeof(*cfg)) return FALSE;

    if (!usb_control_transfer(&dev, 0x80, USB_REQ_GET_DESCRIPTOR,
                               (u16)(USB_DESC_CONFIG << 8), 0, cfgbuf, total_len)) {
        return FALSE;
    }

    usb_parse_first_interface(&dev, cfgbuf, total_len, cfg->bLength);

    if (!usb_control_transfer(&dev, 0x00, USB_REQ_SET_CONFIG,
                               cfg->bConfigurationValue, 0, NULL, 0)) {
        return FALSE;
    }

    dev.present = TRUE;
    g_devices[g_device_count] = dev;
    usb_device_t *stored = &g_devices[g_device_count];
    g_device_count++;

    kprintf("USB: port %u, addr %u, VID:PID %x:%x, if class %x/%x/%x\n",
            port, stored->address, stored->vendor_id, stored->product_id,
            stored->if_class, stored->if_subclass, stored->if_protocol);

    if (stored->if_class == USB_CLASS_HID) {
        usb_hid_probe(stored);
    } else if (stored->if_class == USB_CLASS_MASS_STORAGE) {
        usb_msd_probe(stored);
    }

    return TRUE;
}

void usb_init(void) {
    g_device_count = 0;
    memset(g_devices, 0, sizeof(g_devices));

    if (!uhci_init()) {
        return; /* нет UHCI-контроллера в системе -- не ошибка, просто нет USB */
    }

    u32 next_address = 1;
    for (u32 port = 0; port < uhci_port_count(); port++) {
        if (!uhci_port_is_connected(port)) continue;

        bool_t low_speed = FALSE;
        if (!uhci_port_reset_and_enable(port, &low_speed)) continue;

        if (next_address > 127) break;
        usb_enumerate_device(port, low_speed, next_address);
        next_address++; /* адрес расходуется даже при неудаче -- не страшно,
                            адресное пространство 1..127 нам хватит с большим запасом */
    }

    usb_hid_start_polling(); /* фоновая задача опроса клавиатур/мышей, если найдены */

    if (g_device_count == 0) {
        kprintf("USB: устройств не обнаружено.\n");
    }
}
