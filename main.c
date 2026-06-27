/* main.c — логика ядра */

/* Вывод символа через BIOS INT 10h, Teletype (inline ASM — без проблем с соглашениями) */
static void bios_putchar(char c) {
    asm volatile (
        "movb $0x0E, %%ah\n\t"   /* BIOS: Teletype output */
        "movw $0x0007, %%bx\n\t" /* страница 0, белый цвет */
        "int  $0x10\n\t"
        :
        : "a"(c)   /* GCC кладёт c в AL */
        : "bx"
    );
}

/* Данные: строка */
static const char message[] = "Hello, World!\r\n";

/* Печать строки */
static void print(const char *s) {
    int i = 0;
    while (s[i] != '\0') {
        bios_putchar(s[i]);
        i++;
    }
}

/* Точка входа — вызывается из boot.asm */
void kmain(void) {
    print(message);
}