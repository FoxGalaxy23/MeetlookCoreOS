#ifndef IP_H
#define IP_H

#include "types.h"
#include "net.h"

/* ip.h -- минимальный IPv4 (RFC 791): без фрагментации (отправляем и
 * принимаем только кадры, укладывающиеся в один Ethernet-кадр, то есть
 * <= ~1480 байт полезной нагрузки IP -- этого с большим запасом хватает
 * для ICMP echo, DNS/DHCP и заголовков TCP; фрагментированные входящие
 * пакеты просто отбрасываются), без опций (заголовок всегда ровно 20
 * байт), без реальной маршрутизации (либо локальная подсеть -- ARP
 * напрямую на получателя, либо через единственный настроенный шлюз). */

#define IP_PROTO_ICMP 1u
#define IP_PROTO_TCP  6u
#define IP_PROTO_UDP  17u

#define IP_HDR_LEN 20u

typedef struct __attribute__((packed)) {
    u8  ver_ihl;      /* версия (4 бита) + длина заголовка в 32-битных словах (4 бита) */
    u8  tos;
    u16 total_len;    /* сетевой порядок байт, включая заголовок */
    u16 id;
    u16 flags_frag;
    u8  ttl;
    u8  proto;
    u16 checksum;     /* сетевой порядок байт */
    u32 src;          /* сетевой порядок байт */
    u32 dst;          /* сетевой порядок байт */
} ip_hdr_t;

/* Собирает и отправляет один IPv4-пакет: заголовок + payload/payload_len
   (payload -- это уже готовое тело протокола верхнего уровня, ICMP/UDP/
   TCP, с ИХ заголовком, но без контрольной суммы, которую они сами
   считают с учётом псевдозаголовка). Определяет next-hop MAC сам через
   arp_resolve() (net/arp.c) -- локальный получатель или шлюз, смотря по
   net_ip_is_local(). Возвращает FALSE при неудаче ARP (нет ответа) или
   выключенном интерфейсе. */
bool_t ip_send(ip_addr_t dst, u8 proto, const void *payload, u32 payload_len);

/* Вызывается eth_input() для кадров с EtherType == ETHERTYPE_IPV4.
   Проверяет контрольную сумму заголовка, отбрасывает то, что явно не
   нам (dst не совпадает с нашим IP и не broadcast) или фрагментировано,
   и передаёт тело дальше по proto (icmp_input()/udp_input()/tcp_input()). */
void ip_input(const u8 *packet, u32 len);

#endif
