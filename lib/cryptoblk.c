#include "cryptoblk.h"
#include "string.h"

/* См. include/cryptoblk.h. */

/* Размер чанка (в секторах) для одного вызова read_fn/write_fn --
 * компромисс между числом вызовов read_fn/write_fn (меньше -- лучше,
 * особенно если под ними syscall) и размером статического scratch-
 * буфера ниже (16 КиБ при 32 секторах -- терпимо даже с учётом того,
 * что этот же процесс уже держит другие статические буферы, см.
 * apps/create-user.c). Не связано с SECTORS_PER_BLOCK в fs/ufs.c --
 * тот про размер блока файловой системы, этот -- чисто про удобный
 * размер чанка I/O, разные вещи. */
#define CRYPTOBLK_MAX_BATCH_SECTORS 32u

/* Статический scratch-буфер под цифротекст чанка -- НЕ reentrant (как
 * и остальные static-буферы в этом дереве, см. g_atomic_readback в
 * apps/install-os.c) -- предполагается однопоточное использование в
 * рамках одного процесса, что для ring 3 приложений в этой ОС и так
 * единственный сценарий. */
static u8 g_scratch[CRYPTOBLK_MAX_BATCH_SECTORS * XTS_SECTOR_SIZE];

void cryptoblk_init(cryptoblk_dev_t *dev, const u8 master_key[XTS_MASTER_KEY_LEN],
                     cryptoblk_read_fn read_fn, cryptoblk_write_fn write_fn, void *io_ctx) {
    aes_xts_key_derive_from_master(master_key, &dev->xk);
    dev->read_fn = read_fn;
    dev->write_fn = write_fn;
    dev->io_ctx = io_ctx;
}

bool_t cryptoblk_read(cryptoblk_dev_t *dev, u32 lba, u32 count, void *buf) {
    u8 *out = (u8 *)buf;
    u32 remaining = count;
    u32 cur_lba = lba;

    while (remaining > 0) {
        u32 chunk = remaining > CRYPTOBLK_MAX_BATCH_SECTORS ? CRYPTOBLK_MAX_BATCH_SECTORS : remaining;
        if (!dev->read_fn(dev->io_ctx, cur_lba, chunk, g_scratch)) return FALSE;

        for (u32 i = 0; i < chunk; i++) {
            aes_xts_decrypt_sector(&dev->xk, (u64)(cur_lba + i),
                                    g_scratch + i * XTS_SECTOR_SIZE,
                                    out + i * XTS_SECTOR_SIZE);
        }

        out += (u32)chunk * XTS_SECTOR_SIZE;
        cur_lba += chunk;
        remaining -= chunk;
    }
    return TRUE;
}

bool_t cryptoblk_write(cryptoblk_dev_t *dev, u32 lba, u32 count, const void *buf) {
    const u8 *in = (const u8 *)buf;
    u32 remaining = count;
    u32 cur_lba = lba;

    while (remaining > 0) {
        u32 chunk = remaining > CRYPTOBLK_MAX_BATCH_SECTORS ? CRYPTOBLK_MAX_BATCH_SECTORS : remaining;

        for (u32 i = 0; i < chunk; i++) {
            aes_xts_encrypt_sector(&dev->xk, (u64)(cur_lba + i),
                                    in + i * XTS_SECTOR_SIZE,
                                    g_scratch + i * XTS_SECTOR_SIZE);
        }

        if (!dev->write_fn(dev->io_ctx, cur_lba, chunk, g_scratch)) return FALSE;

        in += (u32)chunk * XTS_SECTOR_SIZE;
        cur_lba += chunk;
        remaining -= chunk;
    }
    return TRUE;
}
