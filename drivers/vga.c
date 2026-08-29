#include "vga.h"
#include "io.h"

#define VGA_WIDTH 80
#define VGA_HEIGHT 25
#define VGA_MEMORY ((u16*)0xB8000)

static u16 *vga_buffer = VGA_MEMORY;
static u8 term_color;
static u32 cursor_row;
static u32 cursor_col;

static inline u16 vga_entry(char c, u8 color) {
    return (u16)c | ((u16)color << 8);
}

void vga_set_color(u8 fg, u8 bg) {
    term_color = fg | (bg << 4);
}

void vga_clear(void) {
    for (u32 y = 0; y < VGA_HEIGHT; y++) {
        for (u32 x = 0; x < VGA_WIDTH; x++) {
            vga_buffer[y * VGA_WIDTH + x] = vga_entry(' ', term_color);
        }
    }
    cursor_row = 0;
    cursor_col = 0;
    vga_update_cursor();
}

void vga_init(void) {
    vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);
    vga_clear();
}

void vga_scroll_if_needed(void) {
    if (cursor_row >= VGA_HEIGHT) {
        for (u32 y = 1; y < VGA_HEIGHT; y++) {
            for (u32 x = 0; x < VGA_WIDTH; x++) {
                vga_buffer[(y - 1) * VGA_WIDTH + x] = vga_buffer[y * VGA_WIDTH + x];
            }
        }
        for (u32 x = 0; x < VGA_WIDTH; x++) {
            vga_buffer[(VGA_HEIGHT - 1) * VGA_WIDTH + x] = vga_entry(' ', term_color);
        }
        cursor_row = VGA_HEIGHT - 1;
    }
}

void vga_putc(char c) {
    if (c == '\n') {
        cursor_col = 0;
        cursor_row++;
    } else if (c == '\r') {
        cursor_col = 0;
    } else if (c == '\t') {
        cursor_col = (cursor_col + 8) & ~7;
    } else {
        vga_buffer[cursor_row * VGA_WIDTH + cursor_col] = vga_entry(c, term_color);
        cursor_col++;
    }

    if (cursor_col >= VGA_WIDTH) {
        cursor_col = 0;
        cursor_row++;
    }
    vga_scroll_if_needed();
    vga_update_cursor();
}

void vga_backspace(void) {
    if (cursor_col == 0) {
        if (cursor_row > 0) {
            cursor_row--;
            cursor_col = VGA_WIDTH - 1;
        }
    } else {
        cursor_col--;
    }
    vga_buffer[cursor_row * VGA_WIDTH + cursor_col] = vga_entry(' ', term_color);
    vga_update_cursor();
}

void vga_puts(const char *s) {
    for (u32 i = 0; s[i] != '\0'; i++) {
        vga_putc(s[i]);
    }
}

void vga_update_cursor(void) {
    u16 pos = cursor_row * VGA_WIDTH + cursor_col;
    outb(0x3D4, 0x0F);
    outb(0x3D5, (u8)(pos & 0xFF));
    outb(0x3D4, 0x0E);
    outb(0x3D5, (u8)((pos >> 8) & 0xFF));
}
