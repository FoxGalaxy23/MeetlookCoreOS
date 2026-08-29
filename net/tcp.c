/* tcp.c -- см. include/tcp.h за общим описанием и принятыми упрощениями. */

#include "tcp.h"
#include "ip.h"
#include "string.h"
#include "task.h"
#include "timer.h"

#define TCP_RX_BUF   8192u
#define TCP_MAX_SEG  1400u /* с запасом меньше 1460 (наш анонсируемый MSS), под заголовки */
#define TCP_RTO_MS   500u
#define TCP_MAX_RETRIES 5u

#define FLAG_FIN 0x01u
#define FLAG_SYN 0x02u
#define FLAG_RST 0x04u
#define FLAG_PSH 0x08u
#define FLAG_ACK 0x10u
#define FLAG_URG 0x20u

typedef struct __attribute__((packed)) {
    u16 src_port;
    u16 dst_port;
    u32 seq;
    u32 ack;
    u8  data_offset; /* верхние 4 бита -- длина заголовка в 32-битных словах */
    u8  flags;
    u16 window;
    u16 checksum;
    u16 urgent;
} tcp_hdr_t;

#define TCP_HDR_LEN 20u

enum {
    TCP_CLOSED = 0,
    TCP_SYN_SENT,
    TCP_ESTABLISHED,
    TCP_CLOSE_WAIT, /* удалённая сторона прислала FIN, мы ещё нет */
    TCP_CLOSING,    /* мы отправили FIN (после ESTABLISHED или CLOSE_WAIT) --
                        полная детализация FIN_WAIT1/2/CLOSING/LAST_ACK из
                        RFC 793 сознательно не различается, см. include/tcp.h */
};

typedef struct {
    volatile bool_t in_use;
    volatile u8 state;
    u16 local_port;
    ip_addr_t remote_ip;
    u16 remote_port;

    volatile u32 snd_una;
    u32 snd_nxt;
    volatile u32 rcv_nxt;
    u16 peer_mss;

    volatile bool_t fin_received;
    volatile bool_t rst_received;

    u8 rxbuf[TCP_RX_BUF];
    volatile u32 rx_len;

    /* Единственный "летящий" неподтверждённый сегмент (см. общий
       комментарий про stop-and-wait в include/tcp.h). */
    volatile bool_t seg_outstanding;
    u32 seg_seq;
    u32 seg_len;   /* длина в пространстве последовательности: 0 для чистого
                       управляющего сегмента без данных, 1 для голого SYN/FIN,
                       иначе -- число байт данных (+1, если вместе с FIN) */
    u8  seg_flags;
    u8  seg_data[TCP_MAX_SEG];
    volatile u64 seg_sent_tick;
    u32 seg_retries;
} tcp_socket_t;

static tcp_socket_t g_socks[TCP_MAX_SOCKETS];
static u16 g_next_local_port = 49152;

void tcp_init(void) {
    for (u32 i = 0; i < TCP_MAX_SOCKETS; i++) g_socks[i].in_use = FALSE;
}

static u64 ms_to_ticks(u32 ms) {
    u32 freq = timer_frequency();
    if (freq == 0) freq = 100;
    u64 t = ((u64)ms * (u64)freq) / 1000u;
    return t == 0 ? 1 : t;
}

static tcp_socket_t *sock_get(s32 h) {
    if (h < 0 || (u32)h >= TCP_MAX_SOCKETS) return NULL;
    if (!g_socks[h].in_use) return NULL;
    return &g_socks[h];
}

static s32 sock_alloc(void) {
    u64 f = net_irq_save();
    for (u32 i = 0; i < TCP_MAX_SOCKETS; i++) {
        if (!g_socks[i].in_use) {
            memset((void *)&g_socks[i], 0, sizeof(g_socks[i]));
            g_socks[i].in_use = TRUE;
            net_irq_restore(f);
            return (s32)i;
        }
    }
    net_irq_restore(f);
    return -1;
}

static u16 alloc_local_port(void) {
    u16 p = g_next_local_port++;
    if (g_next_local_port == 0) g_next_local_port = 49152;
    return p;
}

/* Отправляет сегмент с явно заданными seq/flags/данными; сама решает
   window по свободному месту в rxbuf. Опция MSS добавляется только для
   сегментов с флагом SYN (её место -- строго в SYN, RFC 793 §3.1). */
