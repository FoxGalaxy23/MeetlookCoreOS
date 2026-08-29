/* dhcp.c -- см. include/dhcp.h за общим описанием. */

#include "dhcp.h"
#include "net.h"
#include "udp.h"
#include "string.h"
#include "timer.h"

#define DHCP_SERVER_PORT 67u
#define DHCP_CLIENT_PORT 68u
#define DHCP_MAGIC_COOKIE 0x63825363u

#define BOOTREQUEST 1u
#define HTYPE_ETHERNET 1u

#define DHCP_MSG_DISCOVER 1u
#define DHCP_MSG_OFFER    2u
#define DHCP_MSG_REQUEST  3u
#define DHCP_MSG_ACK      5u
#define DHCP_MSG_NAK      6u

#define OPT_SUBNET_MASK  1u
#define OPT_ROUTER       3u
#define OPT_DNS          6u
#define OPT_REQUESTED_IP 50u
#define OPT_MSG_TYPE     53u
#define OPT_SERVER_ID    54u
#define OPT_PARAM_LIST   55u
#define OPT_END          255u
#define OPT_PAD          0u

typedef struct __attribute__((packed)) {
    u8  op, htype, hlen, hops;
    u32 xid;
    u16 secs;
    u16 flags;
    u32 ciaddr, yiaddr, siaddr, giaddr;
    u8  chaddr[16];
    u8  sname[64];
    u8  file[128];
    u32 magic_cookie;
    u8  options[312];
} dhcp_pkt_t;

typedef struct {
    u8 msg_type;
    ip_addr_t yiaddr;
    ip_addr_t server_id;
    ip_addr_t subnet_mask;
    ip_addr_t router;
    ip_addr_t dns;
    bool_t have_mask, have_router, have_dns, have_server_id;
} dhcp_parsed_t;

static u32 build_base(dhcp_pkt_t *pkt, u32 xid, mac_addr_t mac) {
    memset(pkt, 0, sizeof(*pkt));
    pkt->op = BOOTREQUEST;
    pkt->htype = HTYPE_ETHERNET;
    pkt->hlen = 6;
    pkt->hops = 0;
    pkt->xid = net_htonl(xid);
    pkt->secs = 0;
    pkt->flags = net_htons(0x8000u); /* просим сервер отвечать broadcast'ом --
        у нас ещё нет собственного IP, unicast-ответ мы бы просто отбросили
        (см. ip_input(), сравнение dst с net_my_ip()) */
    pkt->ciaddr = 0;
    pkt->yiaddr = 0;
    pkt->siaddr = 0;
    pkt->giaddr = 0;
    memcpy(pkt->chaddr, mac.b, 6);
    pkt->magic_cookie = net_htonl(DHCP_MAGIC_COOKIE);
    return (u32)((u8 *)&pkt->options - (u8 *)pkt); /* длина до начала options */
}

static u32 append_options_discover(u8 *opt) {
    u32 n = 0;
    opt[n++] = OPT_MSG_TYPE; opt[n++] = 1; opt[n++] = DHCP_MSG_DISCOVER;
    opt[n++] = OPT_PARAM_LIST; opt[n++] = 3; opt[n++] = OPT_SUBNET_MASK; opt[n++] = OPT_ROUTER; opt[n++] = OPT_DNS;
    opt[n++] = OPT_END;
    return n;
}

static u32 append_options_request(u8 *opt, ip_addr_t requested_ip, ip_addr_t server_id) {
    u32 n = 0;
    opt[n++] = OPT_MSG_TYPE; opt[n++] = 1; opt[n++] = DHCP_MSG_REQUEST;
    opt[n++] = OPT_REQUESTED_IP; opt[n++] = 4;
    u32 rip = net_htonl(requested_ip);
    memcpy(opt + n, &rip, 4); n += 4;
    opt[n++] = OPT_SERVER_ID; opt[n++] = 4;
    u32 sid = net_htonl(server_id);
    memcpy(opt + n, &sid, 4); n += 4;
    opt[n++] = OPT_PARAM_LIST; opt[n++] = 3; opt[n++] = OPT_SUBNET_MASK; opt[n++] = OPT_ROUTER; opt[n++] = OPT_DNS;
    opt[n++] = OPT_END;
    return n;
}

static void parse_options(const dhcp_pkt_t *pkt, u32 pkt_len, u32 hdr_len, dhcp_parsed_t *out) {
    memset(out, 0, sizeof(*out));
    out->yiaddr = net_ntohl(pkt->yiaddr);

    u32 max_opt = pkt_len > hdr_len ? pkt_len - hdr_len : 0;
    if (max_opt > sizeof(pkt->options)) max_opt = sizeof(pkt->options);

    u32 i = 0;
    while (i < max_opt) {
        u8 code = pkt->options[i];
        if (code == OPT_END) break;
        if (code == OPT_PAD) { i++; continue; }
        if (i + 1 >= max_opt) break;
        u8 len = pkt->options[i + 1];
        if (i + 2 + len > max_opt) break;
        const u8 *val = &pkt->options[i + 2];

        switch (code) {
            case OPT_MSG_TYPE:
                if (len >= 1) out->msg_type = val[0];
                break;
            case OPT_SUBNET_MASK:
                if (len == 4) {
                    u32 v; memcpy(&v, val, 4);
                    out->subnet_mask = net_ntohl(v);
                    out->have_mask = TRUE;
                }
                break;
            case OPT_ROUTER:
                if (len >= 4) {
                    u32 v; memcpy(&v, val, 4);
                    out->router = net_ntohl(v);
                    out->have_router = TRUE;
                }
                break;
            case OPT_DNS:
                if (len >= 4) {
                    u32 v; memcpy(&v, val, 4);
                    out->dns = net_ntohl(v);
                    out->have_dns = TRUE;
                }
                break;
            case OPT_SERVER_ID:
                if (len == 4) {
                    u32 v; memcpy(&v, val, 4);
                    out->server_id = net_ntohl(v);
                    out->have_server_id = TRUE;
                }
                break;
            default:
                break;
        }
        i += 2u + len;
    }
}

