#include "usb_hid.h"
#include "string.h"
#include "kprintf.h"
#include "task.h"

/* ---- HID class-specific требования (USB HID 1.11) ---- */
#define HID_SUBCLASS_BOOT 0x01
#define HID_PROTOCOL_KEYBOARD 0x01
#define HID_PROTOCOL_MOUSE    0x02

#define HID_REQ_SET_PROTOCOL 0x0B
#define HID_REQ_SET_IDLE     0x0A

#define USB_MAX_HID_KBD   2
#define USB_MAX_HID_MOUSE 2

typedef struct {
    bool_t          active;
    usb_device_t   *dev;
    usb_endpoint_t *ep;      /* interrupt IN */
    u8              prev_keys[6];
} hid_kbd_ctx_t;

typedef struct {
    bool_t          active;
    usb_device_t   *dev;
    usb_endpoint_t *ep;      /* interrupt IN */
    u8              buttons;
    s32             x, y;
} hid_mouse_ctx_t;

static hid_kbd_ctx_t   g_kbds[USB_MAX_HID_KBD];
static u32             g_kbd_count = 0;
static hid_mouse_ctx_t g_mice[USB_MAX_HID_MOUSE];
static u32             g_mouse_count = 0;

/* ---- Кольцевой буфер декодированных символов клавиатуры ----
   Один производитель (фоновая задача опроса usb_hid_poll_task), один
   потребитель (keyboard_getchar()/keyboard_haskey() в keyboard.c) --
   этого достаточно для корректности без явной блокировки на x86. */
#define KBD_RINGBUF_SIZE 32
static volatile char kbd_ring[KBD_RINGBUF_SIZE];
static volatile u32  kbd_ring_head = 0; /* пишет только фоновая задача */
static volatile u32  kbd_ring_tail = 0; /* читает только keyboard.c */

static void kbd_ring_push(char c) {
    u32 next = (kbd_ring_head + 1) % KBD_RINGBUF_SIZE;
    if (next == kbd_ring_tail) return; /* буфер полон -- символ теряется,
        как и было бы, если бы пользователь печатал быстрее, чем шелл
        читает; не критично для интерактивного ввода */
    kbd_ring[kbd_ring_head] = c;
    kbd_ring_head = next;
}

bool_t usb_hid_kbd_haskey(void) {
    return kbd_ring_tail != kbd_ring_head;
}

char usb_hid_kbd_getchar(void) {
    if (kbd_ring_tail == kbd_ring_head) return 0;
    char c = kbd_ring[kbd_ring_tail];
    kbd_ring_tail = (kbd_ring_tail + 1) % KBD_RINGBUF_SIZE;
    return c;
}

/* ---- Таблица usage-кодов Boot Keyboard -> ASCII ----
   Тот же набор клавиш, что и у PS/2-драйвера (drivers/keyboard.c) --
   буквы, цифры, основные знаки препинания, Enter/Backspace/Tab/Space --
   ради единообразия поведения независимо от того, какая клавиатура
   подключена. Автоповтор при удержании клавиши, в отличие от PS/2 (где
   его порождает само устройство), не реализован -- HID boot-репорт по
   USB не содержит событий "повтор", а таймер повтора отдельно от этого
   не заведён; для интерактивного шелла разовое нажатие на символ
   достаточно. Caps Lock также не отслеживается (это состояние
   индикатора, а не бит модификатора) -- поддерживается только Shift. */
static const char hid_usage_ascii[0x39] = {
    /* 0x00-0x03 */ 0, 0, 0, 0,
    /* 0x04-0x1D: a-z */
    'a','b','c','d','e','f','g','h','i','j','k','l','m',
    'n','o','p','q','r','s','t','u','v','w','x','y','z',
    /* 0x1E-0x27: 1-9,0 */
    '1','2','3','4','5','6','7','8','9','0',
    /* 0x28 Enter, 0x29 Esc, 0x2A Backspace, 0x2B Tab, 0x2C Space */
    '\n', 27, '\b', '\t', ' ',
    /* 0x2D - 0x38 */
    '-','=','[',']','\\', 0, ';','\'','`',',','.','/',
};

static const char hid_usage_ascii_shift[0x39] = {
    0, 0, 0, 0,
    'A','B','C','D','E','F','G','H','I','J','K','L','M',
    'N','O','P','Q','R','S','T','U','V','W','X','Y','Z',
    '!','@','#','$','%','^','&','*','(',')',
    '\n', 27, '\b', '\t', ' ',
    '_','+','{','}','|', 0, ':','"','~','<','>','?',
};

#define HID_MOD_LSHIFT 0x02
#define HID_MOD_RSHIFT 0x20

