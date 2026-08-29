/* cryptoblktest.c -- ATOMIC_PLAN.md, Фаза 4: проверяет lib/cryptoblk.c
 * (прозрачный AES-XTS слой поверх абстрактного посекторного I/O) на
 * ПОДДЕЛЬНОМ диске -- статический массив в памяти, read_fn/write_fn
 * читают/пишут прямо в него. НИ РАЗУ не трогает настоящий диск (ни
 * sys_disk_read_raw/write_raw, ни storage_read_sectors/write_sectors)
 * -- тот же принцип, что apps/atomictest.c для Фазы 2 и apps/usertest.c
 * для Фазы 3: сначала доказать, что слой работает сам по себе, поверх
 * ЛЮБОГО read_fn/write_fn, прежде чем связывать его с чем-то реальным
 * (реальным потребителем будет fs/ufs.c, монтирующий тело
 * пользовательского образа -- Фаза 5, ещё не сделана).
 *
 * Не требует ни одной capability -- вся работа в памяти. */

#include "mlapp.h"
#include "libc.h"
#include "usyscall.h"
#include "cryptoblk.h"

#define FAKE_DISK_SECTORS 128u
static u8 g_fake_disk[FAKE_DISK_SECTORS * XTS_SECTOR_SIZE]; /* "физический" носитель -- то, что видит read_fn/write_fn */

static bool_t g_all_ok = TRUE;

static void check(bool_t cond, const char *what) {
    printf("%s: %s\n", what, cond ? "OK" : "FAIL");
    if (!cond) g_all_ok = FALSE;
}

/* read_fn/write_fn для поддельного диска -- io_ctx не нужен (диск один,
 * глобальный), но сигнатура универсальная (см. include/cryptoblk.h),
 * поэтому принимаем и игнорируем. */
static bool_t fake_read(void *io_ctx, u32 lba, u32 count, void *buf) {
    (void)io_ctx;
    if ((u64)lba + count > FAKE_DISK_SECTORS) return FALSE;
    memcpy(buf, g_fake_disk + (u32)lba * XTS_SECTOR_SIZE, (u32)count * XTS_SECTOR_SIZE);
    return TRUE;
}
static bool_t fake_write(void *io_ctx, u32 lba, u32 count, const void *buf) {
    (void)io_ctx;
    if ((u64)lba + count > FAKE_DISK_SECTORS) return FALSE;
    memcpy(g_fake_disk + (u32)lba * XTS_SECTOR_SIZE, buf, (u32)count * XTS_SECTOR_SIZE);
    return TRUE;
}

APP_ENTRY s32 app_main(const kapi_t *kapi, u32 argc, char **argv) {
    (void)kapi; (void)argc; (void)argv;

    printf("=== cryptoblktest -- прозрачный AES-XTS слой (ATOMIC_PLAN.md, Фаза 4) ===\n\n");

    static const u8 master_key[XTS_MASTER_KEY_LEN] = {
        0x00,0x11,0x22,0x33,0x44,0x55,0x66,0x77,0x88,0x99,0xaa,0xbb,0xcc,0xdd,0xee,0xff,
        0x10,0x21,0x32,0x43,0x54,0x65,0x76,0x87,0x98,0xa9,0xba,0xcb,0xdc,0xed,0xfe,0x0f,
    };

    cryptoblk_dev_t dev;
    cryptoblk_init(&dev, master_key, fake_read, fake_write, NULL);

    /* ---- 1. Пишем узнаваемый паттерн на 40 секторов (> CRYPTOBLK_MAX_BATCH_SECTORS=32,
     * чтобы задеть и цикл разбиения на чанки, не только один чанк). ---- */
    static u8 plaintext[40 * XTS_SECTOR_SIZE];
    for (u32 s = 0; s < 40; s++) {
        for (u32 i = 0; i < XTS_SECTOR_SIZE; i++) {
            plaintext[s * XTS_SECTOR_SIZE + i] = (u8)(s * 7 + i); /* разный узнаваемый паттерн на сектор */
        }
    }
    bool_t write_ok = cryptoblk_write(&dev, 10u, 40u, plaintext); /* с намеренным смещением lba=10, не с нуля -- проверяем, что тюк = АБСОЛЮТНЫЙ номер, переданный сюда, а не "с нуля от начала вызова" */
    check(write_ok, "cryptoblk_write(lba=10, 40 секторов)");

    /* ---- 2. На "диске" НЕ должно быть plaintext -- иначе слой ничего
     * не шифрует, а просто копирует байты. ---- */
    bool_t disk_differs = memcmp(g_fake_disk + 10u * XTS_SECTOR_SIZE, plaintext, 40u * XTS_SECTOR_SIZE) != 0;
    check(disk_differs, "содержимое \"диска\" отличается от plaintext (реально зашифровано)");

    /* ---- 3. Читаем обратно -- обязано совпасть с исходным plaintext. ---- */
    static u8 readback[40 * XTS_SECTOR_SIZE];
    bool_t read_ok = cryptoblk_read(&dev, 10u, 40u, readback);
    check(read_ok, "cryptoblk_read(lba=10, 40 секторов)");
    check(memcmp(readback, plaintext, sizeof(plaintext)) == 0, "прочитанное совпадает с исходным plaintext");

    /* ---- 4. Перекрёстная проверка ОДНОГО сектора напрямую через
     * aes_xts_encrypt_sector() -- убеждаемся, что cryptoblk и голый
     * примитив используют ОДИН И ТОТ ЖЕ тюк (здесь -- LBA 15, третий
     * сектор чанка, не первый и не последний -- нарочно, чтобы
     * зацепить возможную ошибку "офсет всегда от начала чанка/вызова"
     * вместо "офсет = переданный lba"). ---- */
    {
        aes_xts_key_t xk;
        aes_xts_key_derive_from_master(master_key, &xk);
        u8 expected_ct[XTS_SECTOR_SIZE];
        aes_xts_encrypt_sector(&xk, 15ull, plaintext + (15u - 10u) * XTS_SECTOR_SIZE, expected_ct);
        check(memcmp(g_fake_disk + 15u * XTS_SECTOR_SIZE, expected_ct, XTS_SECTOR_SIZE) == 0,
              "сектор LBA=15 на \"диске\" совпадает с прямым aes_xts_encrypt_sector(tweak=15)");
    }

    /* ---- 5. Порча одного байта на "диске" -- расшифровка ДОЛЖНА дать
     * не исходный plaintext (не проверка какой-то конкретной пропагации,
     * просто убеждаемся, что порча реально на что-то влияет, а не
     * поглощается по дороге багом). ---- */
    {
        static u8 tampered_readback[XTS_SECTOR_SIZE];
        g_fake_disk[20u * XTS_SECTOR_SIZE] ^= 0x01u; /* один бит в секторе LBA=20 */
        cryptoblk_read(&dev, 20u, 1u, tampered_readback);
        bool_t differs = memcmp(tampered_readback, plaintext + (20u - 10u) * XTS_SECTOR_SIZE, XTS_SECTOR_SIZE) != 0;
        check(differs, "порча байта на \"диске\" -- расшифровка сектора LBA=20 действительно меняется");
        g_fake_disk[20u * XTS_SECTOR_SIZE] ^= 0x01u; /* вернуть как было -- дальше не мешать другим проверкам, если появятся */
    }

    printf("\n=== %s ===\n", g_all_ok ? "cryptoblktest: ВСЁ OK" : "cryptoblktest: ЕСТЬ FAIL");

    sys_exit(g_all_ok ? 0 : 1);
    return 0; /* не достигается */
}
