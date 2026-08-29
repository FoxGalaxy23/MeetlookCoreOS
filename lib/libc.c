/* libc.c -- см. include/libc.h за общим описанием. Линкуется в каждый
 * пользовательский бинарник (см. Makefile, правило build/apps/%.elf) --
 * как lib/string.c и lib/usyscall.c, но, в отличие от lib/string.c, НЕ
 * компилируется в само ядро: и printf-бэкенд (kapi_t/системные вызовы),
 * и куча имеют смысл только внутри пользовательского процесса.
 */

#include "libc.h"

/* ============================================================
 * Консольный ввод-вывод: два бэкенда (см. libc_init() в libc.h)
 * ============================================================ */

static const kapi_t *g_kapi = NULL;

void libc_init(const kapi_t *kapi) {
    g_kapi = kapi;
}

static void console_write(const char *data, u32 len) {
    if (g_kapi) {
        /* Режим 'rungod' (ring 0): пишем посимвольно через vga_putc --
           у kapi_t нет варианта "вывести ровно len байт произвольных
           (возможно, без завершающего нуля) данных" вроде sys_write(),
           только vga_puts() для NUL-terminated строк, что не подходит
           для промежуточных фрагментов форматированного вывода. */
        for (u32 i = 0; i < len; i++) g_kapi->vga_putc(data[i]);
    } else {
        /* Режим 'run' (ring 3, по умолчанию): обычный системный вызов.
           sys_write() сам обрежет len до разумного предела (см.
           kernel/syscall.c) -- для консольного вывода одной команды
           этого более чем достаточно. */
        sys_write(data, len);
    }
}

void putchar(char c) {
    console_write(&c, 1);
}

void puts(const char *s) {
    console_write(s, strlen(s));
    console_write("\n", 1);
}

s32 getchar(void) {
    if (g_kapi) return (s32)g_kapi->keyboard_getchar();
    return sys_getchar();
}

s32 readline(char *buf, u32 maxlen) {
    if (g_kapi) return (s32)g_kapi->console_readline(buf, maxlen);
    return sys_readline(buf, maxlen);
}

/* ============================================================
 * printf-семейство -- один общий форматтер (fmt_core) поверх
 * "приёмника" (sink): либо консоль (console_write), либо буфер
 * (snprintf_sink) -- набор спецификаторов намеренно совпадает с
 * kernel/kprintf.c, см. общий комментарий в libc.h.
 * ============================================================ */

typedef void (*sink_fn)(void *ctx, const char *data, u32 len);

static void stdout_sink(void *ctx, const char *data, u32 len) {
    (void)ctx;
    console_write(data, len);
}

typedef struct {
    char *buf;
    u32   cap;   /* включая место под завершающий '\0' */
    u32   pos;   /* сколько символов УЖЕ БЫЛО БЫ записано (может расти
                    дальше cap-1 -- как и настоящий snprintf, возвращаемое
                    значение позволяет узнать, что вывод обрезан) */
} snprintf_ctx_t;

static void snprintf_sink(void *ctx, const char *data, u32 len) {
    snprintf_ctx_t *s = (snprintf_ctx_t *)ctx;
    for (u32 i = 0; i < len; i++) {
        if (s->pos + 1 < s->cap) s->buf[s->pos] = data[i]; /* +1 -- место под '\0' */
        s->pos++;
    }
}

