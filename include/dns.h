#ifndef DNS_H
#define DNS_H

#include "types.h"
#include "net.h"

/* dns.h -- минимальный DNS-клиент (RFC 1035): только запрос записи A
 * (IPv4-адрес по имени), только рекурсивный запрос к одному настроенному
 * серверу (net_config_t::dns -- получен по DHCP либо задан вручную через
 * 'ifconfig'). Этого достаточно 'wget'/'curl', которым нужно превратить
 * имя хоста из URL в IPv4-адрес перед tcp_connect(). */

/* Если host уже выглядит как точечно-десятичный IPv4-адрес ("1.2.3.4"),
   разбирает его напрямую, без обращения к сети -- этой же функцией
   пользуется и net/dns.c (dns_resolve() сначала проверяет литерал, чтобы
   не слать DNS-запрос там, где он не нужен), и её можно позвать отдельно. */
bool_t dns_parse_ipv4_literal(const char *host, ip_addr_t *out_ip);

/* Резолвит host через DNS-сервер net_config_t::dns; блокирует (опрос) не
   дольше timeout_ms. Возвращает TRUE и заполняет *out_ip при успехе. */
bool_t dns_resolve(const char *host, u32 timeout_ms, ip_addr_t *out_ip);

#endif
