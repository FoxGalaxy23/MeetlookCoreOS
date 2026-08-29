#ifndef ICMP_H
#define ICMP_H

#include "types.h"
#include "net.h"

/* icmp.h -- ICMP echo request/reply (RFC 792), только то, что нужно
 * команде 'ping': отправка запроса и приём ответа. Другие типы ICMP
 * (unreachable, time exceeded...) не обрабатываются -- этой ОС они не
 * нужны, а любой непонятный тип icmp_input() просто игнорирует. */

#define ICMP_TYPE_ECHO_REPLY   0u
#define ICMP_TYPE_ECHO_REQUEST 8u

void icmp_init(void);

/* Вызывается ip_input() для IP_PROTO_ICMP. */
void icmp_input(ip_addr_t src, const u8 *payload, u32 len);

/* Отправляет один ICMP echo request на dst с заданными id/seq (как в
   настоящем ping -- id обычно == pid, seq растёт с каждым пакетом) и
   ждёт ответа не дольше timeout_ms (опрос, см. общий комментарий в
   include/net.h). Возвращает TRUE и заполняет *out_rtt_ms при получении
   ЭТОГО КОНКРЕТНОГО ответа (по id/seq, чтобы не спутать с ответом на
   более старый забытый запрос). */
bool_t icmp_ping(ip_addr_t dst, u16 id, u16 seq, u32 timeout_ms, u32 *out_rtt_ms);

#endif