static void u64toa_(u64 value, char *buf, int base) {
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

static void s64toa_(s64 value, char *buf, int base) {
    if (value < 0 && base == 10) {
        buf[0] = '-';
        u64toa_((u64)(-value), buf + 1, base);
    } else {
        u64toa_((u64)value, buf, base);
    }
}

/* Дополняет s пробелами/нулями слева до width (если s короче) и шлёт
 * в sink -- используется только числовыми спецификаторами (d/u/x)
 * ниже, см. общий комментарий у их разбора width/zero_pad в fmt_core()
 * за тем, откуда взялся этот баг и почему теперь исправлен именно так.
 * '-' (для %d отрицательных значений) корректно остаётся ПЕРВЫМ
 * символом при zero_pad -- "-0003", не "000-3". */
static void pad_and_sink(sink_fn sink, void *ctx, const char *s, u32 width, bool_t zero_pad) {
    u32 len = strlen(s);
    if (len >= width) {
        sink(ctx, s, len);
        return;
    }
    u32 pad_count = width - len;
    char pad = zero_pad ? '0' : ' ';
    if (zero_pad && s[0] == '-') {
        sink(ctx, s, 1);
        for (u32 n = pad_count; n > 0; n--) sink(ctx, &pad, 1);
        sink(ctx, s + 1, len - 1);
    } else {
        for (u32 n = pad_count; n > 0; n--) sink(ctx, &pad, 1);
        sink(ctx, s, len);
    }
}

static void fmt_core(sink_fn sink, void *ctx, const char *fmt, va_list args) {
    char buf[64];

    for (u32 i = 0; fmt[i] != '\0'; i++) {
        if (fmt[i] != '%') {
            sink(ctx, &fmt[i], 1);
            continue;
        }
        i++;

        /* ШИРИНА/нулевое дополнение (%0Nx, %Nd и т.п.) -- ОБЯЗАТЕЛЬНО
         * разбирается здесь, ДО switch, безусловно (какой бы символ ни
         * шёл дальше) -- иначе непрочитанные цифры проваливались бы в
         * default-ветку ниже, печатались БУКВАЛЬНО ("%04x" вместо
         * числа) и, что куда хуже, НЕ потребляли бы va_arg -- тогда
         * следующий спецификатор в этой же строке читал бы ЧУЖОЙ
         * аргумент (классический argument-shift баг). Живьём поймано
         * на apps/mbrtest.c: printf("  %04x  %s\n", off, line) -- "0"
         * и "4" утекали в default, %s забирал `off` вместо `line`
         * (u32 0 -> NULL -> "(null)" на первой строке, а на второй,
         * off=16=0x10, `strlen((char*)0x10)` -- page fault по адресу
         * 0x10, см. KNOWN_ISSUES.md). Поддержан ровно тот минимум
         * синтаксиса, который встречается в этом дереве: необязательный
         * ведущий '0' (флаг zero_pad) + десятичная ширина -- ни
         * precision (`.N`), ни другие флаги (`-`,`+`,` `,`#`) нигде не
         * используются, разбирать их сейчас незачем. */
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
                const char *s = va_arg(args, const char *);
                if (!s) s = "(null)";
                sink(ctx, s, strlen(s));
                break;
            }
            case 'd': {
                if (is_long) s64toa_(va_arg(args, s64), buf, 10);
                else itoa(va_arg(args, int), buf, 10);
                pad_and_sink(sink, ctx, buf, width, zero_pad);
                break;
            }
            case 'u': {
                if (is_long) u64toa_(va_arg(args, u64), buf, 10);
                else utoa(va_arg(args, u32), buf, 10);
                pad_and_sink(sink, ctx, buf, width, zero_pad);
                break;
            }
            case 'x': {
                if (is_long) u64toa_(va_arg(args, u64), buf, 16);
                else utoa(va_arg(args, u32), buf, 16);
                pad_and_sink(sink, ctx, buf, width, zero_pad);
                break;
            }
            case 'c': {
                char c = (char)va_arg(args, int);
                sink(ctx, &c, 1);
                break;
            }
            case '%':
                sink(ctx, "%", 1);
                break;
            case '\0':
                /* одинокий '%' в самом конце строки -- печатаем как есть и
                   останавливаемся, не заходя за пределы fmt */
                sink(ctx, "%", 1);
                return;
            default:
                /* Неизвестный спецификатор -- в этом дереве такого не
                   бывает (см. полный список выше -- d/u/x/s/c/%), это
                   чисто defensive fallback. Ширина/zero_pad, если были,
                   уже съедены циклом выше и здесь не восстанавливаются
                   буквально (посчитанное значение width не хранит
                   исходное число цифр/ведущих нулей) -- не страшно: то,
                   что действительно важно (не проглотить va_arg молча),
                   уже гарантировано разбором width/is_long выше, ЭТА
                   ветка вообще не участвует в потреблении аргументов. */
                sink(ctx, "%", 1);
                if (is_long) sink(ctx, "l", 1);
                sink(ctx, &fmt[i], 1);
                break;
        }
    }
}

s32 vprintf(const char *fmt, va_list args) {
    fmt_core(stdout_sink, NULL, fmt, args);
    return 0; /* точное число выведенных байт никому не нужно для консоли */
}

s32 printf(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    s32 r = vprintf(fmt, args);
    va_end(args);
    return r;
}

s32 vsnprintf(char *buf, u32 size, const char *fmt, va_list args) {
    snprintf_ctx_t ctx = { buf, size, 0 };
    fmt_core(snprintf_sink, &ctx, fmt, args);
    if (size > 0) buf[(ctx.pos < size) ? ctx.pos : (size - 1)] = '\0';
    return (s32)ctx.pos; /* как и настоящий snprintf: сколько было бы
                             написано без обрезки */
}

s32 snprintf(char *buf, u32 size, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    s32 r = vsnprintf(buf, size, fmt, args);
    va_end(args);
    return r;
}

/* ============================================================
 * malloc-семейство -- независимая от ядра куча процесса: простой
 * аллокатор первого подходящего блока (first-fit) поверх статической
 * арены LIBC_HEAP_SIZE байт (часть .bss бинарника, см. общий
 * комментарий в libc.h). Один линейный односвязный список блоков,
 * отсортированный по адресу (гарантируется тем, что split() всегда
 * режет блок на "голову" и "хвост", а не наоборот) -- это позволяет
 * сливать соседние свободные блоки простым проходом по списку, без
 * отдельных prev-указателей.
 * ============================================================ */

