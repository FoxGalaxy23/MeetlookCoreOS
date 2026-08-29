/* ip.c -- см. include/ip.h за общим описанием. */

#include "ip.h"
#include "eth.h"
#include "arp.h"
#include "icmp.h"
#include "udp.h"
#include "tcp.h"
#include "string.h"
#include "kprintf.h"

static u16 g_next_id = 1;

bool_t ip_send(ip_addr_t dst, u8 proto, const void *payload, u32 payload_len) {
    if (!net_is_up()) return FALSE;
    /* net_my_ip() == IP_NONE (0.0.0.0) допустим -- это единственно
       возможный источник для DHCP DISCOVER/REQUEST ДО получения адреса
       (см. net/dhcp.c); пакет в этом случае обязан идти на broadcast, что
       и так единственный сценарий, где отправка без собственного IP
       вообще имеет смысл (без ответа arp_resolve() ни на что другое
       всё равно не смочь получить MAC получателя). */
    if (payload_len > 1500u) return FALSE; /* больше стандартного Ethernet MTU (без фрагментации) */

    ip_addr_t next_hop = net_ip_is_local(dst) || dst == IP_BROADCAST ? dst : (ip_addr_t)0;
    if (next_hop == 0) {
        net_config_t cfg;
        net_get_config(&cfg);
        if (cfg.gateway == IP_NONE) return FALSE; /* нет маршрута -- нет шлюза */
        next_hop = cfg.gateway;
    }

    mac_addr_t dst_mac;
    if (!arp_resolve(next_hop, &dst_mac, 2000)) {
        kprintf("ip: ARP resolve for next-hop failed\n");
        return FALSE;
    }

    u8 buf[IP_HDR_LEN + 1500];
    if (IP_HDR_LEN + payload_len > sizeof(buf)) return FALSE;

    ip_hdr_t hdr;
    hdr.ver_ihl = (4u << 4) | 5u; /* IPv4, заголовок 5*4=20 байт */
    hdr.tos = 0;
    hdr.total_len = net_htons((u16)(IP_HDR_LEN + payload_len));
    hdr.id = net_htons(g_next_id++);
    hdr.flags_frag = 0;
    hdr.ttl = 64;
    hdr.proto = proto;
    hdr.checksum = 0;
    hdr.src = net_htonl(net_my_ip());
    hdr.dst = net_htonl(dst);

    hdr.checksum = net_checksum(&hdr, IP_HDR_LEN);

    memcpy(buf, &hdr, IP_HDR_LEN);
    memcpy(buf + IP_HDR_LEN, payload, payload_len);

    return eth_send(dst_mac, ETHERTYPE_IPV4, buf, IP_HDR_LEN + payload_len);
}

void ip_input(const u8 *packet, u32 len) {
    if (len < IP_HDR_LEN) return;

    ip_hdr_t hdr;
    memcpy(&hdr, packet, IP_HDR_LEN);

    if ((hdr.ver_ihl >> 4) != 4) return; /* не IPv4 */
    u32 ihl = (u32)(hdr.ver_ihl & 0x0Fu) * 4u;
    if (ihl < IP_HDR_LEN || ihl > len) return;

    if (net_checksum(packet, ihl) != 0) return; /* битый заголовок */

    u16 total_len = net_ntohs(hdr.total_len);
    if (total_len < ihl || total_len > len) return;

    /* Фрагментация не поддерживается (см. include/ip.h) -- отбрасываем
       всё, что фрагментировано (MF=1 либо ненулевой fragment offset). */
    u16 flags_frag = net_ntohs(hdr.flags_frag);
    if ((flags_frag & 0x2000u) || (flags_frag & 0x1FFFu) != 0) return;

    ip_addr_t dst = net_ntohl(hdr.dst);

    /* ---- Баг, найденный при разработке Фазы 4.4 (см. CONTAINER_ARCHITECTURE.md,
     * "DHCP-запрос уходит на провод... но RX-роль его не подхватывает") ----
     *
     * net/dhcp.c уже выставляет broadcast-флаг (0x8000) в заголовке
     * BOOTP, ПРОСЯ сервер отвечать на 255.255.255.255 -- именно чтобы
     * не столкнуться с этим фильтром (см. комментарий в build_base(),
     * net/dhcp.c). Флаг -- просьба, а не гарантия: RFC 2131 §4.1 прямо
     * разрешает серверу ответить ЮНИКАСТОМ на ЕЩЁ НЕ НАШ 'yiaddr'
     * (адрес назначения в самом DHCPOFFER/DHCPACK, тот же адрес, ради
     * которого весь обмен и затевался), и многие реализации (в т.ч.
     * QEMU slirp, судя по наблюдаемому поведению) так и делают,
     * независимо от флага. На уровне Ethernet кадр всё равно проходит
     * (dst MAC -- наш собственный, RCR_APM в apps/rtl8139d.c его
     * пропускает без promiscuous-режима), а вот здесь, на уровне IP, до
     * этой правки он отбрасывался: `net_my_ip()` в момент DHCP-обмена
     * ещё 0.0.0.0 (IP_NONE) -- то есть мы ждём именно тот пакет,
     * который должен сообщить нам наш будущий IP, но фильтр требовал,
     * чтобы пакет уже был адресован НА этот ещё не известный нам IP --
     * классическая курица и яйцо.
     *
     * Решение: пока свой адрес не настроен (net_my_ip() == IP_NONE),
     * фильтр по dst не применяем вовсе -- Ethernet-уровень (карта в
     * apps/rtl8139d.c без promiscuous, см. выше) уже отсеял всё, что не
     * адресовано нам напрямую или широковещательно, так что здесь
     * дополнительный отсев IP-адреса назначения не даёт защиты (сейчас
     * ничего не привязано на конкретный IP, только на UDP-порт --
     * см. udp_listen(DHCP_CLIENT_PORT), net/dhcp.c), а только мешает
     * ровно тому обмену, который должен настроить этот самый IP. Как
     * только net_my_ip() перестанет быть IP_NONE (net_set_dhcp_result()
     * либо ifconfig), обычная строгая проверка снова в силе. */
    if (net_my_ip() != IP_NONE && dst != net_my_ip() && dst != IP_BROADCAST) return;

    ip_addr_t src = net_ntohl(hdr.src);
    const u8 *payload = packet + ihl;
    u32 payload_len = total_len - ihl;

    switch (hdr.proto) {
        case IP_PROTO_ICMP:
            icmp_input(src, payload, payload_len);
            break;
        case IP_PROTO_UDP:
            udp_input(src, payload, payload_len);
            break;
        case IP_PROTO_TCP:
            tcp_input(src, payload, payload_len);
            break;
        default:
            break;
    }
}