bool_t dhcp_request(u32 timeout_ms) {
    if (!net_is_up()) return FALSE;

    mac_addr_t mac = net_my_mac();
    u32 xid = ((u32)mac.b[2] << 24) | ((u32)mac.b[3] << 16) | ((u32)mac.b[4] << 8) | mac.b[5];
    xid ^= (u32)timer_ticks();

    s32 h = udp_listen(DHCP_CLIENT_PORT);
    if (h < 0) return FALSE;

    dhcp_pkt_t pkt;
    u32 hdr_len = build_base(&pkt, xid, mac);
    u32 opt_len = append_options_discover(pkt.options);

    u64 deadline_start = timer_ticks();

    bool_t got_offer = FALSE;
    dhcp_parsed_t offer;
    memset(&offer, 0, sizeof(offer));

    /* Фаза DISCOVER -> OFFER: до 3 попыток, каждая ждёт до 1/3 общего
       таймаута (грубая, но достаточная для QEMU/локальной сети схема
       повторов -- сервер обычно отвечает за десятки миллисекунд). */
    for (u32 attempt = 0; attempt < 3 && !got_offer; attempt++) {
        if (!udp_send(IP_BROADCAST, DHCP_SERVER_PORT, DHCP_CLIENT_PORT, &pkt, hdr_len + opt_len)) break;

        u32 slice = timeout_ms / 3;
        u32 waited = 0;
        while (waited < slice) {
            u8 resp[600];
            ip_addr_t rsrc; u16 rport;
            s32 n = udp_recv(h, resp, sizeof(resp), &rsrc, &rport, 200);
            waited += 200;
            (void)rsrc; (void)rport;
            if (n < (s32)hdr_len) continue;

            dhcp_pkt_t rp;
            memset(&rp, 0, sizeof(rp));
            memcpy(&rp, resp, (u32)n < sizeof(rp) ? (u32)n : sizeof(rp));
            if (net_ntohl(rp.xid) != xid) continue;
            if (net_ntohl(rp.magic_cookie) != DHCP_MAGIC_COOKIE) continue;

            dhcp_parsed_t parsed;
            parse_options(&rp, (u32)n, hdr_len, &parsed);
            if (parsed.msg_type == DHCP_MSG_OFFER) {
                offer = parsed;
                got_offer = TRUE;
                break;
            }
        }
    }

    if (!got_offer || !offer.have_server_id) {
        udp_close(h);
        return FALSE;
    }

    /* Фаза REQUEST -> ACK. */
    u32 req_opt_len = append_options_request(pkt.options, offer.yiaddr, offer.server_id);

    bool_t got_ack = FALSE;
    dhcp_parsed_t ack;
    memset(&ack, 0, sizeof(ack));

    for (u32 attempt = 0; attempt < 3 && !got_ack; attempt++) {
        if (!udp_send(IP_BROADCAST, DHCP_SERVER_PORT, DHCP_CLIENT_PORT, &pkt, hdr_len + req_opt_len)) break;

        u64 elapsed_ms = ((timer_ticks() - deadline_start) * 1000u) / (timer_frequency() ? timer_frequency() : 100u);
        u32 remaining = elapsed_ms < timeout_ms ? timeout_ms - (u32)elapsed_ms : 0;
        u32 slice = remaining / (3 - attempt);
        if (slice == 0) slice = 300;

        u32 waited = 0;
        while (waited < slice) {
            u8 resp[600];
            ip_addr_t rsrc; u16 rport;
            s32 n = udp_recv(h, resp, sizeof(resp), &rsrc, &rport, 200);
            waited += 200;
            (void)rsrc; (void)rport;
            if (n < (s32)hdr_len) continue;

            dhcp_pkt_t rp;
            memset(&rp, 0, sizeof(rp));
            memcpy(&rp, resp, (u32)n < sizeof(rp) ? (u32)n : sizeof(rp));
            if (net_ntohl(rp.xid) != xid) continue;
            if (net_ntohl(rp.magic_cookie) != DHCP_MAGIC_COOKIE) continue;

            dhcp_parsed_t parsed;
            parse_options(&rp, (u32)n, hdr_len, &parsed);
            if (parsed.msg_type == DHCP_MSG_ACK) {
                ack = parsed;
                got_ack = TRUE;
                break;
            }
            if (parsed.msg_type == DHCP_MSG_NAK) {
                udp_close(h);
                return FALSE;
            }
        }
    }

    udp_close(h);
    if (!got_ack) return FALSE;

    ip_addr_t mask = ack.have_mask ? ack.subnet_mask : offer.subnet_mask;
    ip_addr_t router = ack.have_router ? ack.router : offer.router;
    ip_addr_t dns = ack.have_dns ? ack.dns : offer.dns;
    if (mask == 0) mask = ip_make(255, 255, 255, 0); /* разумный запасной вариант, если сервер не прислал */

    net_set_dhcp_result(ack.yiaddr, mask, router, dns);
    return TRUE;
}
