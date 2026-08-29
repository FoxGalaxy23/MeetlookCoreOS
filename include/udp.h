#ifndef UDP_H
#define UDP_H

#include "types.h"
#include "net.h"

/* udp.h -- минимальный UDP (RFC 768). Наружу (userspace) UDP-сокеты не
 * экспортируются -- этой ОС они не нужны напрямую: единственные
 * потребители UDP -- собственные клиенты DHCP (net/dhcp.c) и DNS
 * (net/dns.c), у обоих простой шаблон "запрос -> один ответ с таймаутом",
 * под который и сделан этот маленький API с фиксированным пулом
 * "слушателей" по локальному порту (см. UDP_MAX_LISTENERS ниже). */

#define UDP_HDR_LEN 8u
#define UDP_MAX_LISTENERS 4u
#define UDP_LISTENER_BUF 1500u

void udp_init(void);

/* Вызывается ip_input() для IP_PROTO_UDP. */
void udp_input(ip_addr_t src, const u8 *payload, u32 len);

bool_t udp_send(ip_addr_t dst_ip, u16 dst_port, u16 src_port, const void *data, u32 len);

/* Резервирует слот, принимающий пакеты, адресованные local_port. Возвращает
   индекс слота (>=0) либо <0, если все UDP_MAX_LISTENERS заняты. */
s32 udp_listen(u16 local_port);

/* Ждёт (опрос, task_sleep) не дольше timeout_ms один пакет, пришедший в
   слот h. Возвращает длину данных (>=0) либо <0 по таймауту. out_src/
   out_src_port (могут быть NULL) получают адрес отправителя. */
s32 udp_recv(s32 h, void *buf, u32 maxlen, ip_addr_t *out_src, u16 *out_src_port, u32 timeout_ms);

void udp_close(s32 h);

#endif