static bool_t send_segment(tcp_socket_t *s, u32 seq, u8 flags, const u8 *data, u32 data_len) {
    u8 packet[TCP_HDR_LEN + 4 + TCP_MAX_SEG];
    bool_t with_mss = (flags & FLAG_SYN) != 0;
    u32 opt_len = with_mss ? 4u : 0u;
    u32 hdr_total = TCP_HDR_LEN + opt_len;

    if (hdr_total + data_len > sizeof(packet)) return FALSE;

    tcp_hdr_t hdr;
    hdr.src_port = net_htons(s->local_port);
    hdr.dst_port = net_htons(s->remote_port);
    hdr.seq = net_htonl(seq);
    hdr.ack = net_htonl((flags & FLAG_ACK) ? s->rcv_nxt : 0);
    hdr.data_offset = (u8)(((hdr_total / 4u) & 0x0Fu) << 4);
    hdr.flags = flags;
    u32 space = TCP_RX_BUF - s->rx_len;
    hdr.window = net_htons((u16)(space > 65535u ? 65535u : space));
    hdr.checksum = 0;
    hdr.urgent = 0;

    memcpy(packet, &hdr, TCP_HDR_LEN);
    if (with_mss) {
        packet[TCP_HDR_LEN + 0] = 2;  /* kind = MSS */
        packet[TCP_HDR_LEN + 1] = 4;  /* length */
        u16 mss = net_htons(1460);
        memcpy(packet + TCP_HDR_LEN + 2, &mss, 2);
    }
    if (data_len > 0) memcpy(packet + hdr_total, data, data_len);

    struct __attribute__((packed)) {
        u32 src, dst;
        u8 zero, proto;
        u16 len;
    } pseudo;
    pseudo.src = net_htonl(net_my_ip());
    pseudo.dst = net_htonl(s->remote_ip);
    pseudo.zero = 0;
    pseudo.proto = IP_PROTO_TCP;
    pseudo.len = net_htons((u16)(hdr_total + data_len));

    u32 acc = net_checksum_partial(0, &pseudo, sizeof(pseudo));
    acc = net_checksum_partial(acc, packet, hdr_total + data_len);
    ((tcp_hdr_t *)packet)->checksum = net_checksum_finish(acc);

    return ip_send(s->remote_ip, IP_PROTO_TCP, packet, hdr_total + data_len);
}

/* Готовит сегмент к (пере)отправке и запоминает его для возможного
   повтора по таймауту -- используется для SYN, данных и FIN. Пустые
   ACK-и (без данных, без SYN/FIN) через эту функцию не идут: их не
   нужно подтверждать, поэтому и повторять незачем.

   flags -- ТОЧНО то, что уйдёт на провод, без каких-либо добавлений:
   вызывающий код обязан сам решить, нужен ли ACK (для самого первого
   SYN активного открытия -- НЕТ, см. RFC 793 §3.4: сегмент с ACK,
   пришедший в LISTEN без предшествующего SYN от этой стороны, любой
   нормальный стек отвергнет RST'ом; для данных/FIN уже установленного
   соединения -- ДА, см. вызовы в tcp_send()/tcp_close() ниже). */
static bool_t send_tracked(tcp_socket_t *s, u8 flags, const u8 *data, u32 data_len) {
    s->seg_seq = s->snd_nxt;
    s->seg_flags = flags;
    s->seg_len = data_len + (((flags & FLAG_SYN) || (flags & FLAG_FIN)) ? 1u : 0u);
    if (data_len > 0) memcpy(s->seg_data, data, data_len);
    s->seg_retries = 0;
    s->seg_sent_tick = timer_ticks();
    s->seg_outstanding = TRUE;

    bool_t ok = send_segment(s, s->seg_seq, flags, data, data_len);
    s->snd_nxt = s->seg_seq + s->seg_len;
    return ok;
}

/* Вызывается из всех блокирующих циклов ожидания -- если "летящий"
   сегмент не подтверждён дольше TCP_RTO_MS, отправляет его повторно;
   после TCP_MAX_RETRIES неудач считает соединение мёртвым (state =
   TCP_CLOSED, как будто пришёл RST -- с точки зрения вызывающего кода
   выглядит одинаково: дальше ждать нечего). */
static void check_retransmit(tcp_socket_t *s) {
    if (!s->seg_outstanding) return;
    u64 elapsed_ticks = timer_ticks() - s->seg_sent_tick;
    if (elapsed_ticks < ms_to_ticks(TCP_RTO_MS)) return;

    if (s->seg_retries >= TCP_MAX_RETRIES) {
        s->state = TCP_CLOSED;
        s->seg_outstanding = FALSE;
        return;
    }
    s->seg_retries++;
    s->seg_sent_tick = timer_ticks();
    send_segment(s, s->seg_seq, s->seg_flags, s->seg_data,
                 s->seg_len - (((s->seg_flags & FLAG_SYN) || (s->seg_flags & FLAG_FIN)) ? 1u : 0u));
}

