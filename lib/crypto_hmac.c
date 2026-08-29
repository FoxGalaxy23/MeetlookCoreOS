#include "crypto_hmac.h"
#include "string.h"

/* См. include/crypto_hmac.h. Стандартная конструкция RFC 2104:
 *   HMAC(K, m) = H((K' xor opad) || H((K' xor ipad) || m))
 * где K' -- ключ, дополненный нулями до SHA256_BLOCK_SIZE (или сначала
 * пропущенный через H(), если исходный ключ длиннее блока). */

static void derive_block_key(const u8 *key, u32 key_len, u8 out[SHA256_BLOCK_SIZE]) {
    memset(out, 0, SHA256_BLOCK_SIZE);
    if (key_len > SHA256_BLOCK_SIZE) {
        sha256(key, key_len, out); /* out теперь 32 байта хэша, остаток уже 0 из memset */
    } else {
        memcpy(out, key, key_len);
    }
}

void hmac_sha256_init(hmac_sha256_ctx_t *ctx, const u8 *key, u32 key_len) {
    u8 block_key[SHA256_BLOCK_SIZE];
    u8 ipad_key[SHA256_BLOCK_SIZE];
    u32 i;

    derive_block_key(key, key_len, block_key);

    for (i = 0; i < SHA256_BLOCK_SIZE; i++) {
        ipad_key[i] = block_key[i] ^ 0x36u;
        ctx->opad_key[i] = block_key[i] ^ 0x5Cu;
    }

    sha256_init(&ctx->inner);
    sha256_update(&ctx->inner, ipad_key, SHA256_BLOCK_SIZE);
}

void hmac_sha256_update(hmac_sha256_ctx_t *ctx, const u8 *data, u32 len) {
    sha256_update(&ctx->inner, data, len);
}

void hmac_sha256_final(hmac_sha256_ctx_t *ctx, u8 out[SHA256_DIGEST_SIZE]) {
    u8 inner_digest[SHA256_DIGEST_SIZE];
    sha256_final(&ctx->inner, inner_digest);

    sha256_ctx_t outer;
    sha256_init(&outer);
    sha256_update(&outer, ctx->opad_key, SHA256_BLOCK_SIZE);
    sha256_update(&outer, inner_digest, SHA256_DIGEST_SIZE);
    sha256_final(&outer, out);
}

void hmac_sha256(const u8 *key, u32 key_len, const u8 *data, u32 data_len,
                  u8 out[SHA256_DIGEST_SIZE]) {
    hmac_sha256_ctx_t ctx;
    hmac_sha256_init(&ctx, key, key_len);
    hmac_sha256_update(&ctx, data, data_len);
    hmac_sha256_final(&ctx, out);
}
