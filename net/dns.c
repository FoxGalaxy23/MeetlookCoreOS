/* dns.c -- см. include/dns.h за общим описанием. */

#include "dns.h"
#include "udp.h"
#include "net.h"
#include "string.h"

#define DNS_PORT 53u

typedef struct __attribute__((packed)) {
    u16 id;
    u16 flags;
    u16 qdcount;
    u16 ancount;
    u16 nscount;
    u16 arcount;
} dns_hdr_t;

bool_t dns_parse_ipv4_literal(const char *host, ip_addr_t *out_ip) {
    u32 parts[4];
    u32 idx = 0;
    u32 val = 0;
    bool_t have_digit = FALSE;

    for (const char *p = host; ; p++) {
        char c = *p;
        if (c >= '0' && c <= '9') {
            val = val * 10 + (u32)(c - '0');
            if (val > 255) return FALSE;
            have_digit = TRUE;
        } else if (c == '.' || c == '\0') {
            if (!have_digit) return FALSE;
            if (idx >= 4) return FALSE;
            parts[idx++] = val;
            val = 0;
            have_digit = FALSE;
            if (c == '\0') break;
        } else {
            return FALSE; /* содержит нецифровой, неточечный символ -- не литерал IPv4 */
        }
    }
    if (idx != 4) return FALSE;

    *out_ip = ip_make((u8)parts[0], (u8)parts[1], (u8)parts[2], (u8)parts[3]);
    return TRUE;
}

/* Кодирует host ("example.com") в формат DNS QNAME (3example3com0) в buf;
   возвращает записанную длину либо 0 при ошибке (слишком длинное имя). */
static u32 encode_qname(const char *host, u8 *buf, u32 buf_size) {
    u32 out = 0;
    const char *label_start = host;

    while (TRUE) {
        const char *p = label_start;
        while (*p != '.' && *p != '\0') p++;
        u32 label_len = (u32)(p - label_start);
        if (label_len == 0 || label_len > 63) return 0;
        if (out + 1 + label_len + 1 > buf_size) return 0; /* +1 конечный нулевой байт */

        buf[out++] = (u8)label_len;
        memcpy(buf + out, label_start, label_len);
        out += label_len;

        if (*p == '\0') break;
        label_start = p + 1;
    }
    buf[out++] = 0;
    return out;
}

/* Пропускает закодированное DNS-имя начиная с offset в packet (packet_len
   -- полная длина пакета, нужна для проверки границ), учитывая указатели
   сжатия имён (RFC 1035 §4.1.4, байт с двумя старшими битами 11).
   Возвращает смещение сразу ПОСЛЕ имени в исходном потоке (для указателя
   -- это позиция сразу после самого 2-байтного указателя, а не то, куда
   он ссылается), либо 0 при ошибке разбора. */
static u32 skip_name(const u8 *packet, u32 packet_len, u32 offset) {
    u32 hops = 0;
    while (TRUE) {
        if (offset >= packet_len) return 0;
        u8 b = packet[offset];
        if ((b & 0xC0) == 0xC0) {
            if (offset + 2 > packet_len) return 0;
            return offset + 2; /* указатель -- всегда ровно 2 байта на этом месте */
        }
        if (b == 0) return offset + 1;
        offset += 1 + b;
        if (++hops > 128) return 0; /* защита от испорченного пакета без нулевого байта */
    }
}

bool_t dns_resolve(const char *host, u32 timeout_ms, ip_addr_t *out_ip) {
    if (dns_parse_ipv4_literal(host, out_ip)) return TRUE;

    net_config_t cfg;
    net_get_config(&cfg);
    if (!cfg.up || cfg.dns == IP_NONE) return FALSE;

    static u16 next_txid = 0x1234;
    u16 txid = next_txid++;
    u16 src_port = (u16)(40000u + (txid % 10000u));

    u8 query[12 + 256 + 4];
    dns_hdr_t hdr;
    hdr.id = net_htons(txid);
    hdr.flags = net_htons(0x0100u); /* RD (recursion desired) */
    hdr.qdcount = net_htons(1);
    hdr.ancount = 0;
    hdr.nscount = 0;
    hdr.arcount = 0;
    memcpy(query, &hdr, sizeof(hdr));

    u32 name_len = encode_qname(host, query + sizeof(hdr), sizeof(query) - sizeof(hdr) - 4);
    if (name_len == 0) return FALSE;

    u32 pos = sizeof(hdr) + name_len;
    query[pos++] = 0x00; query[pos++] = 0x01; /* QTYPE = A */
    query[pos++] = 0x00; query[pos++] = 0x01; /* QCLASS = IN */

    s32 h = udp_listen(src_port);
    if (h < 0) return FALSE;

    bool_t ok = FALSE;
    u32 waited = 0;
    if (!udp_send(cfg.dns, DNS_PORT, src_port, query, pos)) {
        udp_close(h);
        return FALSE;
    }

    while (waited < timeout_ms && !ok) {
        u8 resp[1500];
        ip_addr_t rsrc; u16 rport;
        u32 remaining = timeout_ms - waited;
        s32 n = udp_recv(h, resp, sizeof(resp), &rsrc, &rport, remaining < 200 ? remaining : 200);
        waited += 200;
        if (n < (s32)sizeof(dns_hdr_t)) continue;
        if (rsrc != cfg.dns || rport != DNS_PORT) continue;

        dns_hdr_t rhdr;
        memcpy(&rhdr, resp, sizeof(rhdr));
        if (net_ntohs(rhdr.id) != txid) continue;

        u16 flags = net_ntohs(rhdr.flags);
        if (!(flags & 0x8000u)) continue; /* не ответ (QR bit) */
        u16 rcode = flags & 0x000Fu;
        u16 ancount = net_ntohs(rhdr.ancount);
        u16 qdcount = net_ntohs(rhdr.qdcount);
        if (rcode != 0 || ancount == 0) break; /* NXDOMAIN/SERVFAIL/пусто -- дальше ждать нечего */

        u32 off = sizeof(dns_hdr_t);
        /* Пропускаем секцию вопросов (эхо нашего собственного запроса). */
        for (u16 i = 0; i < qdcount; i++) {
            off = skip_name(resp, (u32)n, off);
            if (off == 0 || off + 4 > (u32)n) { off = 0; break; }
            off += 4; /* QTYPE + QCLASS */
        }
        if (off == 0) break;

        for (u16 i = 0; i < ancount; i++) {
            off = skip_name(resp, (u32)n, off);
            if (off == 0 || off + 10 > (u32)n) break;

            u16 rtype, rclass; u32 ttl; u16 rdlen;
            memcpy(&rtype, resp + off, 2); off += 2;
            memcpy(&rclass, resp + off, 2); off += 2;
            memcpy(&ttl, resp + off, 4); off += 4;
            (void)ttl;
            memcpy(&rdlen, resp + off, 2); off += 2;
            rtype = net_ntohs(rtype); rclass = net_ntohs(rclass); rdlen = net_ntohs(rdlen);

            if (off + rdlen > (u32)n) break;

            if (rtype == 1 && rclass == 1 && rdlen == 4) { /* A / IN */
                u8 a, b, c, d;
                a = resp[off]; b = resp[off + 1]; c = resp[off + 2]; d = resp[off + 3];
                *out_ip = ip_make(a, b, c, d);
                ok = TRUE;
                break;
            }
            off += rdlen;
        }
        break; /* получили осмысленный ответ на наш txid -- дальше не ждём */
    }

    udp_close(h);
    return ok;
}
