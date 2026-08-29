#ifndef USERIMG_H
#define USERIMG_H

#include "types.h"

/* userimg.h -- заголовок зашифрованного пользовательского образа.
 * См. ATOMIC_PLAN.md, Фаза 3, точный формат оттуда (порядок полей
 * НЕ менять -- он же порядок байт при serialize/parse и порядок,
 * в котором поля идут под header_hmac):
 *
 *   u32 magic
 *   u32 version
 *   char username[32]           -- НЕ зашифровано (список при логине, Фаза 5)
 *   u8  salt[16]                -- для PBKDF2
 *   u32 kdf_iterations
 *   u8  wrapped_master_key[32]  -- master key, зашифрованный AES(KEK) от PBKDF2(пароль, salt)
 *   u8  canary[16]              -- известный plaintext, зашифрованный master key
 *   u8  header_hmac[32]         -- HMAC-SHA256(все поля выше, ключ = KEK)
 *
 * Итого 140 байт полезной нагрузки, дозаполняется нулями до
 * USERIMG_SECTOR_SIZE (как boot-meta -- см. bootmeta.h, тот же приём).
 *
 * ВАЖНО про header_hmac и canary -- почему оба, хотя на первый взгляд
 * избыточно (раз KEK неверный -> header_hmac и так не сойдётся): 
 * header_hmac аутентифицирует ВЕСЬ заголовок целиком (защита от
 * порчи/подмены -- в т.ч. wrapped_master_key), canary -- более дешёвая,
 * не зависящая от остального заголовка проверка именно того, что
 * получившийся master_key расшифровывается в осмысленный вид (лишняя
 * защита от бага в самой реализации unwrap, не только от неверного
 * пароля) -- обе проверки в userimg_verify_password() обязаны пройти.
 *
 * Тело слота ПОСЛЕ заголовка (не описано этим файлом) -- обычный UFS,
 * каждый сектор проходит через AES-XTS при записи/чтении (Фаза 4,
 * ещё не сделана) -- см. общий комментарий в ATOMIC_PLAN.md. */

#define USERIMG_MAGIC 0x474D4955u /* "UIMG" по байтам (little-endian слово) */
#define USERIMG_VERSION 1u

#define USERIMG_USERNAME_LEN 32u
#define USERIMG_SALT_LEN 16u
#define USERIMG_MASTER_KEY_LEN 32u
#define USERIMG_CANARY_LEN 16u
#define USERIMG_HMAC_LEN 32u
#define USERIMG_SECTOR_SIZE 512u

/* Размер ОДНОГО слота внутри GPT-раздела user-images (ATOMIC_PLAN.md,
 * Фаза 0/2) -- сам GPT-раздел один большой пул на всех пользователей
 * (число пользователей не известно на этапе разметки диска, см.
 * include/gpt.h у GPT_GUID_MEETLOOK_USER_IMAGES), а на слоты
 * фиксированного размера его делит уже эта константа -- ЕДИНЫЙ
 * источник правды и для apps/create-user.c (Фаза 3, пишет новые
 * слоты), и для будущего кода Фазы 5 (читает/монтирует существующие).
 * Заголовок (этот файл) занимает только первый сектор слота, остаток
 * -- под будущее тело (UFS + AES-XTS, Фаза 4) -- ПОКА НИЧЕМ не занят.
 *
 * 32 МиБ -- та же осторожная "с запасом, но не окончательно" оценка,
 * что ATOMIC_OS_SLOT_SECTORS в apps/install-os.c: реального образа
 * файловой системы внутри слота ещё нет (Фаза 4), пересмотреть, когда
 * появится представление о типичном размере пользовательских данных. */
#define USERIMG_SLOT_SECTORS 65536u /* 32 MiB */

/* Компромисс скорость/стойкость PBKDF2 -- НЕ гонка за современными
 * рекомендациями (OWASP 2024 для PBKDF2-HMAC-SHA256 -- 600000+), а то,
 * что реально терпимо на неускоренной программной реализации SHA-256
 * внутри QEMU на login-пути: несколько сотен мс на разумном хосте.
 * Осознанный компромисс хобби-ОС, не готовой к продакшену -- см.
 * ATOMIC_PLAN.md. Хранится в самом заголовке (kdf_iterations), так что
 * это значение -- только "умолчание при создании", не жёсткое
 * ограничение формата. */
#define USERIMG_DEFAULT_KDF_ITERATIONS 200000u

typedef struct {
    u32 magic;
    u32 version;
    char username[USERIMG_USERNAME_LEN]; /* НЕ обязательно NUL-terminated, если имя ровно 32 байта */
    u8  salt[USERIMG_SALT_LEN];
    u32 kdf_iterations;
    u8  wrapped_master_key[USERIMG_MASTER_KEY_LEN];
    u8  canary[USERIMG_CANARY_LEN];
    u8  header_hmac[USERIMG_HMAC_LEN];
} userimg_header_t;

/* Создаёт НОВЫЙ заголовок: генерирует master_key и salt через RDRAND
 * (crypto_rng.h), выводит KEK = PBKDF2(password, salt, kdf_iterations),
 * оборачивает master_key в AES(KEK), шифрует канарейку master_key'ом,
 * считает header_hmac. Кладёт РАСШИФРОВАННЫЙ master_key наружу в
 * out_master_key -- он дальше уходит в cryptoblk (Фаза 4) для реального
 * шифрования тела образа; сам этот файл тело не трогает.
 *
 * username длиннее USERIMG_USERNAME_LEN -- обрезается; короче --
 * дополняется нулевыми байтами (НЕ пробелами).
 *
 * Возвращает FALSE, если RDRAND недоступен (crypto_rng_init()/bytes()
 * не смогли) -- без настоящей случайности новый образ создавать
 * нельзя, это не тот случай, где уместен детерминированный fallback
 * (в отличие от, например, disk_guid в install-os.c -- GUID не обязан
 * быть тайной, master_key обязан). */
bool_t userimg_header_create(userimg_header_t *hdr,
                              const char *username, u32 username_len,
                              const char *password, u32 password_len,
                              u32 kdf_iterations,
                              u8 out_master_key[USERIMG_MASTER_KEY_LEN]);

/* serialize/parse -- структурные, БЕЗ проверки пароля (для этого --
 * userimg_verify_password() ниже, ей нужен уже разобранный заголовок
 * плюс сам пароль). parse проверяет только magic -- этого достаточно,
 * чтобы отличить "тут есть заголовок" от "слот пуст/не инициализирован"
 * (Фаза 5 сканирует user-images именно так, не имея пароля заранее). */
void userimg_header_serialize(const userimg_header_t *hdr, u8 out[USERIMG_SECTOR_SIZE]);
bool_t userimg_header_parse(const u8 sector[USERIMG_SECTOR_SIZE], userimg_header_t *out);

/* Пересчитывает KEK из password + hdr->salt/kdf_iterations, сверяет
 * header_hmac и canary (см. общий комментарий выше про оба). При
 * успехе кладёт расшифрованный master_key в out_master_key и
 * возвращает TRUE. При ЛЮБОМ расхождении -- FALSE, ОДНА И ТА ЖЕ
 * ошибка что на неверный пароль, что на битый заголовок (сознательно,
 * см. ATOMIC_PLAN.md, Фаза 5: "не утекает, битый это диск или
 * пароль"). out_master_key не трогается при FALSE. */
bool_t userimg_verify_password(const userimg_header_t *hdr,
                                const char *password, u32 password_len,
                                u8 out_master_key[USERIMG_MASTER_KEY_LEN]);

#endif
