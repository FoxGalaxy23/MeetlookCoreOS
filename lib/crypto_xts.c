#include "crypto_xts.h"
#include "crypto_hmac.h"
#include "string.h"

/* См. include/crypto_xts.h. */

/* Метки для вывода двух независимых подключей из master_key через
 * HMAC-SHA256 -- см. общий комментарий в include/crypto_xts.h. Разные
 * метки -> разные, по построению не связанные напрямую выходы HMAC,
 * даже притом что используется один и тот же master_key как ключ. */
static const u8 LABEL_KEY1[] = "MLCOS-XTS-KEY1-DATA";
static const u8 LABEL_KEY2[] = "MLCOS-XTS-KEY2-TWEAK";

void aes_xts_key_derive_from_master(const u8 master_key[XTS_MASTER_KEY_LEN], aes_xts_key_t *xk) {
    u8 k1[32], k2[32];
    hmac_sha256(master_key, XTS_MASTER_KEY_LEN, LABEL_KEY1, sizeof(LABEL_KEY1) - 1u, k1); /* -1: без завершающего '\0' строкового литерала */
    hmac_sha256(master_key, XTS_MASTER_KEY_LEN, LABEL_KEY2, sizeof(LABEL_KEY2) - 1u, k2);
    aes256_key_expand(k1, &xk->key1);
    aes256_key_expand(k2, &xk->key2);
    memset(k1, 0, sizeof(k1)); /* best-effort -- см. тот же приём в lib/userimg.c */
    memset(k2, 0, sizeof(k2));
}

/* "Удвоение" тюка в GF(2^128) -- умножение на примитивный элемент x
 * относительно приводящего многочлена x^128+x^7+x^2+x+1 (0x87 в младшем
 * байте), в little-endian байтовом представлении (IEEE P1619 §5.1,
 * та же "gf128mul_x_ble", что использует Linux kernel/dm-crypt и
 * большинство эталонных реализаций XTS). t -- 16 байт, изменяется на
 * месте. */
static void gf128_double(u8 t[16]) {
    u8 carry_out = 0;
    for (u32 i = 0; i < 16; i++) {
        u8 carry_in = (u8)(t[i] >> 7);
        t[i] = (u8)((t[i] << 1) | carry_out);
        carry_out = carry_in;
    }
    if (carry_out) t[0] ^= 0x87u;
}

/* encrypt=TRUE -> aes256_encrypt_block, иначе aes256_decrypt_block --
 * общий цикл по 32 блокам сектора, различается только направлением
 * блочного шифра (сама конструкция XTS вокруг него одинакова что при
 * encrypt, что при decrypt -- см. IEEE P1619 §5.2/5.3: расшифровка
 * использует ТОТ ЖЕ тюк, ту же последовательность gf128_double(), и
 * ту же операцию XOR-до/XOR-после, только внутренний блочный шифр
 * идёт в обратную сторону). */
static void xts_process(const aes_xts_key_t *xk, u64 sector_number,
                         const u8 in[XTS_SECTOR_SIZE], u8 out[XTS_SECTOR_SIZE], bool_t encrypt) {
    /* Тюк T_0 = AES-Encrypt(Key2, LE128(sector_number)) -- ВСЕГДА
     * encrypt, даже при расшифровке данных (см. IEEE P1619 §5.1: тюк
     * не шифруемые данные, это независимая производная от номера
     * сектора величина, шифруемая Key2 ровно одним и тем же образом в
     * обе стороны). */
    u8 tweak[16];
    memset(tweak, 0, 16);
    for (u32 i = 0; i < 8; i++) tweak[i] = (u8)(sector_number >> (8u * i)); /* little-endian 128-бит (верхние 64 бита -- уже 0 из memset) */
    aes256_encrypt_block(&xk->key2, tweak, tweak);

    u32 num_blocks = XTS_SECTOR_SIZE / AES_BLOCK_SIZE; /* 32, см. общий комментарий про ciphertext stealing */
    for (u32 b = 0; b < num_blocks; b++) {
        const u8 *pin = in + b * AES_BLOCK_SIZE;
        u8 *pout = out + b * AES_BLOCK_SIZE;
        u8 block[AES_BLOCK_SIZE];

        for (u32 i = 0; i < AES_BLOCK_SIZE; i++) block[i] = pin[i] ^ tweak[i]; /* P xor T (или C xor T при расшифровке) */
        if (encrypt) aes256_encrypt_block(&xk->key1, block, block);
        else         aes256_decrypt_block(&xk->key1, block, block);
        for (u32 i = 0; i < AES_BLOCK_SIZE; i++) pout[i] = block[i] ^ tweak[i];

        if (b + 1u < num_blocks) gf128_double(tweak); /* следующий блок -- T_{b+1} = T_b * alpha, не нужен после последнего блока */
    }
}

void aes_xts_encrypt_sector(const aes_xts_key_t *xk, u64 sector_number,
                             const u8 in[XTS_SECTOR_SIZE], u8 out[XTS_SECTOR_SIZE]) {
    xts_process(xk, sector_number, in, out, TRUE);
}

void aes_xts_decrypt_sector(const aes_xts_key_t *xk, u64 sector_number,
                             const u8 in[XTS_SECTOR_SIZE], u8 out[XTS_SECTOR_SIZE]) {
    xts_process(xk, sector_number, in, out, FALSE);
}
