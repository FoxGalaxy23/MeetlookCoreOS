/* main.c — MeetlookCoreOS Kernel Shell */

/* =========================================================
 *  BIOS-примитивы (16-bit real mode)
 * ========================================================= */

static void bios_putchar(char c) {
  if (c == '\n') {
    asm volatile("movb $0x0E, %%ah\n\t"
                 "movw $0x0007, %%bx\n\t"
                 "movb $0x0D, %%al\n\t"
                 "int  $0x10\n\t"
                 :
                 :
                 : "ax", "bx");
    asm volatile("movb $0x0E, %%ah\n\t"
                 "movw $0x0007, %%bx\n\t"
                 "movb $0x0A, %%al\n\t"
                 "int  $0x10\n\t"
                 :
                 :
                 : "ax", "bx");
    return;
  }
  asm volatile("movb $0x0E, %%ah\n\t"
               "movw $0x0007, %%bx\n\t"
               "int  $0x10\n\t"
               :
               : "a"(c)
               : "bx");
}

static void print(const char *s) {
  while (*s)
    bios_putchar(*s++);
}

/* Читает символ с клавиатуры, возвращает ASCII */
static char bios_getchar(void) {
  char c;
  asm volatile("movb $0x00, %%ah\n\t"
               "int  $0x16\n\t"
               : "=a"(c)
               :
               :);
  return c;
}

/* =========================================================
 *  Очистка экрана (INT 10h, AH=06h)
 * ========================================================= */
static void clear_screen(void) {
  asm volatile("movb $0x06, %%ah\n\t"   /* Scroll up */
               "movb $0x00, %%al\n\t"   /* 0 = clear entire window */
               "movb $0x07, %%bh\n\t"   /* attr: white on black */
               "movw $0x0000, %%cx\n\t" /* top-left row=0,col=0 */
               "movw $0x184F, %%dx\n\t" /* bottom-right row=24,col=79 */
               "int  $0x10\n\t"
               :
               :
               : "ax", "bx", "cx", "dx");
  /* Переместить курсор в (0,0) */
  asm volatile("movb $0x02, %%ah\n\t"
               "movb $0x00, %%bh\n\t"
               "movw $0x0000, %%dx\n\t"
               "int  $0x10\n\t"
               :
               :
               : "ax", "bx", "dx");
}

/* =========================================================
 *  BIOS RTC: читаем время/дату
 * ========================================================= */
typedef struct {
  unsigned char h, m, s;
} RtcTime;
typedef struct {
  unsigned char y, mo, d;
} RtcDate;

/* BCD -> десятичное */
static unsigned char bcd2dec(unsigned char v) {
  return (v >> 4) * 10 + (v & 0x0F);
}

static RtcTime get_time(void) {
  RtcTime t;
  unsigned char h, m, s;
  asm volatile("movb $0x02, %%ah\n\t"
               "int  $0x1A\n\t"
               "movb %%ch, %0\n\t"
               "movb %%cl, %1\n\t"
               "movb %%dh, %2\n\t"
               : "=m"(h), "=m"(m), "=m"(s)
               :
               : "ax", "cx", "dx");
  t.h = bcd2dec(h);
  t.m = bcd2dec(m);
  t.s = bcd2dec(s);
  return t;
}

static RtcDate get_date(void) {
  RtcDate d;
  unsigned char y, mo, day;
  asm volatile("movb $0x04, %%ah\n\t"
               "int  $0x1A\n\t"
               "movb %%cl, %0\n\t"
               "movb %%dh, %1\n\t"
               "movb %%dl, %2\n\t"
               : "=m"(y), "=m"(mo), "=m"(day)
               :
               : "ax", "cx", "dx");
  d.y = bcd2dec(y);
  d.mo = bcd2dec(mo);
  d.d = bcd2dec(day);
  return d;
}

/* =========================================================
 *  Вспомогательные функции
 * ========================================================= */

/* Вывод двузначного числа с ведущим нулём */
static void print_d2(unsigned char v) {
  bios_putchar('0' + v / 10);
  bios_putchar('0' + v % 10);
}

/* Сравнение строк */
static int streq(const char *a, const char *b) {
  while (*a && *b && *a == *b) {
    a++;
    b++;
  }
  return *a == *b;
}

/* Начинается ли a с префикса b? */
static int startswith(const char *a, const char *b) {
  while (*b) {
    if (*a++ != *b++)
      return 0;
  }
  return 1;
}

