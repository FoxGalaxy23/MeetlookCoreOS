#include "console.h"
#include "keyboard.h"
#include "vga.h"

u32 console_readline(char *buf, u32 maxlen) {
    u32 len = 0;
    for (;;) {
        char c = keyboard_getchar();
        if (c == '\n') {
            vga_putc('\n');
            buf[len] = '\0';
            return len;
        } else if (c == '\b') {
            if (len > 0) {
                len--;
                vga_backspace();
            }
        } else {
            if (len < maxlen - 1) {
                buf[len++] = c;
                vga_putc(c);
            }
        }
    }
}
