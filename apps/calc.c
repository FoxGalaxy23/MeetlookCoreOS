/* calc.c -- калькулятор выражений, теперь полноценный пользовательский
 * бинарник (rootfs/bin/calc), а не часть образа ядра.
 *
 * Собирается ОТДЕЛЬНО от ядра: своим набором флагов, своим линковочным
 * скриптом (apps/user.ld), по фиксированному адресу загрузки. Не может
 * вызывать функции ядра по имени (kprintf, console_readline и т.д.) --
 * вместо этого получает таблицу kapi_t от загрузчика при старте (см.
 * include/kapi.h, include/mlapp.h, kernel/loader.c).
 *
 * При этом исполняется в том же кольце процессора и с тем же доступом к
 * памяти/портам, что и само ядро -- см. комментарии в kapi.h. Это не
 * "приложение" в песочнице, а обычный код ring 0; используется kapi
 * просто чтобы не дублировать то, что уже реализовано в ядре.
 */

#include "mlapp.h"
#include "string.h"

typedef struct {
    const char *s;
    u32 pos;
    bool_t error;
} parser_t;

static const kapi_t *k;

static void skip_ws(parser_t *p) {
    while (p->s[p->pos] == ' ' || p->s[p->pos] == '\t') p->pos++;
}

static double parse_expr(parser_t *p);

static double parse_number(parser_t *p) {
    skip_ws(p);
    double sign = 1.0;
    if (p->s[p->pos] == '-') { sign = -1.0; p->pos++; }
    else if (p->s[p->pos] == '+') { p->pos++; }

    skip_ws(p);
    if (p->s[p->pos] == '(') {
        p->pos++;
        double v = parse_expr(p);
        skip_ws(p);
        if (p->s[p->pos] == ')') p->pos++;
        else p->error = TRUE;
        return sign * v;
    }

    if (!((p->s[p->pos] >= '0' && p->s[p->pos] <= '9') || p->s[p->pos] == '.')) {
        p->error = TRUE;
        return 0;
    }

    double result = 0;
    while (p->s[p->pos] >= '0' && p->s[p->pos] <= '9') {
        result = result * 10.0 + (p->s[p->pos] - '0');
        p->pos++;
    }
    if (p->s[p->pos] == '.') {
        p->pos++;
        double frac = 0.1;
        while (p->s[p->pos] >= '0' && p->s[p->pos] <= '9') {
            result += (p->s[p->pos] - '0') * frac;
            frac *= 0.1;
            p->pos++;
        }
    }
    return sign * result;
}

static double parse_term(parser_t *p) {
    double left = parse_number(p);
    for (;;) {
        skip_ws(p);
        char op = p->s[p->pos];
        if (op == '*' || op == '/') {
            p->pos++;
            double right = parse_number(p);
            if (op == '*') left = left * right;
            else {
                if (right == 0) { p->error = TRUE; return 0; }
                left = left / right;
            }
        } else break;
    }
    return left;
}

static double parse_expr(parser_t *p) {
    double left = parse_term(p);
    for (;;) {
        skip_ws(p);
        char op = p->s[p->pos];
        if (op == '+' || op == '-') {
            p->pos++;
            double right = parse_term(p);
            if (op == '+') left = left + right;
            else left = left - right;
        } else break;
    }
    return left;
}

static void print_double(double v) {
    if (v < 0) { k->kprintf("-"); v = -v; }
    s32 int_part = (s32)v;
    double frac = v - (double)int_part;
    k->kprintf("%d", int_part);
    k->kprintf(".");
    for (int i = 0; i < 4; i++) {
        frac *= 10.0;
        int digit = (int)frac;
        k->kprintf("%d", digit);
        frac -= digit;
    }
}

static bool_t eval_and_print(const char *expr) {
    parser_t p;
    p.s = expr;
    p.pos = 0;
    p.error = FALSE;
    double result = parse_expr(&p);
    skip_ws(&p);
    if (p.error || expr[p.pos] != '\0') {
        k->kprintf("Expression error\n");
        return FALSE;
    }
    k->kprintf("= ");
    print_double(result);
    k->kprintf("\n");
    return TRUE;
}

/* Собирает argv[1..argc-1] обратно в одну строку через пробел, чтобы
   можно было писать "calc 2 + 3 * (4 - 1)" без кавычек, как раньше. */
static void join_args(u32 argc, char **argv, char *out, u32 out_size) {
    out[0] = '\0';
    u32 len = 0;
    for (u32 i = 1; i < argc && len + 1 < out_size; i++) {
        if (i > 1 && len + 1 < out_size) { out[len++] = ' '; }
        u32 alen = strlen(argv[i]);
        if (len + alen >= out_size) alen = out_size - len - 1;
        memcpy(out + len, argv[i], alen);
        len += alen;
    }
    out[len] = '\0';
}

APP_ENTRY s32 app_main(const kapi_t *kapi, u32 argc, char **argv) {
    k = kapi;

    if (argc >= 2) {
        char expr[128];
        join_args(argc, argv, expr, sizeof(expr));
        eval_and_print(expr);
        return 0;
    }

    k->kprintf("MeetlookCoreOS calculator (user binary, /bin/calc)\n");
    k->kprintf("Enter an expression (e.g. 2 + 3 * (4 - 1)). Type 'exit' to quit.\n");
    char line[128];
    for (;;) {
        k->kprintf("calc> ");
        k->console_readline(line, sizeof(line));
        trim(line);
        if (strcmp(line, "exit") == 0) break;
        if (line[0] == '\0') continue;
        eval_and_print(line);
    }
    return 0;
}
