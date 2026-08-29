#ifndef CRYPTO_RNG_H
#define CRYPTO_RNG_H

#include "types.h"

/* crypto_rng.h -- аппаратный RNG на базе RDRAND/RDSEED (Intel/AMD x86_64).
 * См. ATOMIC_PLAN.md, Фаза 1.
 *
 * Софтверного PRNG (например, LFSR от таймера) здесь сознательно нет --
 * для соли/IV/ключей нужна настоящая энтропия, а не предсказуемый
 * генератор. Если CPU не поддерживает RDRAND (реально только на очень
 * старом железе -- поддержка с Ivy Bridge, 2012), crypto_rng_init()
 * вернёт FALSE, и вызывающий код (Фаза 3, create-user) обязан
 * отказаться от создания зашифрованных образов, а не подставлять
 * слабую замену. */

/* Проверяет CPUID.1:ECX.RDRAND[bit 30] (и опционально RDSEED через
 * CPUID.7:EBX.RDSEED[bit 18]) и кеширует результат. Обязательно
 * вызвать один раз до первого crypto_rng_bytes(). */
bool_t crypto_rng_init(void);

/* TRUE, если RDRAND действительно доступен на этом CPU (после init). */
bool_t crypto_rng_available(void);

/* Заполняет buf случайными байтами через RDRAND (с ретраями на
 * аппаратный "carry flag == 0" по спецификации Intel -- RDRAND может
 * временно не иметь готовой энтропии). Возвращает FALSE, если RNG
 * недоступен или ретраи исчерпаны (аппаратная неисправность DRNG). */
bool_t crypto_rng_bytes(u8 *buf, u32 len);

#endif
