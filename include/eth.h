#ifndef ETH_H
#define ETH_H

#include "types.h"
#include "net.h"

/* eth.h -- уровень Ethernet II. Заголовок кадра: 6 байт MAC назначения,
 * 6 байт MAC источника, 2 байта EtherType, дальше -- полезная нагрузка. */

#define ETH_HDR_LEN 14u

#define ETHERTYPE_IPV4 0x0800u
#define ETHERTYPE_ARP  0x0806u

typedef struct __attribute__((packed)) {
    mac_addr_t dst;
    mac_addr_t src;
    u16 ethertype; /* сетевой порядок байт */
} eth_hdr_t;

/* Собирает и отправляет один Ethernet-кадр: заголовок + payload/payload_len.
   dst -- MAC назначения (уже известный вызывающему -- см. arp_resolve() в
   net/arp.c, вызывающий это не делает сам). Возвращает FALSE, если карта
   не поднята или кадр не влез в MTU. */
bool_t eth_send(mac_addr_t dst, u16 ethertype, const void *payload, u32 payload_len);

/* Вызывается потоком ядра netbridge_rx_pump() (kernel/netbridge.c) для
   каждого кадра, дошедшего от контейнера-драйвера карты (apps/rtl8139d.c,
   Фаза 4, подшаг 4) через SYS_NETBRIDGE_RX_ENQUEUE. frame указывает на
   НАЧАЛО кадра Ethernet (с заголовком), len -- полная длина кадра без
   CRC. Разбирает заголовок и передаёт payload дальше по EtherType
   (arp_input()/ip_input()). */
void eth_input(const u8 *frame, u32 len);

#endif
