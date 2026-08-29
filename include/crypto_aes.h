#ifndef CRYPTO_AES_H
#define CRYPTO_AES_H

#include "types.h"

/* crypto_aes.h -- AES-256 (FIPS 197), блочный шифр + key schedule.
 * См. ATOMIC_PLAN.md, Фаза 1: "AES-256 (блочный шифр, ECB как
 * примитив, дальше режим -- см. ниже) -- сначала encrypt/decrypt
 * одного 16-байтного блока плюс key schedule, это основа для всего
 * остального".
 *
 * Этот файл даёт РОВНО ОДИНОЧНЫЙ БЛОК (16 байт) encrypt/decrypt --
 * никакого режима (ECB/CBC/XTS) здесь нет, это сознательно оставлено
 * вызывающему коду:
 *   - include/userimg.h (Фаза 3) вызывает aes256_encrypt_block()/
 *     aes256_decrypt_block() напрямую по одному блоку за раз для
 *     оборачивания wrapped_master_key (2 блока) и canary (1 блок) --
 *     это и есть "ECB как примитив" из комментария выше: полей
 *     фиксированной длины, каждое из ключевого материала со случайной
 *     энтропией (RDRAND), одинаковые блоки не встречаются, поэтому
 *     слабость ECB на длинных сообщениях (повторяющиеся блоки видны)
 *     здесь не применима.
 *   - AES-XTS-256 (посекторное шифрование тела образа, Фаза 4) --
 *     отдельный файл (crypto_xts.h, ещё не сделан), tweak = номер LBA,
 *     строится ПОВЕРХ этих же aes256_encrypt_block()/decrypt_block().
 *
 * Никакого I/O -- чистая логика над буферами в памяти, тот же принцип,
 * что у остальных crypto_*.h. */

#define AES256_KEY_SIZE   32u /* 256 бит */
#define AES_BLOCK_SIZE    16u /* 128 бит -- одинаково для AES-128/192/256 */
#define AES256_ROUNDS      14u /* Nr для 256-битного ключа, FIPS 197 Таблица 4.1 */

typedef struct {
    /* Nr+1 раундовых ключей по 16 байт (128 бит) каждый -- развёрнутый
     * ключевой график, посчитан один раз в aes256_key_expand() и
     * переиспользуется на каждый блок. */
    u8 round_keys[(AES256_ROUNDS + 1u) * AES_BLOCK_SIZE];
} aes256_key_t;

/* Разворачивает 32-байтный ключ key в ключевой график ks -- вызвать
 * один раз перед encrypt/decrypt (оба используют один и тот же ks,
 * decrypt сам делает InvMixColumns над копией нужных раундовых ключей
 * -- см. реализацию). */
void aes256_key_expand(const u8 key[AES256_KEY_SIZE], aes256_key_t *ks);

/* Шифрует/расшифровывает РОВНО ОДИН 16-байтный блок in -> out (in и
 * out могут совпадать). ks -- заранее развёрнутый через
 * aes256_key_expand(). */
void aes256_encrypt_block(const aes256_key_t *ks, const u8 in[AES_BLOCK_SIZE], u8 out[AES_BLOCK_SIZE]);
void aes256_decrypt_block(const aes256_key_t *ks, const u8 in[AES_BLOCK_SIZE], u8 out[AES_BLOCK_SIZE]);

#endif
