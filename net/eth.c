/* eth.c -- см. include/eth.h за общим описанием. */

#include "eth.h"
#include "netbridge.h"
#include "arp.h"
#include "ip.h"
#include "string.h"

bool_t eth_send(mac_addr_t dst, u16 ethertype, const void *payload, u32 payload_len) {
    if (!net_is_up()) return FALSE;
    if (payload_len > NETBRIDGE_MAX_FRAME - ETH_HDR_LEN) return FALSE;

    /* Буфер кадра -- на стеке, а НЕ static: eth_send() зовётся из
       разных контекстов задач (ip_send() и т.д. напрямую, а также
       arp_input()/icmp_input(), отвечающие сразу же изнутри потока ядра
       netbridge-rx, см. kernel/netbridge.c: netbridge_rx_pump()) --
       общий static-буфер мог бы быть перезаписан другим из этих
       контекстов ровно между memcpy() и netbridge_push_tx_frame() (сама
       она -- и то, что раньше было rtl8139_send(), -- ДО Фазы 4.4 писала
       прямо в регистры карты; теперь копирует кадр в очередь к
       контейнеру-драйверу, см. include/netbridge.h -- в обоих случаях
       синхронно, до возврата, так что риск для ОБЩЕГО static-буфера был
       бы тем же самым). Кадр маленький (<=1514 байт), стек задачи (см.
       TASK_STACK_SIZE, include/task.h) под него места гарантированно
       хватает. */
    u8 frame[NETBRIDGE_MAX_FRAME];
    eth_hdr_t hdr;
    hdr.dst = dst;
    hdr.src = net_my_mac();
    hdr.ethertype = net_htons(ethertype);

    memcpy(frame, &hdr, ETH_HDR_LEN);
    memcpy(frame + ETH_HDR_LEN, payload, payload_len);

    return netbridge_push_tx_frame(frame, ETH_HDR_LEN + payload_len);
}

void eth_input(const u8 *frame, u32 len) {
    if (len < ETH_HDR_LEN) return;

    eth_hdr_t hdr;
    memcpy(&hdr, frame, ETH_HDR_LEN);
    u16 ethertype = net_ntohs(hdr.ethertype);

    const u8 *payload = frame + ETH_HDR_LEN;
    u32 payload_len = len - ETH_HDR_LEN;

    switch (ethertype) {
        case ETHERTYPE_ARP:
            arp_input(payload, payload_len);
            break;
        case ETHERTYPE_IPV4:
            ip_input(payload, payload_len);
            break;
        default:
            break; /* остальные EtherType (IPv6, VLAN...) этой ОС не нужны */
    }
}