void usb_hid_probe(usb_device_t *dev) {
    if (dev->if_subclass != HID_SUBCLASS_BOOT) {
        return; /* обычный (report-only) HID без boot-протокола -- не поддерживаем */
    }
    if (dev->if_protocol != HID_PROTOCOL_KEYBOARD && dev->if_protocol != HID_PROTOCOL_MOUSE) {
        return;
    }

    usb_endpoint_t *ep = NULL;
    for (u32 i = 1; i < dev->ep_count; i++) {
        if (dev->ep[i].is_in && dev->ep[i].transfer_type == USB_XFER_INTERRUPT) {
            ep = &dev->ep[i];
            break;
        }
    }
    if (!ep) return; /* нет interrupt IN точки -- странное устройство, пропускаем */

    /* Переключаем в Boot Protocol (некоторые устройства по умолчанию
       поднимаются в Report Protocol) и просим слать отчёт при каждом
       изменении (idle rate 0). Обе команды необязательны для работы --
       если устройство ответит STALL'ом (бывает у совсем простых
       контроллеров, которые не поддерживают Report Protocol и потому
       считают SET_PROTOCOL излишним), просто продолжаем: оно и так уже
       в нужном нам режиме по умолчанию. */
    usb_control_transfer(dev, 0x21, HID_REQ_SET_PROTOCOL, 0, dev->if_number, NULL, 0);
    usb_control_transfer(dev, 0x21, HID_REQ_SET_IDLE, 0, dev->if_number, NULL, 0);

    if (dev->if_protocol == HID_PROTOCOL_KEYBOARD && g_kbd_count < USB_MAX_HID_KBD) {
        hid_kbd_ctx_t *ctx = &g_kbds[g_kbd_count++];
        memset(ctx, 0, sizeof(*ctx));
        ctx->dev = dev;
        ctx->ep = ep;
        ctx->active = TRUE;
        kprintf("USB HID: клавиатура готова (порт %u).\n", dev->uhci_port);
    } else if (dev->if_protocol == HID_PROTOCOL_MOUSE && g_mouse_count < USB_MAX_HID_MOUSE) {
        hid_mouse_ctx_t *ctx = &g_mice[g_mouse_count++];
        memset(ctx, 0, sizeof(*ctx));
        ctx->dev = dev;
        ctx->ep = ep;
        ctx->active = TRUE;
        kprintf("USB HID: мышь готова (порт %u).\n", dev->uhci_port);
    }
}

/* Короткий таймаут на попытку чтения interrupt-точки -- если устройству
   нечего сообщить, оно просто NAK'ает, и мы не должны из-за этого
   надолго задерживать фоновую задачу (см. общий комментарий в usb_hid.h). */
#define HID_POLL_TIMEOUT_MS 8

static void hid_poll_keyboard(hid_kbd_ctx_t *ctx) {
    u8 report[8];
    memset(report, 0, sizeof(report));
    s32 n = usb_data_transfer(ctx->dev, ctx->ep, report, sizeof(report), HID_POLL_TIMEOUT_MS);
    if (n < (s32)sizeof(report)) return; /* устройству нечего сообщить в этот раз */

    u8 modifier = report[0];
    bool_t shift = (modifier & (HID_MOD_LSHIFT | HID_MOD_RSHIFT)) != 0;

    for (int i = 2; i < 8; i++) {
        u8 usage = report[i];
        if (usage == 0 || usage == 1) continue; /* 0=нет клавиши, 1=ошибка переполнения */

        bool_t was_pressed = FALSE;
        for (int j = 0; j < 6; j++) {
            if (ctx->prev_keys[j] == usage) { was_pressed = TRUE; break; }
        }
        if (was_pressed) continue; /* клавиша уже была нажата в прошлом отчёте -- не дублируем символ */

        if (usage < 0x39) {
            char c = shift ? hid_usage_ascii_shift[usage] : hid_usage_ascii[usage];
            if (c != 0) kbd_ring_push(c);
        }
    }

    memcpy(ctx->prev_keys, &report[2], 6);
}

static void hid_poll_mouse(hid_mouse_ctx_t *ctx) {
    u8 report[4];
    memset(report, 0, sizeof(report));
    /* Boot-мышь обязана слать минимум 3 байта (кнопки, dX, dY);
       четвёртый (колесо) необязателен -- принимаем оба варианта. */
    s32 n = usb_data_transfer(ctx->dev, ctx->ep, report, sizeof(report), HID_POLL_TIMEOUT_MS);
    if (n < 3) return;

    ctx->buttons = report[0];
    ctx->x += (s8)report[1];
    ctx->y += (s8)report[2];
}

static void usb_hid_poll_task(void *arg) {
    (void)arg;
    for (;;) {
        for (u32 i = 0; i < g_kbd_count; i++) {
            if (g_kbds[i].active) hid_poll_keyboard(&g_kbds[i]);
        }
        for (u32 i = 0; i < g_mouse_count; i++) {
            if (g_mice[i].active) hid_poll_mouse(&g_mice[i]);
        }
        task_sleep(5); /* небольшая пауза между циклами опроса -- нет смысла
            крутиться быстрее, чем реально приходят новые interrupt-отчёты */
    }
}

void usb_hid_start_polling(void) {
    if (g_kbd_count == 0 && g_mouse_count == 0) return;
    task_create("usb-hid-poll", usb_hid_poll_task, 0);
}

bool_t usb_hid_mouse_present(void) {
    return g_mouse_count > 0;
}

void usb_hid_mouse_get_state(u8 *buttons, s32 *x, s32 *y) {
    if (g_mouse_count == 0) {
        if (buttons) *buttons = 0;
        if (x) *x = 0;
        if (y) *y = 0;
        return;
    }
    hid_mouse_ctx_t *ctx = &g_mice[0];
    if (buttons) *buttons = ctx->buttons;
    if (x) *x = ctx->x;
    if (y) *y = ctx->y;
}
