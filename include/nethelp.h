#ifndef NETHELP_H
#define NETHELP_H

#include "types.h"
#include "string.h"
#include "usyscall.h"

/* nethelp.h -- маленькие вспомогательные функции, общие для apps/ping.c,
 * apps/wget.c и apps/curl.c. Header-only (только static-функции): каждый
 * app.c в Makefile компилируется отдельной командой (build/apps/%.o),
 * без отдельного общего .o -- проще держать это в заголовке, чем менять
 * сборку ради трёх маленьких утилит. */

static inline void net_put(const char *s) {
    sys_write(s, strlen(s));
}

static inline void net_put_u32(u32 v) {
    char buf[16];
    utoa(v, buf, 10);
    net_put(buf);
}

/* Печатает IPv4-адрес (в ХОСТОВОМ порядке байт, как везде в этом ABI --
   см. include/sysabi.h) в точечно-десятичной записи. */
static inline void net_put_ip(u32 ip) {
    net_put_u32((ip >> 24) & 0xFF); net_put(".");
    net_put_u32((ip >> 16) & 0xFF); net_put(".");
    net_put_u32((ip >> 8) & 0xFF);  net_put(".");
    net_put_u32(ip & 0xFF);
}

/* Разбирает строку вида "a.b.c.d" (все части 0-255, ровно 4 части).
   Возвращает TRUE и заполняет *out при успехе. */
static inline bool_t net_parse_ipv4(const char *s, u32 *out) {
    u32 parts[4];
    u32 idx = 0, val = 0;
    bool_t have_digit = FALSE;

    for (const char *p = s; ; p++) {
        char c = *p;
        if (c >= '0' && c <= '9') {
            val = val * 10 + (u32)(c - '0');
            if (val > 255) return FALSE;
            have_digit = TRUE;
        } else if (c == '.' || c == '\0') {
            if (!have_digit || idx >= 4) return FALSE;
            parts[idx++] = val;
            val = 0;
            have_digit = FALSE;
            if (c == '\0') break;
        } else {
            return FALSE;
        }
    }
    if (idx != 4) return FALSE;

    *out = (parts[0] << 24) | (parts[1] << 16) | (parts[2] << 8) | parts[3];
    return TRUE;
}

/* Резолвит host (имя или "a.b.c.d") в IPv4-адрес. Печатает диагностику
   через net_put() при неудаче -- используется одинаково всеми тремя
   утилитами перед tcp_connect()/ping. */
static inline bool_t net_resolve_or_print_error(const char *host, u32 *out_ip) {
    if (net_parse_ipv4(host, out_ip)) return TRUE;

    if (sys_net_resolve(host, out_ip) < 0) {
        net_put("не удалось найти адрес хоста '");
        net_put(host);
        net_put("' (нет DNS-сервера, таймаут или хост не существует)\n");
        return FALSE;
    }
    return TRUE;
}

/* Разбирает URL вида "[http://]host[:port][/path]" на составляющие.
   out_host -- буфер минимум 256 байт, out_path -- минимум path_cap байт
   (сам путь начинается с '/'; если в URL пути нет, кладёт "/"). Схемы,
   отличные от http (например https://), не поддерживаются -- эта ОС
   не реализует TLS, см. общий комментарий в apps/wget.c/apps/curl.c. */
static inline bool_t net_parse_url(const char *url, char *out_host, u32 host_cap,
                                    u16 *out_port, char *out_path, u32 path_cap) {
    const char *p = url;
    if (str_starts_with(p, "http://")) {
        p += 7;
    } else if (str_starts_with(p, "https://")) {
        net_put("схема https:// не поддерживается (в этой ОС нет TLS) -- используйте http://\n");
        return FALSE;
    }

    u32 hi = 0;
    while (*p && *p != '/' && *p != ':' && hi + 1 < host_cap) {
        out_host[hi++] = *p++;
    }
    out_host[hi] = '\0';
    if (hi == 0) return FALSE;

    *out_port = 80;
    if (*p == ':') {
        p++;
        u32 port = 0;
        while (*p >= '0' && *p <= '9') { port = port * 10 + (u32)(*p - '0'); p++; }
        if (port == 0 || port > 65535) return FALSE;
        *out_port = (u16)port;
    }

    if (*p == '\0') {
        strncpy(out_path, "/", path_cap - 1);
        out_path[path_cap - 1] = '\0';
    } else {
        strncpy(out_path, p, path_cap - 1);
        out_path[path_cap - 1] = '\0';
    }
    return TRUE;
}

#endif
