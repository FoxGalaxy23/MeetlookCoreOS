#ifndef DHCP_H
#define DHCP_H

#include "types.h"

/* dhcp.h -- минимальный DHCP-клиент (RFC 2131): DISCOVER -> OFFER ->
 * REQUEST -> ACK, без продления аренды (rebind/renew) -- при однократном
 * запуске в QEMU этого достаточно; лизы у QEMU slirp по умолчанию
 * длинные (часы), а эта ОС не работает достаточно долго за один сеанс,
 * чтобы истечение аренды стало практической проблемой. */

/* Выполняет полный цикл DISCOVER/OFFER/REQUEST/ACK, не дольше timeout_ms
   суммарно. При успехе применяет полученную конфигурацию через
   net_set_dhcp_result() (include/net.h) и возвращает TRUE. */
bool_t dhcp_request(u32 timeout_ms);

#endif
