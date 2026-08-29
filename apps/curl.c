/* curl.c -- упрощённый аналог curl: HTTP/1.1 GET по TCP (см.
 * include/tcp.h), тело ответа печатается в stdout (в отличие от
 * apps/wget.c, который сохраняет его в файл). Как и настоящий curl без
 * опций, по умолчанию тихо не показывает заголовки ответа -- добавьте
 * -i, чтобы увидеть их тоже. Без TLS (см. include/nethelp.h). */

#include "mlapp.h"
#include "nethelp.h"

#define MAX_BODY (256u * 1024u)

static char g_body[MAX_BODY];

APP_ENTRY s32 app_main(const kapi_t *kapi, u32 argc, char **argv) {
    (void)kapi;

    if (argc < 2) {
        net_put("usage: curl [-i] <url>\n");
        sys_exit(1);
    }

    bool_t show_headers = FALSE;
    const char *url = argv[1];
    if (strcmp(argv[1], "-i") == 0 && argc >= 3) {
        show_headers = TRUE;
        url = argv[2];
    }

    char host[256];
    char path[512];
    u16 port;
    if (!net_parse_url(url, host, sizeof(host), &port, path, sizeof(path))) {
        net_put("curl: не удалось разобрать URL\n");
        sys_exit(1);
    }

    u32 ip;
    if (!net_resolve_or_print_error(host, &ip)) sys_exit(1);

    s32 sock = sys_tcp_connect(ip, port);
    if (sock < 0) {
        net_put("curl: (7) Failed to connect to "); net_put(host); net_put(": Connection refused/timeout\n");
        sys_exit(1);
    }

    char req[768];
    u32 n = 0;
    n += (u32)strlen(strcpy(req + n, "GET "));
    n += (u32)strlen(strcpy(req + n, path));
    n += (u32)strlen(strcpy(req + n, " HTTP/1.1\r\nHost: "));
    n += (u32)strlen(strcpy(req + n, host));
    n += (u32)strlen(strcpy(req + n, "\r\nUser-Agent: MeetlookCoreOS-curl/1.0\r\nConnection: close\r\n\r\n"));

    if (sys_tcp_send(sock, req, n) != (s32)n) {
        net_put("curl: (55) Failed sending HTTP request\n");
        sys_tcp_close(sock);
        sys_exit(1);
    }

    u32 total = 0;
    for (;;) {
        if (total >= MAX_BODY) break;
        s32 r = sys_tcp_recv(sock, g_body + total, MAX_BODY - total, 10000);
        if (r <= 0) break;
        total += (u32)r;
    }
    sys_tcp_close(sock);

    u32 header_end = 0;
    for (u32 i = 0; i + 3 < total; i++) {
        if (g_body[i] == '\r' && g_body[i + 1] == '\n' && g_body[i + 2] == '\r' && g_body[i + 3] == '\n') {
            header_end = i + 4;
            break;
        }
    }
    if (header_end == 0) {
        /* Не похоже на HTTP -- выводим как есть, вдруг это curl к
           чему-то совсем простому без корректных заголовков. */
        sys_write(g_body, total);
        sys_exit(total > 0 ? 0 : 1);
    }

    if (show_headers) {
        sys_write(g_body, header_end);
    }
    sys_write(g_body + header_end, total - header_end);

    sys_exit(0);
    return 0; /* не достигается */
}
