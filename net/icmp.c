/* icmp.c -- см. include/icmp.h за общим описанием. */

#include "icmp.h"
#include "ip.h"
#include "string.h"
#include "task.h"
#include "timer.h"

typedef struct __attribute__((packed)) {
    u8  type;
    u8  code;
    u16 checksum;
    u16 id;
    u16 seq;
    /* дальше -- произвольные данные, у нас не используются */
} icmp_hdr_t;

/* Единственный "слот" ожидаемого ответа -- см. общий комментарий в
   include/icmp.h про то, почему этого достаточно для целей этой ОС
   (одна команда 'ping' в один момент времени на практике). */
static volatile bool_t g_pending = FALSE;
static volatile u16 g_pending_id = 0;
static volatile u16 g_pending_seq = 0;
static volatile bool_t g_reply_seen = FALSE;
static volatile u64 g_reply_tick = 0;

static u64 ms_to_ticks(u32 ms) {
    u32 freq = timer_frequency();
    if (freq == 0) freq = 100;
    u64 ticks = ((u64)ms * (u64)freq) / 1000u;
    if (ticks == 0) ticks = 1;
    return ticks;
}

static u32 ticks_to_ms(u64 ticks) {
    u32 freq = timer_frequency();
    if (freq == 0) freq = 100;
    return (u32)((ticks * 1000u) / freq);
}

void icmp_init(void) {
    g_pending = FALSE;
    g_reply_seen = FALSE;
}

void icmp_input(ip_addr_t src, const u8 *payload, u32 len) {
    (void)src;
    if (len < sizeof(icmp_hdr_t)) return;

    icmp_hdr_t hdr;
    memcpy(&hdr, payload, sizeof(hdr));

    if (hdr.type != ICMP_TYPE_ECHO_REQUEST && hdr.type != ICMP_TYPE_ECHO_REPLY) return;

    if (net_checksum(payload, len) != 0) return; /* битый пакет */

    if (hdr.type == ICMP_TYPE_ECHO_REQUEST) {
        /* Отвечаем на пинг от других узлов -- эта ОС тоже обязана быть
           "пингуемой", иначе странно было бы называть это TCP/IP-стеком. */
        u8 reply[1500];
        if (len > sizeof(reply)) return;
        memcpy(reply, payload, len);
        icmp_hdr_t *rh = (icmp_hdr_t *)reply;
        rh->type = ICMP_TYPE_ECHO_REPLY;
        rh->checksum = 0;
        rh->checksum = net_checksum(reply, len);
        ip_send(src, IP_PROTO_ICMP, reply, len);
        return;
    }

    /* ECHO_REPLY: если это тот ответ, которого ждёт icmp_ping(), фиксируем
       время получения. */
    if (g_pending && net_ntohs(hdr.id) == g_pending_id && net_ntohs(hdr.seq) == g_pending_seq) {
        g_reply_tick = timer_ticks();
        g_reply_seen = TRUE;
    }
}

bool_t icmp_ping(ip_addr_t dst, u16 id, u16 seq, u32 timeout_ms, u32 *out_rtt_ms) {
    u8 pkt[sizeof(icmp_hdr_t) + 32];
    icmp_hdr_t hdr;
    hdr.type = ICMP_TYPE_ECHO_REQUEST;
    hdr.code = 0;
    hdr.checksum = 0;
    hdr.id = net_htons(id);
    hdr.seq = net_htons(seq);
    memcpy(pkt, &hdr, sizeof(hdr));
    for (u32 i = sizeof(hdr); i < sizeof(pkt); i++) pkt[i] = (u8)('a' + (i % 23)); /* заполнитель, как у настоящего ping */
    u16 csum = net_checksum(pkt, sizeof(pkt));
    memcpy(pkt + offsetof(icmp_hdr_t, checksum), &csum, 2);

    g_pending_id = id;
    g_pending_seq = seq;
    g_reply_seen = FALSE;
    g_pending = TRUE;

    u64 sent_tick = timer_ticks();
    if (!ip_send(dst, IP_PROTO_ICMP, pkt, sizeof(pkt))) {
        g_pending = FALSE;
        return FALSE;
    }

    u64 deadline = sent_tick + ms_to_ticks(timeout_ms);
    while (timer_ticks() < deadline) {
        if (g_reply_seen) {
            g_pending = FALSE;
            if (out_rtt_ms) *out_rtt_ms = ticks_to_ms(g_reply_tick - sent_tick);
            return TRUE;
        }
        task_sleep(5);
    }

    g_pending = FALSE;
    return FALSE;
}
