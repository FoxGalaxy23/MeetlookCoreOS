#include "userimg.h"
#include "crypto_rng.h"
#include "crypto_pbkdf2.h"
#include "crypto_hmac.h"
#include "crypto_aes.h"
#include "string.h"

/* См. include/userimg.h. */

/* Известный plaintext для canary -- РОВНО 16 байт (1 блок AES), не
 * секрет сам по себе (это не ключевой материал -- секрет в том, каким
 * ключом он оказался зашифрован верно). Любая фиксированная строка
 * подошла бы -- эта просто узнаваема в hex-дампе при отладке. */
static const u8 CANARY_PLAINTEXT[USERIMG_CANARY_LEN] = {
    'M','L','C','O','S','-','U','S','E','R','I','M','G','!','!','\0'
};

/* Смещения полей в сериализованном виде -- см. include/userimg.h за
 * порядком. PACKED_LEN -- сколько байт идёт ПОД header_hmac (все поля
 * до него, magic..canary включительно) -- используется и при
 * serialize(), и при подсчёте/проверке HMAC, поэтому вынесено в один
 * общий packer, тем же приёмом, что pack_fields() в lib/bootmeta.c. */
#define OFF_MAGIC     0u
#define OFF_VERSION   4u
#define OFF_USERNAME  8u
#define OFF_SALT      (OFF_USERNAME + USERIMG_USERNAME_LEN)
#define OFF_ITER      (OFF_SALT + USERIMG_SALT_LEN)
#define OFF_WRAPPED   (OFF_ITER + 4u)
#define OFF_CANARY    (OFF_WRAPPED + USERIMG_MASTER_KEY_LEN)
#define PACKED_LEN    (OFF_CANARY + USERIMG_CANARY_LEN) /* = 108 */
#define OFF_HMAC      PACKED_LEN

static void put_u32(u8 *p, u32 v) {
    p[0] = (u8)(v & 0xFFu);
    p[1] = (u8)((v >> 8) & 0xFFu);
    p[2] = (u8)((v >> 16) & 0xFFu);
    p[3] = (u8)((v >> 24) & 0xFFu);
}
static u32 get_u32(const u8 *p) {
    return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24);
}

/* Пакует magic..canary (PACKED_LEN=108 байт) -- ровно то, что идёт под
 * header_hmac -- в буфер buf. Используется и serialize(), и подсчётом/
 * проверкой HMAC, чтобы раскладка не разъехалась в двух местах. */
static void pack_fields(const userimg_header_t *hdr, u8 buf[PACKED_LEN]) {
    put_u32(buf + OFF_MAGIC, hdr->magic);
    put_u32(buf + OFF_VERSION, hdr->version);
    memcpy(buf + OFF_USERNAME, hdr->username, USERIMG_USERNAME_LEN);
    memcpy(buf + OFF_SALT, hdr->salt, USERIMG_SALT_LEN);
    put_u32(buf + OFF_ITER, hdr->kdf_iterations);
    memcpy(buf + OFF_WRAPPED, hdr->wrapped_master_key, USERIMG_MASTER_KEY_LEN);
    memcpy(buf + OFF_CANARY, hdr->canary, USERIMG_CANARY_LEN);
}

/* Константное по времени сравнение -- сверка HMAC/canary не должна
 * зависеть от того, на каком байте нашлось первое расхождение (обычный
 * memcmp с ранним выходом теоретически даёт наблюдателю по времени
 * ответа информацию о префиксе). Простая XOR-аккумуляция, без ранних
 * возвратов. */
static bool_t ct_equal(const u8 *a, const u8 *b, u32 len) {
    u8 diff = 0;
    for (u32 i = 0; i < len; i++) diff |= (u8)(a[i] ^ b[i]);
    return diff == 0;
}

/* AES(key) поверх РОВНО N блоков подряд -- используется и для wrap
 * (2 блока master_key под KEK), и для canary (1 блок под master_key).
 * encrypt=TRUE -> encrypt_block, иначе decrypt_block. */
static void aes_blocks(const u8 *key, const u8 *in, u8 *out, u32 n_blocks, bool_t encrypt) {
    aes256_key_t ks;
    aes256_key_expand(key, &ks);
    for (u32 i = 0; i < n_blocks; i++) {
        if (encrypt) aes256_encrypt_block(&ks, in + i * AES_BLOCK_SIZE, out + i * AES_BLOCK_SIZE);
        else         aes256_decrypt_block(&ks, in + i * AES_BLOCK_SIZE, out + i * AES_BLOCK_SIZE);
    }
}

