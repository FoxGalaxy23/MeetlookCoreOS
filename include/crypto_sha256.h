#ifndef CRYPTO_SHA256_H
#define CRYPTO_SHA256_H

#include "types.h"

/* crypto_sha256.h -- SHA-256 (FIPS 180-4). См. ATOMIC_PLAN.md, Фаза 1.
 *
 * Как и lib/mbr.c/lib/gpt.c -- никакого I/O здесь нет, только буферы
 * в памяти. Используется как компонент HMAC-SHA256 (crypto_hmac.h) и
 * PBKDF2 (crypto_pbkdf2.h), а также напрямую -- где нужна просто
 * проверка целостности блока данных. */

#define SHA256_BLOCK_SIZE  64u  /* размер блока сообщения, байт */
#define SHA256_DIGEST_SIZE 32u  /* размер хэша на выходе, байт */

typedef struct {
    u32 state[8];        /* текущее состояние (H0..H7) */
    u64 total_len;        /* сколько байт сообщения обработано всего */
    u8  buffer[SHA256_BLOCK_SIZE]; /* незавершённый остаток последнего блока */
    u32 buffer_len;       /* сколько байт в buffer валидно (0..63) */
} sha256_ctx_t;

void sha256_init(sha256_ctx_t *ctx);
void sha256_update(sha256_ctx_t *ctx, const u8 *data, u32 len);
void sha256_final(sha256_ctx_t *ctx, u8 out[SHA256_DIGEST_SIZE]);

/* Утилита "всё сразу" -- эквивалент init+update+final одним вызовом. */
void sha256(const u8 *data, u32 len, u8 out[SHA256_DIGEST_SIZE]);

#endif
