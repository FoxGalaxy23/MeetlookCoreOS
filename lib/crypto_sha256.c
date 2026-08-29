#include "crypto_sha256.h"

/* Реализация по FIPS 180-4. См. include/crypto_sha256.h. */

static const u32 K[64] = {
    0x428a2f98u,0x71374491u,0xb5c0fbcfu,0xe9b5dba5u,0x3956c25bu,0x59f111f1u,0x923f82a4u,0xab1c5ed5u,
    0xd807aa98u,0x12835b01u,0x243185beu,0x550c7dc3u,0x72be5d74u,0x80deb1feu,0x9bdc06a7u,0xc19bf174u,
    0xe49b69c1u,0xefbe4786u,0x0fc19dc6u,0x240ca1ccu,0x2de92c6fu,0x4a7484aau,0x5cb0a9dcu,0x76f988dau,
    0x983e5152u,0xa831c66du,0xb00327c8u,0xbf597fc7u,0xc6e00bf3u,0xd5a79147u,0x06ca6351u,0x14292967u,
    0x27b70a85u,0x2e1b2138u,0x4d2c6dfcu,0x53380d13u,0x650a7354u,0x766a0abbu,0x81c2c92eu,0x92722c85u,
    0xa2bfe8a1u,0xa81a664bu,0xc24b8b70u,0xc76c51a3u,0xd192e819u,0xd6990624u,0xf40e3585u,0x106aa070u,
    0x19a4c116u,0x1e376c08u,0x2748774cu,0x34b0bcb5u,0x391c0cb3u,0x4ed8aa4au,0x5b9cca4fu,0x682e6ff3u,
    0x748f82eeu,0x78a5636fu,0x84c87814u,0x8cc70208u,0x90befffau,0xa4506cebu,0xbef9a3f7u,0xc67178f2u
};

static u32 rotr32(u32 x, u32 n) { return (x >> n) | (x << (32u - n)); }

/* Обрабатывает ровно один 64-байтный блок, обновляя ctx->state. */
static void sha256_process_block(sha256_ctx_t *ctx, const u8 block[SHA256_BLOCK_SIZE]) {
    u32 w[64];
    u32 i;

    for (i = 0; i < 16; i++) {
        w[i] = ((u32)block[i * 4 + 0] << 24) |
               ((u32)block[i * 4 + 1] << 16) |
               ((u32)block[i * 4 + 2] << 8)  |
               ((u32)block[i * 4 + 3]);
    }
    for (i = 16; i < 64; i++) {
        u32 s0 = rotr32(w[i - 15], 7) ^ rotr32(w[i - 15], 18) ^ (w[i - 15] >> 3);
        u32 s1 = rotr32(w[i - 2], 17) ^ rotr32(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }

    u32 a = ctx->state[0], b = ctx->state[1], c = ctx->state[2], d = ctx->state[3];
    u32 e = ctx->state[4], f = ctx->state[5], g = ctx->state[6], h = ctx->state[7];

    for (i = 0; i < 64; i++) {
        u32 s1 = rotr32(e, 6) ^ rotr32(e, 11) ^ rotr32(e, 25);
        u32 ch = (e & f) ^ ((~e) & g);
        u32 temp1 = h + s1 + ch + K[i] + w[i];
        u32 s0 = rotr32(a, 2) ^ rotr32(a, 13) ^ rotr32(a, 22);
        u32 maj = (a & b) ^ (a & c) ^ (b & c);
        u32 temp2 = s0 + maj;

        h = g; g = f; f = e; e = d + temp1;
        d = c; c = b; b = a; a = temp1 + temp2;
    }

    ctx->state[0] += a; ctx->state[1] += b; ctx->state[2] += c; ctx->state[3] += d;
    ctx->state[4] += e; ctx->state[5] += f; ctx->state[6] += g; ctx->state[7] += h;
}

void sha256_init(sha256_ctx_t *ctx) {
    ctx->state[0] = 0x6a09e667u; ctx->state[1] = 0xbb67ae85u;
    ctx->state[2] = 0x3c6ef372u; ctx->state[3] = 0xa54ff53au;
    ctx->state[4] = 0x510e527fu; ctx->state[5] = 0x9b05688cu;
    ctx->state[6] = 0x1f83d9abu; ctx->state[7] = 0x5be0cd19u;
    ctx->total_len = 0;
    ctx->buffer_len = 0;
}

void sha256_update(sha256_ctx_t *ctx, const u8 *data, u32 len) {
    ctx->total_len += len;

    /* Сначала дополняем то, что осталось от предыдущего update() --
     * буфер должен либо остаться неполным, либо ровно на границе
     * блока уйти в sha256_process_block(). */
    if (ctx->buffer_len > 0) {
        u32 need = SHA256_BLOCK_SIZE - ctx->buffer_len;
        u32 take = (len < need) ? len : need;
        u32 i;
        for (i = 0; i < take; i++) ctx->buffer[ctx->buffer_len + i] = data[i];
        ctx->buffer_len += take;
        data += take;
        len -= take;

        if (ctx->buffer_len == SHA256_BLOCK_SIZE) {
            sha256_process_block(ctx, ctx->buffer);
            ctx->buffer_len = 0;
        }
    }

    /* Целые блоки -- напрямую из data, без копирования в buffer. */
    while (len >= SHA256_BLOCK_SIZE) {
        sha256_process_block(ctx, data);
        data += SHA256_BLOCK_SIZE;
        len -= SHA256_BLOCK_SIZE;
    }

    /* Остаток (< 64 байт) -- в буфер, ждать следующего update()/final(). */
    if (len > 0) {
        u32 i;
        for (i = 0; i < len; i++) ctx->buffer[i] = data[i];
        ctx->buffer_len = len;
    }
}

void sha256_final(sha256_ctx_t *ctx, u8 out[SHA256_DIGEST_SIZE]) {
    u64 bit_len = ctx->total_len * 8u;
    u8 pad = 0x80u;
    u32 i;

    /* 0x80, затем нули до 56 байт в блоке (оставляя место под 8-байтную
     * длину), с переносом в новый блок, если текущий остаток > 55 байт --
     * стандартный Merkle-Damgard padding. */
    sha256_update(ctx, &pad, 1);

    u8 zero = 0x00u;
    while (ctx->buffer_len != 56u) {
        sha256_update(ctx, &zero, 1);
    }

    u8 len_bytes[8];
    for (i = 0; i < 8; i++) {
        len_bytes[i] = (u8)(bit_len >> (56u - 8u * i));
    }
    /* Пишем длину напрямую в буфер и обрабатываем финальный блок сами --
     * sha256_update() увеличил бы total_len, что здесь уже не нужно. */
    for (i = 0; i < 8; i++) ctx->buffer[56 + i] = len_bytes[i];
    sha256_process_block(ctx, ctx->buffer);

    for (i = 0; i < 8; i++) {
        out[i * 4 + 0] = (u8)(ctx->state[i] >> 24);
        out[i * 4 + 1] = (u8)(ctx->state[i] >> 16);
        out[i * 4 + 2] = (u8)(ctx->state[i] >> 8);
        out[i * 4 + 3] = (u8)(ctx->state[i]);
    }
}

void sha256(const u8 *data, u32 len, u8 out[SHA256_DIGEST_SIZE]) {
    sha256_ctx_t ctx;
    sha256_init(&ctx);
    sha256_update(&ctx, data, len);
    sha256_final(&ctx, out);
}
