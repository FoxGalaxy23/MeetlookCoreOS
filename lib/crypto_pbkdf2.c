#include "crypto_pbkdf2.h"
#include "crypto_hmac.h"
#include "crypto_sha256.h"
#include "string.h"

/* См. include/crypto_pbkdf2.h. Стандартный алгоритм RFC 8018 §5.2:
 *
 *   DK = T_1 || T_2 || ... || T_l
 *   T_i = F(P, S, c, i)
 *   F(P, S, c, i) = U_1 xor U_2 xor ... xor U_c
 *   U_1 = HMAC(P, S || INT_32_BE(i))
 *   U_j = HMAC(P, U_{j-1})   для j = 2..c
 *
 * SALT_MAX -- запас под самый длинный salt, который у нас реально
 * встретится (userimg_header_t.salt -- 16 байт, см. include/userimg.h);
 * буфер на стеке фиксированного размера проще, чем куча в freestanding
 * окружении без malloc общего назначения. */
#define PBKDF2_SALT_MAX 64u

static void be32(u32 v, u8 out[4]) {
    out[0] = (u8)(v >> 24);
    out[1] = (u8)(v >> 16);
    out[2] = (u8)(v >> 8);
    out[3] = (u8)v;
}

void pbkdf2_hmac_sha256(const u8 *password, u32 password_len,
                         const u8 *salt, u32 salt_len,
                         u32 iterations,
                         u8 *out, u32 dklen) {
    u8 salt_block[PBKDF2_SALT_MAX + 4];
    u32 block_index;
    u32 produced = 0;

    if (salt_len > PBKDF2_SALT_MAX) salt_len = PBKDF2_SALT_MAX; /* не должно происходить с нашими вызывающими -- защитный предел */
    memcpy(salt_block, salt, salt_len);

    for (block_index = 1; produced < dklen; block_index++) {
        be32(block_index, salt_block + salt_len);

        u8 u[SHA256_DIGEST_SIZE];
        hmac_sha256(password, password_len, salt_block, salt_len + 4u, u); /* U_1 */

        u8 t[SHA256_DIGEST_SIZE];
        memcpy(t, u, SHA256_DIGEST_SIZE);

        for (u32 j = 1; j < iterations; j++) {
            u8 u_next[SHA256_DIGEST_SIZE];
            hmac_sha256(password, password_len, u, SHA256_DIGEST_SIZE, u_next); /* U_{j+1} = HMAC(P, U_j) */
            memcpy(u, u_next, SHA256_DIGEST_SIZE);
            for (u32 k = 0; k < SHA256_DIGEST_SIZE; k++) t[k] ^= u[k];
        }

        u32 copy_len = (dklen - produced) < SHA256_DIGEST_SIZE ? (dklen - produced) : SHA256_DIGEST_SIZE;
        memcpy(out + produced, t, copy_len);
        produced += copy_len;
    }
}
