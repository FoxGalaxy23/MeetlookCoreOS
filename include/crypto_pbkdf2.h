#ifndef CRYPTO_PBKDF2_H
#define CRYPTO_PBKDF2_H

#include "types.h"

/* crypto_pbkdf2.h -- PBKDF2-HMAC-SHA256 (RFC 8018). См. ATOMIC_PLAN.md,
 * Фаза 1/3: пароль пользователя -> KEK (key-encryption-key) для
 * зашифрованного per-user образа (include/userimg.h).
 *
 * Как и остальные crypto_*.h -- никакого I/O, чистая логика над
 * буферами в памяти. Итерации -- параметр вызывающего кода, не
 * захардкожены здесь (см. общий комментарий в ATOMIC_PLAN.md, Фаза 1:
 * хранится в самом заголовке образа, kdf_iterations). */

/* dklen -- желаемая длина выходного ключа в байтах (для наших целей
 * всегда SHA256_DIGEST_SIZE=32, но функция не ограничивает это
 * значение специально -- обычный PBKDF2 умеет производить ключ любой
 * длины через несколько "блоков" F(), см. RFC 8018 §5.2; для 32-байтного
 * KEK нужен ровно один блок). iterations -- ДОЛЖНО быть >= 1. */
void pbkdf2_hmac_sha256(const u8 *password, u32 password_len,
                         const u8 *salt, u32 salt_len,
                         u32 iterations,
                         u8 *out, u32 dklen);

#endif
