#include "keyboard.h"
#include "io.h"
#include "usb_hid.h"

#define KBD_DATA_PORT   0x60
#define KBD_STATUS_PORT 0x64

static bool_t shift_pressed = FALSE;
static bool_t caps_lock = FALSE;

/* Таблица скан-кодов набор 1 -> ASCII (без шифта) */
static const char scancode_ascii[128] = {
    0,  27, '1','2','3','4','5','6','7','8','9','0','-','=','\b',
    '\t','q','w','e','r','t','y','u','i','o','p','[',']','\n',
    0,'a','s','d','f','g','h','j','k','l',';','\'','`',
    0,'\\','z','x','c','v','b','n','m',',','.','/', 0,
    '*', 0, ' ', 0,
};

static const char scancode_ascii_shift[128] = {
    0,  27, '!','@','#','$','%','^','&','*','(',')','_','+','\b',
    '\t','Q','W','E','R','T','Y','U','I','O','P','{','}','\n',
    0,'A','S','D','F','G','H','J','K','L',':','"','~',
    0,'|','Z','X','C','V','B','N','M','<','>','?', 0,
    '*', 0, ' ', 0,
};

#define SC_LSHIFT 0x2A
#define SC_RSHIFT 0x36
#define SC_LSHIFT_REL (0x2A | 0x80)
#define SC_RSHIFT_REL (0x36 | 0x80)
#define SC_CAPSLOCK 0x3A

void keyboard_init(void) {
    shift_pressed = FALSE;
    caps_lock = FALSE;
}

bool_t keyboard_haskey(void) {
    /* USB-клавиатура (если есть) опрашивается отдельной фоновой задачей
       (см. usb_hid.c) и лишь складывает уже готовые символы в кольцевой
       буфер -- проверка здесь дешёвая (сравнение двух индексов), поэтому
       не мешает быстрому пути с PS/2. */
    return (inb(KBD_STATUS_PORT) & 1) != 0 || usb_hid_kbd_haskey();
}

static char apply_case(char c) {
    if (caps_lock && c >= 'a' && c <= 'z') return c - 32;
    if (caps_lock && c >= 'A' && c <= 'Z') return c + 32;
    return c;
}

char keyboard_getchar(void) {
    for (;;) {
        /* USB-клавиатура уже отдаёт готовый ASCII-символ (декодирование
           usage-кодов сделано в usb_hid.c) -- никакой обработки
           скан-кодов/shift/caps-lock для неё здесь не нужно. */
        if (usb_hid_kbd_haskey()) {
            char uc = usb_hid_kbd_getchar();
            if (uc != 0) return uc;
            continue;
        }

        if (!((inb(KBD_STATUS_PORT) & 1) != 0)) continue;
        u8 sc = inb(KBD_DATA_PORT);

        if (sc == SC_LSHIFT || sc == SC_RSHIFT) { shift_pressed = TRUE; continue; }
        if (sc == SC_LSHIFT_REL || sc == SC_RSHIFT_REL) { shift_pressed = FALSE; continue; }
        if (sc == SC_CAPSLOCK) { caps_lock = !caps_lock; continue; }

        if (sc & 0x80) continue; /* отпускание прочей клавиши - игнор */
        if (sc >= 128) continue;

        char c = shift_pressed ? scancode_ascii_shift[sc] : scancode_ascii[sc];
        if (c == 0) continue;

        if (!shift_pressed) c = apply_case(c);
        return c;
    }
}