typedef struct block_header {
    u32 size;               /* полезный размер (без этого заголовка), выровнен по 8 */
    bool_t used;
    struct block_header *next;
} block_header_t;

#define HEAP_ALIGN 8u
#define HEAP_MIN_SPLIT 32u /* не разбиваем блок, если "хвост" после раздела
                               окажется меньше этого -- иначе куча быстро
                               забивается бесполезными огрызками в пару байт */

static u8 g_heap_arena[LIBC_HEAP_SIZE] __attribute__((aligned(16)));
static block_header_t *g_heap_head = NULL;

static u32 align_up_(u32 v, u32 a) {
    return (v + (a - 1)) & ~(a - 1);
}

static void heap_lazy_init(void) {
    if (g_heap_head) return;
    g_heap_head = (block_header_t *)g_heap_arena;
    g_heap_head->size = (u32)sizeof(g_heap_arena) - (u32)sizeof(block_header_t);
    g_heap_head->used = FALSE;
    g_heap_head->next = NULL;
}

void *malloc(u32 size) {
    if (size == 0) return NULL;
    heap_lazy_init();

    u32 want = align_up_(size, HEAP_ALIGN);

    for (block_header_t *b = g_heap_head; b; b = b->next) {
        if (b->used || b->size < want) continue;

        /* Блок достаточно велик, чтобы после выделения want байт в нём
           остался кусок, которому стоит стать отдельным свободным
           блоком (а не просто "внутренней дырой" внутри занятого) --
           режем его на b (used, ровно want байт) и новый хвост. */
        if (b->size >= want + (u32)sizeof(block_header_t) + HEAP_MIN_SPLIT) {
            block_header_t *tail = (block_header_t *)((u8 *)b + sizeof(block_header_t) + want);
            tail->size = b->size - want - (u32)sizeof(block_header_t);
            tail->used = FALSE;
            tail->next = b->next;

            b->size = want;
            b->next = tail;
        }

        b->used = TRUE;
        return (void *)((u8 *)b + sizeof(block_header_t));
    }

    return NULL; /* арена исчерпана -- см. LIBC_HEAP_SIZE в libc.h */
}

void *calloc(u32 nmemb, u32 size) {
    /* Простая защита от переполнения nmemb*size -- вместо честной
       проверки div/mul overflow (u32 * u32 может кратно переполниться)
       достаточно отсечь заведомо абсурдные размеры: сама арена не
       больше нескольких сотен килобайт, так что запрос свыше неё уже
       гарантированно не поместится. */
    if (nmemb != 0 && size > (u32)sizeof(g_heap_arena) / nmemb) return NULL;
    u32 total = nmemb * size;
    void *p = malloc(total);
    if (p) memset(p, 0, total);
    return p;
}

static void heap_coalesce(void) {
    for (block_header_t *b = g_heap_head; b && b->next; ) {
        u8 *b_end = (u8 *)b + sizeof(block_header_t) + b->size;
        if (!b->used && !b->next->used && b_end == (u8 *)b->next) {
            b->size += (u32)sizeof(block_header_t) + b->next->size;
            b->next = b->next->next;
            /* не двигаемся дальше -- вдруг b теперь граничит и со СЛЕДУЮЩИМ
               (новым b->next) свободным блоком тоже, надо проверить снова */
            continue;
        }
        b = b->next;
    }
}

void free(void *ptr) {
    if (!ptr) return;
    block_header_t *b = (block_header_t *)((u8 *)ptr - sizeof(block_header_t));
    b->used = FALSE;
    heap_coalesce();
}

void *realloc(void *ptr, u32 new_size) {
    if (!ptr) return malloc(new_size);
    if (new_size == 0) { free(ptr); return NULL; }

    block_header_t *b = (block_header_t *)((u8 *)ptr - sizeof(block_header_t));
    if (b->size >= new_size) return ptr; /* уже достаточно места -- не трогаем */

    void *new_ptr = malloc(new_size);
    if (!new_ptr) return NULL; /* ptr остаётся действительным, как и полагается realloc() */
    memcpy(new_ptr, ptr, b->size);
    free(ptr);
    return new_ptr;
}

u32 heap_total_bytes(void) {
    return (u32)sizeof(g_heap_arena);
}

u32 heap_used_bytes(void) {
    heap_lazy_init();
    u32 used = 0;
    for (block_header_t *b = g_heap_head; b; b = b->next) {
        if (b->used) used += b->size + (u32)sizeof(block_header_t);
    }
    return used;
}

u32 heap_free_bytes(void) {
    return heap_total_bytes() - heap_used_bytes();
}
