/* usertest.c -- ATOMIC_PLAN.md, Фаза 3: собирает заголовок
 * зашифрованного пользовательского образа (userimg_header_create()),
 * сериализует, разбирает обратно, проверяет пароль (верный и
 * неверный), и порчей байт проверяет, что header_hmac действительно
 * защищает весь заголовок -- НИ РАЗУ не трогая настоящий диск (никакого
 * sys_disk_read_raw/write_raw здесь нет и не должно быть). Тот же
 * принцип, что и у apps/atomictest.c для Фазы 2 -- сначала доказать,
 * что формат и крипто-логика работают в памяти, реальная запись слота
 * на диск (apps/create-user.c) -- отдельный, более поздний шаг (см.
 * ATOMIC_PLAN.md за тем, почему он не тут же, следом).
 *
 * Не требует ни одной capability -- вся работа в памяти + сама
 * инструкция RDRAND (не системный вызов). kdf_iterations здесь
 * НАМЕРЕННО маленький (1000, не USERIMG_DEFAULT_KDF_ITERATIONS=200000)
 * -- тест гоняется на каждый билд, секунды PBKDF2 внутри QEMU того не
 * стоят; сила KDF не то, что здесь проверяется (это уже проверено
 * host-сверкой в ATOMIC_PLAN.md, Фаза 3, и известным-ответом вектором
 * в apps/cryptotest.c). */

#include "mlapp.h"
#include "libc.h"
#include "usyscall.h"
#include "userimg.h"

static bool_t g_all_ok = TRUE;

static void check(bool_t cond, const char *what) {
    printf("%s: %s\n", what, cond ? "OK" : "FAIL");
    if (!cond) g_all_ok = FALSE;
}

APP_ENTRY s32 app_main(const kapi_t *kapi, u32 argc, char **argv) {
    (void)kapi; (void)argc; (void)argv;

    printf("=== usertest -- заголовок зашифрованного пользовательского образа (ATOMIC_PLAN.md, Фаза 3) ===\n\n");

    static const char username[] = "mikhail";
    static const char password[] = "correct-horse-battery-staple";
    static const char wrong_password[] = "wrong-password";
    const u32 kdf_iterations = 1000u; /* см. общий комментарий выше -- не боевое значение */

    /* ---- 1. Создание ---- */
    userimg_header_t hdr;
    u8 master_key[USERIMG_MASTER_KEY_LEN];
    bool_t created = userimg_header_create(&hdr, username, (u32)strlen(username),
                                            password, (u32)strlen(password),
                                            kdf_iterations, master_key);
    check(created, "userimg_header_create");
    if (!created) {
        printf("(RDRAND недоступен на этом CPU -- без него нового образа не создать,\n"
               " см. общий комментарий в include/userimg.h; остальные проверки не имеют смысла)\n");
        printf("\n=== usertest: ЕСТЬ FAIL ===\n");
        sys_exit(1);
        return 0;
    }

    printf("username: %s\n", hdr.username);
    printf("kdf_iterations: %u\n", hdr.kdf_iterations);

    /* ---- 2. Serialize -> parse ---- */
    static u8 sector[USERIMG_SECTOR_SIZE];
    userimg_header_serialize(&hdr, sector);

    userimg_header_t parsed;
    bool_t parsed_ok = userimg_header_parse(sector, &parsed);
    check(parsed_ok, "userimg_header_parse (magic совпал)");

    /* ---- 3. Верный пароль -- обязан пройти и вернуть ТОТ ЖЕ master_key ---- */
    if (parsed_ok) {
        u8 recovered[USERIMG_MASTER_KEY_LEN];
        bool_t ok = userimg_verify_password(&parsed, password, (u32)strlen(password), recovered);
        check(ok, "userimg_verify_password(верный пароль)");
        if (ok) {
            bool_t key_matches = TRUE;
            for (u32 i = 0; i < USERIMG_MASTER_KEY_LEN; i++) {
                if (recovered[i] != master_key[i]) { key_matches = FALSE; break; }
            }
            check(key_matches, "master_key совпадает с исходным");
        }
    }

    /* ---- 4. Неверный пароль -- обязан ОТКАЗАТЬ (не утечь, не подставить мусор) ---- */
    {
        u8 dummy[USERIMG_MASTER_KEY_LEN];
        bool_t rejected = !userimg_verify_password(&parsed, wrong_password, (u32)strlen(wrong_password), dummy);
        check(rejected, "userimg_verify_password(неверный пароль) отклоняет");
    }

    /* ---- 5. Порча username -- header_hmac обязан поймать (покрывает
     * ВЕСЬ заголовок, не только ключевые поля) -- см. общий комментарий
     * в include/userimg.h про то, зачем header_hmac считается по
     * magic..canary целиком, а не только по wrapped_master_key. ---- */
    {
        static u8 tampered[USERIMG_SECTOR_SIZE];
        memcpy(tampered, sector, USERIMG_SECTOR_SIZE);
        tampered[8] ^= 0x01u; /* байт 0 поля username, см. lib/userimg.c, OFF_USERNAME */

        userimg_header_t parsed_tampered;
        userimg_header_parse(tampered, &parsed_tampered); /* magic не тронут -- пройдёт */
        u8 dummy[USERIMG_MASTER_KEY_LEN];
        bool_t rejected = !userimg_verify_password(&parsed_tampered, password, (u32)strlen(password), dummy);
        check(rejected, "порченный username -- header_hmac отклоняет");
    }

    /* ---- 6. Порча wrapped_master_key напрямую ---- */
    {
        static u8 tampered[USERIMG_SECTOR_SIZE];
        memcpy(tampered, sector, USERIMG_SECTOR_SIZE);
        tampered[60] ^= 0x01u; /* байт 0 поля wrapped_master_key, см. lib/userimg.c, OFF_WRAPPED */

        userimg_header_t parsed_tampered;
        userimg_header_parse(tampered, &parsed_tampered);
        u8 dummy[USERIMG_MASTER_KEY_LEN];
        bool_t rejected = !userimg_verify_password(&parsed_tampered, password, (u32)strlen(password), dummy);
        check(rejected, "порченный wrapped_master_key -- отклоняет");
    }

    /* ---- 7. Пустой/неинициализированный слот -- magic не совпадёт
     * (так Фаза 5 отличает "тут есть пользователь" от "слот свободен",
     * не имея пароля заранее). ---- */
    {
        static u8 empty[USERIMG_SECTOR_SIZE];
        memset(empty, 0, USERIMG_SECTOR_SIZE);
        userimg_header_t parsed_empty;
        check(!userimg_header_parse(empty, &parsed_empty), "пустой сектор -- magic не совпадает");
    }

    printf("\n=== %s ===\n", g_all_ok ? "usertest: ВСЁ OK" : "usertest: ЕСТЬ FAIL");

    /* ВАЖНО: вход в app_main -- через iretq, не call -- обязателен явный
     * sys_exit(), см. KNOWN_ISSUES.md и apps/atomictest.c. */
    sys_exit(g_all_ok ? 0 : 1);
    return 0; /* не достигается */
}