static void maybe_finalize_close(tcp_socket_t *s) {
    if (s->state == TCP_CLOSING && !s->seg_outstanding && s->fin_received) {
        s->state = TCP_CLOSED;
    }
}

void tcp_input(ip_addr_t src, const u8 *payload, u32 len) {
    if (len < TCP_HDR_LEN) return;

    tcp_hdr_t hdr;
    memcpy(&hdr, payload, TCP_HDR_LEN);

    struct __attribute__((packed)) {
        u32 src, dst;
        u8 zero, proto;
        u16 len;
    } pseudo;
    pseudo.src = net_htonl(src);
    pseudo.dst = net_htonl(net_my_ip());
    pseudo.zero = 0;
    pseudo.proto = IP_PROTO_TCP;
    pseudo.len = net_htons((u16)len);
    u32 acc = net_checksum_partial(0, &pseudo, sizeof(pseudo));
    acc = net_checksum_partial(acc, payload, len);
    if (net_checksum_finish(acc) != 0) return; /* битый сегмент */

    u32 doff = (u32)(hdr.data_offset >> 4) * 4u;
    if (doff < TCP_HDR_LEN || doff > len) return;
    const u8 *data = payload + doff;
    u32 data_len = len - doff;

    u16 src_port = net_ntohs(hdr.src_port);
    u16 dst_port = net_ntohs(hdr.dst_port);
    u32 seq = net_ntohl(hdr.seq);
    u32 ack = net_ntohl(hdr.ack);

    for (u32 i = 0; i < TCP_MAX_SOCKETS; i++) {
        tcp_socket_t *s = &g_socks[i];
        if (!s->in_use || s->local_port != dst_port || s->remote_ip != src ||
            s->remote_port != src_port || s->state == TCP_CLOSED) {
            continue;
        }

        if (hdr.flags & FLAG_RST) {
            s->rst_received = TRUE;
            s->state = TCP_CLOSED;
            s->seg_outstanding = FALSE;
            return;
        }

        if (s->state == TCP_SYN_SENT) {
            if ((hdr.flags & FLAG_SYN) && (hdr.flags & FLAG_ACK) && ack == s->snd_nxt) {
                s->rcv_nxt = seq + 1;
                s->snd_una = ack;
                s->seg_outstanding = FALSE;
                s->peer_mss = 536; /* значение по умолчанию, если опция MSS отсутствует */
                if (doff > TCP_HDR_LEN) {
                    u32 oi = TCP_HDR_LEN;
                    while (oi + 1 < doff) {
                        u8 kind = payload[oi];
                        if (kind == 0) break;
                        if (kind == 1) { oi++; continue; }
                        if (oi + 1 >= doff) break;
                        u8 olen = payload[oi + 1];
                        if (olen < 2 || oi + olen > doff) break;
                        if (kind == 2 && olen == 4) {
                            u16 mss; memcpy(&mss, payload + oi + 2, 2);
                            s->peer_mss = net_ntohs(mss);
                        }
                        oi += olen;
                    }
                }
                s->state = TCP_ESTABLISHED;
                send_segment(s, s->snd_nxt, FLAG_ACK, NULL, 0);
            }
            return;
        }

        /* ESTABLISHED / CLOSE_WAIT / CLOSING: обрабатываем ACK, данные и FIN. */
        if (hdr.flags & FLAG_ACK) {
            if (s->seg_outstanding) {
                u32 seg_end = s->seg_seq + s->seg_len;
                if ((s32)(ack - seg_end) >= 0 && (s32)(ack - s->snd_una) > 0) {
                    s->snd_una = ack;
                    s->seg_outstanding = FALSE;
                }
            }
        }

        if (data_len > 0 && seq == s->rcv_nxt) {
            u32 space = TCP_RX_BUF - s->rx_len;
            u32 copy_len = data_len > space ? space : data_len;
            if (copy_len > 0) {
                memcpy(s->rxbuf + s->rx_len, data, copy_len);
                s->rx_len += copy_len;
                s->rcv_nxt += copy_len;
            }
            send_segment(s, s->snd_nxt, FLAG_ACK, NULL, 0);
        } else if (data_len > 0) {
            /* Сегмент не по порядку -- молча отбрасываем (см. общий
               комментарий про отсутствие пересборки в include/tcp.h);
               peer сам решит повторить его по таймауту. */
        }

        if (hdr.flags & FLAG_FIN) {
            u32 fin_seq = seq + data_len;
            if (fin_seq == s->rcv_nxt) {
                s->rcv_nxt += 1;
                s->fin_received = TRUE;
                if (s->state == TCP_ESTABLISHED) s->state = TCP_CLOSE_WAIT;
                send_segment(s, s->snd_nxt, FLAG_ACK, NULL, 0);
            }
        }

        maybe_finalize_close(s);
        return;
    }
}

