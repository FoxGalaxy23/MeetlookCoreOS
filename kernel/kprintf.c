#include "kprintf.h"
#include "vga.h"
#include "string.h"

/* u64-варианты itoa/utoa (lib/string.c даёт только 32-битные) --
   нужны локально здесь для %lx/%llx/%lu/%llu/%ld/%lld ниже. Тот же
   алгоритм, что и utoa()/itoa(), просто на 64-битном значении. */
static void u64toa(u64 value, char *buf, int base) {
    char tmp[64];
    int i = 0;
    if (value == 0) tmp[i++] = '0';
    while (value) {
        int d = (int)(value % (u64)base);
        tmp[i++] = (d < 10) ? (char)('0' + d) : (char)('a' + d - 10);
        value /= (u64)base;
    }
    int j = 0;
    while (i > 0) buf[j++] = tmp[--i];
    buf[j] = '\0';
}

static void s64toa(s64 value, char *buf, int base) {
    if (value < 0 && base == 10) {
        buf[0] = '-';
        u64toa((u64)(-value), buf + 1, base);
    } else {
        u64toa((u64)value, buf, base);
    }
}

/* Дополняет s пробелами/нулями слева до width и печатает через
 * vga_puts() -- см. общий комментарий у ширины/zero_pad в kprintf()
 * ниже за тем, откуда взялся этот баг (byte-в-byte тот же, что уже был
 * найден и исправлен в lib/libc.c::fmt_core(), см. общий комментарий
 * там же и KNOWN_ISSUES.md) и почему теперь исправлен именно так.
 * '-' (для %d отрицательных значений) корректно остаётся ПЕРВЫМ
 * символом при zero_pad -- "-0003", не "000-3". */
static void pad_and_puts(const char *s, u32 width, bool_t zero_pad) {
    u32 len = strlen(s);
    if (len >= width) {
        vga_puts(s);
        return;
    }
    u32 pad_count = width - len;
    char pad = zero_pad ? '0' : ' ';
    if (zero_pad && s[0] == '-') {
        vga_putc('-');
        for (u32 n = pad_count; n > 0; n--) vga_putc(pad);
        vga_puts(s + 1);
    } else {
        for (u32 n = pad_count; n > 0; n--) vga_putc(pad);
        vga_puts(s);
    }
}

void kprintf(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    char buf[64];

    for (u32 i = 0; fmt[i] != '\0'; i++) {
        if (fmt[i] != '%') {
            vga_putc(fmt[i]);
            continue;
        }
        i++;

        /* Длина 'l'/'ll' (напр. %llx, %lu) -- нам не важно различие
           между "long" и "long long", в обоих случаях читаем 64-битный
           аргумент. Без этой проверки 'l' попадал(и) в default-ветку
           ниже, ничего не читая из va_args -- то есть НЕ ТОЛЬКО ломался
           сам этот спецификатор (печатался буквально как "%llx" вместо
           числа), но и портил ВСЕ последующие %-спецификаторы в этом же
           вызове kprintf(), т.к. va_arg переставал соответствовать
           реально переданным аргументам (пропущенный va_arg -- classic
           argument-shift баг). Именно так выглядела нечитаемая ошибка
           "PT_LOAD segment [0x%11x, 0x%1lx)..." из elf_load()
           (kernel/loader.c) вместо настоящих шестнадцатеричных адресов. */

        /* ШИРИНА/нулевое дополнение (%0Nx, %Nd и т.п.) -- РОВНО ТОТ ЖЕ
         * класс бага, что и с 'l' выше, просто для цифр вместо буквы --
         * без разбора здесь цифры так же утекали бы в default,
         * печатались БУКВАЛЬНО и не потребляли бы va_arg, сдвигая ВСЕ
         * последующие спецификаторы. Живьём поймано на
         * apps/mbrtest.c (userspace printf(), lib/libc.c::fmt_core() --
         * тот же самый баг, тот же фикс, см. её общий комментарий за
         * полным разбором с адресами и KNOWN_ISSUES.md) -- здесь,
         * в kprintf(), идентичный код так и не подвергался этому же
         * входу вживую ни разу (никто не звал "%0Nx" в самом ядре), но
         * баг был ровно тем же самым, просто не найденным. Разбирается
         * БЕЗУСЛОВНО, до switch, как и 'l' -- тот же принцип. */
        u32 width = 0;
        bool_t zero_pad = (fmt[i] == '0');
        while (fmt[i] >= '0' && fmt[i] <= '9') {
            width = width * 10 + (u32)(fmt[i] - '0');
            i++;
        }

        bool_t is_long = FALSE;
        if (fmt[i] == 'l') {
            is_long = TRUE;
            i++;
            if (fmt[i] == 'l') i++;
        }

        switch (fmt[i]) {
            case 's': {
                const char *s = va_arg(args, const char*);
                if (!s) s = "(null)";
                vga_puts(s);
                break;
            }
            case 'd': {
                if (is_long) {
                    s64 v = va_arg(args, s64);
                    s64toa(v, buf, 10);
                } else {
                    int v = va_arg(args, int);
                    itoa(v, buf, 10);
                }
                pad_and_puts(buf, width, zero_pad);
                break;
            }
            case 'u': {
                if (is_long) {
                    u64 v = va_arg(args, u64);
                    u64toa(v, buf, 10);
                } else {
                    u32 v = va_arg(args, u32);
                    utoa(v, buf, 10);
                }
                pad_and_puts(buf, width, zero_pad);
                break;
            }
            case 'x': {
                if (is_long) {
                    u64 v = va_arg(args, u64);
                    u64toa(v, buf, 16);
                } else {
                    u32 v = va_arg(args, u32);
                    utoa(v, buf, 16);
                }
                pad_and_puts(buf, width, zero_pad);
                break;
            }
            case 'c': {
                int c = va_arg(args, int);
                vga_putc((char)c);
                break;
            }
            case '%':
                vga_putc('%');
                break;
            default:
                vga_putc('%');
                if (is_long) vga_putc('l');
                vga_putc(fmt[i]);
                break;
        }
    }
    va_end(args);
}
