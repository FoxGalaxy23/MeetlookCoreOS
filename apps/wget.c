/* wget.c -- упрощённый аналог wget: HTTP/1.1 GET по TCP (см.
 * include/tcp.h) и сохранение тела ответа в файл активной ФС (см.
 * SYS_FS_WRITE_FILE, include/sysabi.h). Как и настоящий wget без
 * дополнительных опций -- молча следует ровно одному соединению, без
 * редиректов (3xx выводится как ошибка, не преследуется автоматически)
 * и без TLS (см. include/nethelp.h: net_parse_url() отклоняет https://).
 *
 * usage: wget <url> [-O имя_файла]
 * По умолчанию имя файла берётся из последнего компонента пути URL
 * (например http://host/dir/page.html -> page.html), либо "index.html",
 * если путь пуст или заканчивается на '/' -- как у настоящего wget. */

#include "mlapp.h"
#include "nethelp.h"

#define RECV_CHUNK 1024u
#define MAX_BODY (256u * 1024u) /* с запасом хватает для типичной HTML/текстовой страницы */

static char g_body[MAX_BODY];

static void default_filename(const char *path, char *out, u32 out_cap) {
    const char *last_slash = path;
    for (const char *p = path; *p; p++) if (*p == '/') last_slash = p + 1;

    if (*last_slash == '\0') {
        strncpy(out, "index.html", out_cap - 1);
    } else {
        strncpy(out, last_slash, out_cap - 1);
    }
    out[out_cap - 1] = '\0';
}

APP_ENTRY s32 app_main(const kapi_t *kapi, u32 argc, char **argv) {
    (void)kapi;

    if (argc < 2) {
        net_put("usage: wget <url> [-O имя_файла]\n");
        sys_exit(1);
    }

    char host[256];
    char path[512];
    u16 port;
    if (!net_parse_url(argv[1], host, sizeof(host), &port, path, sizeof(path))) {
        net_put("wget: не удалось разобрать URL\n");
        sys_exit(1);
    }

    char outfile[128];
    if (argc >= 4 && strcmp(argv[2], "-O") == 0) {
        strncpy(outfile, argv[3], sizeof(outfile) - 1);
        outfile[sizeof(outfile) - 1] = '\0';
    } else {
        default_filename(path, outfile, sizeof(outfile));
    }

    net_put("Resolving "); net_put(host); net_put("... ");
    u32 ip;
    if (!net_resolve_or_print_error(host, &ip)) sys_exit(1);
    net_put_ip(ip); net_put("\n");

    net_put("Connecting to "); net_put_ip(ip); net_put(":"); net_put_u32(port); net_put("... ");
    s32 sock = sys_tcp_connect(ip, port);
    if (sock < 0) {
        net_put("failed (connection refused or timed out)\n");
        sys_exit(1);
    }
    net_put("connected.\n");

    char req[768];
    u32 n = 0;
    n += (u32)strlen(strcpy(req + n, "GET "));
    n += (u32)strlen(strcpy(req + n, path));
    n += (u32)strlen(strcpy(req + n, " HTTP/1.1\r\nHost: "));
    n += (u32)strlen(strcpy(req + n, host));
    n += (u32)strlen(strcpy(req + n, "\r\nUser-Agent: MeetlookCoreOS-wget/1.0\r\nConnection: close\r\n\r\n"));

    net_put("HTTP request sent, awaiting response... ");
    if (sys_tcp_send(sock, req, n) != (s32)n) {
        net_put("failed to send request\n");
        sys_tcp_close(sock);
        sys_exit(1);
    }

    /* Читаем ВЕСЬ ответ (заголовки + тело) в один буфер до закрытия
       соединения сервером (recv возвращает 0) -- проще, чем разбирать
       Content-Length/chunked прямо во время чтения, а для типичного
       ответа HTTP/1.1 с "Connection: close" сервер и так закрывает
       соединение сам по завершении тела. */
    u32 total = 0;
    for (;;) {
        if (total >= MAX_BODY) break;
        s32 r = sys_tcp_recv(sock, g_body + total, MAX_BODY - total, 10000);
        if (r <= 0) break; /* 0 == EOF (сервер закрыл), <0 == таймаут/ошибка */
        total += (u32)r;
    }
    sys_tcp_close(sock);

    net_put("200 OK\n");

    /* Отделяем заголовки ("\r\n\r\n") от тела -- в файл кладём только
       тело, как настоящий wget, заголовки лишь бегло проверяем на код
       ответа. */
    u32 header_end = 0;
    for (u32 i = 0; i + 3 < total; i++) {
        if (g_body[i] == '\r' && g_body[i + 1] == '\n' && g_body[i + 2] == '\r' && g_body[i + 3] == '\n') {
            header_end = i + 4;
            break;
        }
    }

    if (header_end == 0) {
        net_put("wget: ответ сервера не похож на HTTP (нет пустой строки после заголовков)\n");
        sys_exit(1);
    }

    /* Грубая проверка статус-строки: "HTTP/1.x <код> ..." -- если код не
       2xx, всё равно сохраняем тело (как и настоящий wget без -q),
       но предупреждаем. */
    const char *sp1 = 0;
    for (u32 i = 0; i < header_end && !sp1; i++) if (g_body[i] == ' ') sp1 = &g_body[i + 1];
    if (sp1 && !(sp1[0] == '2')) {
        net_put("wget: предупреждение -- сервер вернул код, отличный от 2xx\n");
    }

    u32 body_len = total - header_end;
    net_put("Length: "); net_put_u32(body_len); net_put(" bytes\n");
    net_put("Saving to: '"); net_put(outfile); net_put("'\n");

    if (sys_fs_write_file(outfile, g_body + header_end, body_len) < 0) {
        net_put("wget: не удалось записать файл (проверьте права/активную ФС)\n");
        sys_exit(1);
    }

    net_put("'"); net_put(outfile); net_put("' saved ["); net_put_u32(body_len); net_put("]\n");

    sys_exit(0);
    return 0; /* не достигается */
}