bool_t userimg_header_create(userimg_header_t *hdr,
                              const char *username, u32 username_len,
                              const char *password, u32 password_len,
                              u32 kdf_iterations,
                              u8 out_master_key[USERIMG_MASTER_KEY_LEN]) {
    if (!crypto_rng_init() || !crypto_rng_available()) return FALSE; /* см. общий комментарий в include/userimg.h -- без RDRAND не создаём */

    memset(hdr, 0, sizeof(*hdr));
    hdr->magic = USERIMG_MAGIC;
    hdr->version = USERIMG_VERSION;

    u32 copy_len = username_len < USERIMG_USERNAME_LEN ? username_len : USERIMG_USERNAME_LEN;
    memcpy(hdr->username, username, copy_len); /* остаток уже 0 из memset выше */

    if (!crypto_rng_bytes(hdr->salt, USERIMG_SALT_LEN)) return FALSE;
    hdr->kdf_iterations = kdf_iterations;

    if (!crypto_rng_bytes(out_master_key, USERIMG_MASTER_KEY_LEN)) return FALSE;

    u8 kek[32];
    pbkdf2_hmac_sha256((const u8 *)password, password_len, hdr->salt, USERIMG_SALT_LEN,
                        kdf_iterations, kek, 32);

    aes_blocks(kek, out_master_key, hdr->wrapped_master_key, USERIMG_MASTER_KEY_LEN / AES_BLOCK_SIZE, TRUE);
    aes_blocks(out_master_key, CANARY_PLAINTEXT, hdr->canary, 1u, TRUE);

    u8 packed[PACKED_LEN];
    pack_fields(hdr, packed);
    hmac_sha256(kek, 32, packed, PACKED_LEN, hdr->header_hmac);

    memset(kek, 0, sizeof(kek)); /* по возможности стереть KEK со стека -- best-effort в freestanding без гарантий от компилятора */
    return TRUE;
}

void userimg_header_serialize(const userimg_header_t *hdr, u8 out[USERIMG_SECTOR_SIZE]) {
    memset(out, 0, USERIMG_SECTOR_SIZE);
    u8 packed[PACKED_LEN];
    pack_fields(hdr, packed);
    memcpy(out, packed, PACKED_LEN);
    memcpy(out + OFF_HMAC, hdr->header_hmac, USERIMG_HMAC_LEN);
    /* offset (OFF_HMAC+32)..511 уже нули из memset выше. */
}

bool_t userimg_header_parse(const u8 sector[USERIMG_SECTOR_SIZE], userimg_header_t *out) {
    u32 magic = get_u32(sector + OFF_MAGIC);
    if (magic != USERIMG_MAGIC) return FALSE; /* см. include/userimg.h -- это и есть "слот пуст/не инициализирован" для Фазы 5 */

    out->magic = magic;
    out->version = get_u32(sector + OFF_VERSION);
    memcpy(out->username, sector + OFF_USERNAME, USERIMG_USERNAME_LEN);
    memcpy(out->salt, sector + OFF_SALT, USERIMG_SALT_LEN);
    out->kdf_iterations = get_u32(sector + OFF_ITER);
    memcpy(out->wrapped_master_key, sector + OFF_WRAPPED, USERIMG_MASTER_KEY_LEN);
    memcpy(out->canary, sector + OFF_CANARY, USERIMG_CANARY_LEN);
    memcpy(out->header_hmac, sector + OFF_HMAC, USERIMG_HMAC_LEN);
    return TRUE;
}

bool_t userimg_verify_password(const userimg_header_t *hdr,
                                const char *password, u32 password_len,
                                u8 out_master_key[USERIMG_MASTER_KEY_LEN]) {
    u8 kek[32];
    pbkdf2_hmac_sha256((const u8 *)password, password_len, hdr->salt, USERIMG_SALT_LEN,
                        hdr->kdf_iterations, kek, 32);

    u8 packed[PACKED_LEN];
    pack_fields(hdr, packed);
    u8 expected_hmac[USERIMG_HMAC_LEN];
    hmac_sha256(kek, 32, packed, PACKED_LEN, expected_hmac);

    if (!ct_equal(expected_hmac, hdr->header_hmac, USERIMG_HMAC_LEN)) {
        memset(kek, 0, sizeof(kek));
        return FALSE;
    }

    u8 candidate_master_key[USERIMG_MASTER_KEY_LEN];
    aes_blocks(kek, hdr->wrapped_master_key, candidate_master_key, USERIMG_MASTER_KEY_LEN / AES_BLOCK_SIZE, FALSE);
    memset(kek, 0, sizeof(kek));

    u8 canary_check[USERIMG_CANARY_LEN];
    aes_blocks(candidate_master_key, hdr->canary, canary_check, 1u, FALSE);
    if (!ct_equal(canary_check, CANARY_PLAINTEXT, USERIMG_CANARY_LEN)) {
        memset(candidate_master_key, 0, sizeof(candidate_master_key));
        return FALSE;
    }

    memcpy(out_master_key, candidate_master_key, USERIMG_MASTER_KEY_LEN);
    return TRUE;
}