s32 tcp_connect(ip_addr_t dst_ip, u16 dst_port, u32 timeout_ms) {
    if (!net_is_up() || net_my_ip() == IP_NONE) return -1;

    s32 h = sock_alloc();
    if (h < 0) return -1;
    tcp_socket_t *s = &g_socks[h];

    s->local_port = alloc_local_port();
    s->remote_ip = dst_ip;
    s->remote_port = dst_port;
    s->state = TCP_SYN_SENT;

    u32 iss = (u32)timer_ticks() * 2654435761u; /* произвольная, но меняющаяся между соединениями */
    s->snd_nxt = iss;
    s->snd_una = iss;

    if (!send_tracked(s, FLAG_SYN, NULL, 0)) {
        s->in_use = FALSE;
        return -1;
    }

    u64 deadline = timer_ticks() + ms_to_ticks(timeout_ms);
    while (timer_ticks() < deadline) {
        if (s->state == TCP_ESTABLISHED) return h;
        if (s->state == TCP_CLOSED) { s->in_use = FALSE; return -1; }
        check_retransmit(s);
        task_sleep(10);
    }

    s->in_use = FALSE; /* таймаут: соединение не установилось */
    return -1;
}

s32 tcp_send(s32 h, const void *buf, u32 len, u32 timeout_ms) {
    tcp_socket_t *s = sock_get(h);
    if (!s) return -1;
    if (s->state != TCP_ESTABLISHED && s->state != TCP_CLOSE_WAIT) return -1;

    const u8 *p = (const u8 *)buf;
    u32 sent = 0;
    u64 deadline = timer_ticks() + ms_to_ticks(timeout_ms);

    while (sent < len) {
        u32 chunk = len - sent;
        if (chunk > TCP_MAX_SEG) chunk = TCP_MAX_SEG;

        if (!send_tracked(s, FLAG_PSH | FLAG_ACK, p + sent, chunk)) return (s32)sent;

        while (s->seg_outstanding) {
            if (s->state == TCP_CLOSED) return (s32)sent;
            if (timer_ticks() >= deadline) return (s32)sent;
            check_retransmit(s);
            task_sleep(10);
        }
        sent += chunk;
    }
    return (s32)sent;
}

s32 tcp_recv(s32 h, void *buf, u32 maxlen, u32 timeout_ms) {
    tcp_socket_t *s = sock_get(h);
    if (!s) return -1;

    u64 deadline = timer_ticks() + ms_to_ticks(timeout_ms);
    while (TRUE) {
        if (s->rx_len > 0) {
            u32 n = s->rx_len;
            if (n > maxlen) n = maxlen;
            u64 f = net_irq_save();
            memcpy(buf, s->rxbuf, n);
            if (n < s->rx_len) memmove(s->rxbuf, s->rxbuf + n, s->rx_len - n);
            s->rx_len -= n;
            net_irq_restore(f);
            /* Место освободилось -- сообщаем peer обновлённое окно, чтобы
               он не застрял, если раньше мы объявляли близкое к нулю. */
            if (s->state == TCP_ESTABLISHED || s->state == TCP_CLOSE_WAIT) {
                send_segment(s, s->snd_nxt, FLAG_ACK, NULL, 0);
            }
            return (s32)n;
        }
        if (s->fin_received) return 0; /* EOF: peer закрыл(а), больше данных не будет */
        if (s->state == TCP_CLOSED) return -1;
        if (timer_ticks() >= deadline) return -1;
        check_retransmit(s);
        task_sleep(10);
    }
}

void tcp_close(s32 h) {
    tcp_socket_t *s = sock_get(h);
    if (!s) return;

    if (s->state == TCP_ESTABLISHED || s->state == TCP_CLOSE_WAIT) {
        send_tracked(s, FLAG_FIN | FLAG_ACK, NULL, 0);
        s->state = TCP_CLOSING;
    }

    /* Короткая попытка дождаться корректного завершения (см. общий
       комментарий про отсутствие полноценного TIME_WAIT в include/tcp.h);
       если удалённая сторона не отвечает -- всё равно освобождаем сокет. */
    u64 deadline = timer_ticks() + ms_to_ticks(1500);
    while (timer_ticks() < deadline && s->state == TCP_CLOSING) {
        check_retransmit(s);
        task_sleep(10);
    }

    s->in_use = FALSE;
}