/* Длина строки */
static int slen(const char *s) {
  int n = 0;
  while (s[n])
    n++;
  return n;
}

/* =========================================================
 *  Команды шелла
 * ========================================================= */

static void cmd_time(void) {
  RtcTime t = get_time();
  print("Time: ");
  print_d2(t.h);
  bios_putchar(':');
  print_d2(t.m);
  bios_putchar(':');
  print_d2(t.s);
  bios_putchar('\n');
}

static void cmd_date(void) {
  RtcDate d = get_date();
  print("Date: 20");
  print_d2(d.y);
  bios_putchar('-');
  print_d2(d.mo);
  bios_putchar('-');
  print_d2(d.d);
  bios_putchar('\n');
}

static void cmd_echo(const char *arg) {
  print(arg);
  bios_putchar('\n');
}

static void cmd_help(void) {
  print("Commands:\r\n");
  print("  help     - show this help\r\n");
  print("  clear    - clear screen\r\n");
  print("  time     - show current time\r\n");
  print("  date     - show current date\r\n");
  print("  echo ... - print text\r\n");
  print("  reboot   - reboot system\r\n");
  print("  shutdown - power off\r\n");
}

static void cmd_reboot(void) {
  print("Rebooting...\n");
  asm volatile("int $0x19"); /* BIOS bootstrap */
}

static void cmd_shutdown(void) {
  print("Shutting down... (APM)\n");
  /* APM: Set Power State -> Off */
  asm volatile("movw $0x5301, %%ax\n\t" /* APM connect real-mode */
               "movw $0x0000, %%bx\n\t"
               "int  $0x15\n\t"
               "movw $0x530E, %%ax\n\t" /* APM driver version */
               "movw $0x0000, %%bx\n\t"
               "movw $0x0102, %%cx\n\t"
               "int  $0x15\n\t"
               "movw $0x5308, %%ax\n\t" /* Enable APM */
               "movw $0x0001, %%bx\n\t"
               "movw $0x0001, %%cx\n\t"
               "int  $0x15\n\t"
               "movw $0x5307, %%ax\n\t" /* Set Power State */
               "movw $0x0001, %%bx\n\t" /* All devices */
               "movw $0x0003, %%cx\n\t" /* Power off */
               "int  $0x15\n\t"
               :
               :
               : "ax", "bx", "cx");
  /* Если APM не сработал — зависаем */
  print("(Halt)\n");
  asm volatile("cli\n\t.Lhalt: hlt\n\tjmp .Lhalt\n\t");
}

/* =========================================================
 *  Главный шелл
 * ========================================================= */
#define INPUT_MAX 80

static char input_buf[INPUT_MAX + 1];

static void read_line(void) {
  int pos = 0;
  while (1) {
    char c = bios_getchar();
    if (c == '\r' || c == '\n') {
      bios_putchar('\n');
      break;
    } else if (c == '\b' || c == 0x7F) {
      /* Backspace */
      if (pos > 0) {
        pos--;
        bios_putchar('\b');
        bios_putchar(' ');
        bios_putchar('\b');
      }
    } else if (c >= 0x20 && pos < INPUT_MAX) {
      input_buf[pos++] = c;
      bios_putchar(c); /* echo */
    }
  }
  input_buf[pos] = '\0';
}

static void banner(void) {
  print("\r\n");
  print("     Welcome to MeetlookCoreOS 1.0.0!\r\n");
  print("     Type 'help' for available commands.\r\n");
  print("\r\n");
}

void kmain(void) {
  clear_screen();
  banner();

  while (1) {
    print("shell> ");
    read_line();

    const char *cmd = input_buf;

    if (cmd[0] == '\0') {
      /* пустая строка */
      continue;
    } else if (streq(cmd, "clear")) {
      clear_screen();
    } else if (streq(cmd, "time")) {
      cmd_time();
    } else if (streq(cmd, "date")) {
      cmd_date();
    } else if (streq(cmd, "help")) {
      cmd_help();
    } else if (streq(cmd, "reboot")) {
      cmd_reboot();
    } else if (streq(cmd, "shutdown")) {
      cmd_shutdown();
    } else if (startswith(cmd, "echo ")) {
      cmd_echo(cmd + 5);
    } else if (streq(cmd, "echo")) {
      bios_putchar('\n');
    } else {
      print("Unknown command: '");
      print(cmd);
      print("'. Type 'help'.\n");
    }
  }
}