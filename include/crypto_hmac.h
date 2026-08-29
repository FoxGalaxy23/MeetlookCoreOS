#ifndef CRYPTO_HMAC_H
#define CRYPTO_HMAC_H

#include "types.h"
#include "crypto_sha256.h"

/* crypto_hmac.h -- HMAC-SHA256 (FIPS 198-1 / RFC 2104). См.
 * ATOMIC_PLAN.md, Фаза 1.
 *
 * Как и crypto_sha256.h -- никакого I/O, чистая логика над буферами в
 * памяти. Два потребителя: crypto_pbkdf2.h (HMAC -- внутренний примитив
 * PBKDF2) и include/userimg.h, Фаза 3 (аутентификация заголовка
 * зашифрованного пользовательского образа -- отличить "неверный
 * пароль" от "битый диск", см. общий комментарий там же). */

typedef struct {
    sha256_ctx_t inner; /* H(K xor ipad || сообщение) -- накапливается по ходу hmac_update() */
    u8 opad_key[SHA256_BLOCK_SIZE]; /* (K xor opad), сохранён для hmac_final() */
} hmac_sha256_ctx_t;

/* Инициализация с ключом произвольной длины (> SHA256_BLOCK_SIZE --
 * ключ сначала хэшируется до 32 байт, как того требует RFC 2104, п.2). */
void hmac_sha256_init(hmac_sha256_ctx_t *ctx, const u8 *key, u32 key_len);
void hmac_sha256_update(hmac_sha256_ctx_t *ctx, const u8 *data, u32 len);
void hmac_sha256_final(hmac_sha256_ctx_t *ctx, u8 out[SHA256_DIGEST_SIZE]);

/* Утилита "всё сразу" -- эквивалент init+update+final одним вызовом,
 * тем же духом, что sha256() в crypto_sha256.h. */
void hmac_sha256(const u8 *key, u32 key_len, const u8 *data, u32 data_len,
                  u8 out[SHA256_DIGEST_SIZE]);

#endif
