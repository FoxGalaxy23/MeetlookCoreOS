/* udp.c -- см. include/udp.h за общим описанием. */

#include "udp.h"
#include "ip.h"
#include "string.h"
#include "task.h"

typedef struct __attribute__((packed)) {
    u16 src_port;
    u16 dst_port;
    u16 length;   /* заголовок + данные, сетевой порядок байт */
    u16 checksum; /* сетевой порядок байт; 0 разрешено (означает "не считалась") */
} udp_hdr_t;

typedef struct {
    bool_t in_use;
    u16 local_port;
    bool_t has_data;
    u32 data_len;
    ip_addr_t src_ip;
    u16 src_port;
    u8 buf[UDP_LISTENER_BUF];
} udp_listener_t;

static udp_listener_t g_listeners[UDP_MAX_LISTENERS];

void udp_init(void) {
    for (u32 i = 0; i < UDP_MAX_LISTENERS; i++) g_listeners[i].in_use = FALSE;
}

bool_t udp_send(ip_addr_t dst_ip, u16 dst_port, u16 src_port, const void *data, u32 len) {
    if (UDP_HDR_LEN + len > 1500u) return FALSE;

    u8 packet[1500];
    if (UDP_HDR_LEN + len > sizeof(packet)) return FALSE;

    udp_hdr_t hdr;
    hdr.src_port = net_htons(src_port);
    hdr.dst_port = net_htons(dst_port);
    hdr.length = net_htons((u16)(UDP_HDR_LEN + len));
    hdr.checksum = 0;

    memcpy(packet, &hdr, UDP_HDR_LEN);
    memcpy(packet + UDP_HDR_LEN, data, len);

    /* Псевдозаголовок для контрольной суммы UDP (RFC 768): src ip, dst ip,
       zero, proto, udp length -- считаем отдельно и "продолжаем" суммой
       над самим UDP-сегментом (net_checksum_partial держит несократимый
       промежуточный аккумулятор именно для такого случая). */
    struct __attribute__((packed)) {
        u32 src;
        u32 dst;
        u8  zero;
        u8  proto;
        u16 udp_len;
    } pseudo;
    pseudo.src = net_htonl(net_my_ip());
    pseudo.dst = net_htonl(dst_ip);
    pseudo.zero = 0;
    pseudo.proto = IP_PROTO_UDP;
    pseudo.udp_len = net_htons((u16)(UDP_HDR_LEN + len));

    u32 acc = net_checksum_partial(0, &pseudo, sizeof(pseudo));
    acc = net_checksum_partial(acc, packet, UDP_HDR_LEN + len);
    u16 csum = net_checksum_finish(acc);
    if (csum == 0) csum = 0xFFFFu; /* 0 зарезервирован под "сумма не считалась" */
    udp_hdr_t *h2 = (udp_hdr_t *)packet;
    h2->checksum = csum;

    return ip_send(dst_ip, IP_PROTO_UDP, packet, UDP_HDR_LEN + len);
}

void udp_input(ip_addr_t src, const u8 *payload, u32 len) {
    if (len < UDP_HDR_LEN) return;

    udp_hdr_t hdr;
    memcpy(&hdr, payload, UDP_HDR_LEN);
    u16 udp_len = net_ntohs(hdr.length);
    if (udp_len < UDP_HDR_LEN || udp_len > len) return;

    u16 dst_port = net_ntohs(hdr.dst_port);
    u32 data_len = udp_len - UDP_HDR_LEN;
    if (data_len > UDP_LISTENER_BUF) return;

    u64 f = net_irq_save();
    for (u32 i = 0; i < UDP_MAX_LISTENERS; i++) {
        if (g_listeners[i].in_use && g_listeners[i].local_port == dst_port && !g_listeners[i].has_data) {
            memcpy(g_listeners[i].buf, payload + UDP_HDR_LEN, data_len);
            g_listeners[i].data_len = data_len;
            g_listeners[i].src_ip = src;
            g_listeners[i].src_port = net_ntohs(hdr.src_port);
            g_listeners[i].has_data = TRUE;
            break;
        }
    }
    net_irq_restore(f);
}

s32 udp_listen(u16 local_port) {
    u64 f = net_irq_save();
    for (u32 i = 0; i < UDP_MAX_LISTENERS; i++) {
        if (!g_listeners[i].in_use) {
            g_listeners[i].in_use = TRUE;
            g_listeners[i].local_port = local_port;
            g_listeners[i].has_data = FALSE;
            net_irq_restore(f);
            return (s32)i;
        }
    }
    net_irq_restore(f);
    return -1;
}

s32 udp_recv(s32 h, void *buf, u32 maxlen, ip_addr_t *out_src, u16 *out_src_port, u32 timeout_ms) {
    if (h < 0 || (u32)h >= UDP_MAX_LISTENERS) return -1;

    u32 waited = 0;
    while (waited <= timeout_ms) {
        if (g_listeners[h].has_data) {
            u32 n = g_listeners[h].data_len;
            if (n > maxlen) n = maxlen;
            memcpy(buf, g_listeners[h].buf, n);
            if (out_src) *out_src = g_listeners[h].src_ip;
            if (out_src_port) *out_src_port = g_listeners[h].src_port;
            g_listeners[h].has_data = FALSE; /* готовы принять следующий пакет */
            return (s32)n;
        }
        task_sleep(10);
        waited += 10;
    }
    return -1;
}

void udp_close(s32 h) {
    if (h < 0 || (u32)h >= UDP_MAX_LISTENERS) return;
    g_listeners[h].in_use = FALSE;
    g_listeners[h].has_data = FALSE;
}
