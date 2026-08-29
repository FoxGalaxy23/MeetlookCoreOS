#include "users.h"
#include "console.h"
#include "kprintf.h"
#include "vga.h"
#include "string.h"
#include "keyboard.h"
#include "userdisk.h"
#include "crypto_rng.h"

static user_t   table[USERS_MAX];
static u32      next_uid = 0;
static s32      current_slot = -1; /* индекс в table[] вошедшего пользователя */

/* ---------- вспомогательные ---------- */

static s32 find_slot(const char *name) {
    for (u32 i = 0; i < USERS_MAX; i++) {
        if (table[i].used && strcmp(table[i].name, name) == 0) return (s32)i;
    }
    return -1;
}

static s32 find_free_slot(void) {
    for (u32 i = 0; i < USERS_MAX; i++) {
        if (!table[i].used) return (s32)i;
    }
    return -1;
}

static u32 root_count(void) {
    u32 n = 0;
    for (u32 i = 0; i < USERS_MAX; i++) {
        if (table[i].used && table[i].is_root) n++;
    }
    return n;
}

static bool_t name_is_valid(const char *name) {
    if (!name || name[0] == '\0') return FALSE;
    u32 len = strlen(name);
    if (len >= USER_NAME_MAX) return FALSE;
    for (u32 i = 0; i < len; i++) {
        char c = name[i];
        bool_t ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                    (c >= '0' && c <= '9') || c == '_' || c == '-';
        if (!ok) return FALSE;
    }
    return TRUE;
}

/* ---------- инициализация ---------- */

void users_init(void) {
    memset(table, 0, sizeof(table));
    next_uid = 0;
    current_slot = -1;

    /* root : root (uid 0) -- всегда существует, всегда root */
    table[0].used = TRUE;
    table[0].uid = next_uid++;
    strncpy(table[0].name, "root", USER_NAME_MAX);
    strncpy(table[0].password, "root", USER_PASS_MAX);
    table[0].is_root = TRUE;

    /* user : user (uid 1) -- обычный пользователь по умолчанию */
    table[1].used = TRUE;
    table[1].uid = next_uid++;
    strncpy(table[1].name, "user", USER_NAME_MAX);
    strncpy(table[1].password, "user", USER_PASS_MAX);
    table[1].is_root = FALSE;
}

/* ---------- ввод пароля с маскировкой ---------- */

u32 users_read_password(char *buf, u32 maxlen) {
    u32 len = 0;
    for (;;) {
        char c = keyboard_getchar();
        if (c == '\n') {
            vga_putc('\n');
            buf[len] = '\0';
            return len;
        } else if (c == '\b') {
            if (len > 0) {
                len--;
                vga_backspace();
            }
        } else {
            if (len < maxlen - 1) {
                buf[len++] = c;
                vga_putc('*');
            }
        }
    }
}

/* ---------- запросы к таблице ---------- */

u32 users_count(void) {
    u32 n = 0;
    for (u32 i = 0; i < USERS_MAX; i++) if (table[i].used) n++;
    return n;
}

user_t *users_get_slot(u32 index) {
    if (index >= USERS_MAX) return 0;
    if (!table[index].used) return 0;
    return &table[index];
}

user_t *users_find(const char *name) {
    s32 idx = find_slot(name);
    if (idx < 0) return 0;
    return &table[idx];
}

const char *users_name_by_uid(u32 uid) {
    for (u32 i = 0; i < USERS_MAX; i++) {
        if (table[i].used && table[i].uid == uid) return table[i].name;
    }
    return "?";
}

/* ---------- текущий пользователь ---------- */

u32 users_current_uid(void) {
    if (current_slot < 0) return 0;
    return table[current_slot].uid;
}

const char *users_current_name(void) {
    if (current_slot < 0) return "?";
    return table[current_slot].name;
}

bool_t users_current_is_root(void) {
    if (current_slot < 0) return FALSE;
    return table[current_slot].is_root;
}

/* ---------- управление учётными записями ---------- */

bool_t users_add(const char *name, const char *password, bool_t is_root) {
    if (!name_is_valid(name)) return FALSE;
    if (!password || password[0] == '\0') return FALSE;
    if (strlen(password) >= USER_PASS_MAX) return FALSE;
    if (find_slot(name) >= 0) return FALSE; /* уже существует */

    s32 idx = find_free_slot();
    if (idx < 0) return FALSE; /* таблица заполнена */

    memset(&table[idx], 0, sizeof(user_t));
    table[idx].used = TRUE;
    table[idx].uid = next_uid++;
    strncpy(table[idx].name, name, USER_NAME_MAX);
    strncpy(table[idx].password, password, USER_PASS_MAX);
    table[idx].is_root = is_root;
    return TRUE;
}

bool_t users_del(const char *name) {
    s32 idx = find_slot(name);
    if (idx < 0) return FALSE;

    if (current_slot == idx) return FALSE; /* нельзя удалить себя, пока вошёл */
    if (table[idx].is_root && root_count() <= 1) return FALSE; /* в системе должен остаться хотя бы один root */

    memset(&table[idx], 0, sizeof(user_t));
    return TRUE;
}

bool_t users_set_password(const char *name, const char *password) {
    s32 idx = find_slot(name);
    if (idx < 0) return FALSE;
    if (!password || password[0] == '\0') return FALSE;
    if (strlen(password) >= USER_PASS_MAX) return FALSE;

    strncpy(table[idx].password, password, USER_PASS_MAX);
    return TRUE;
}

bool_t users_authenticate(const char *name, const char *password) {
    s32 idx = find_slot(name);
    if (idx < 0) return FALSE;
    return strcmp(table[idx].password, password) == 0;
}

bool_t users_switch_current(const char *name) {
    s32 idx = find_slot(name);
    if (idx < 0) return FALSE;
    current_slot = idx;
    return TRUE;
}

/* ---------- мост к дисковым образам (ATOMIC_PLAN.md, Фаза 5) ---------- */

/* См. include/userdisk.h за общей картиной -- запись в user-images
 * (диск, PBKDF2-пароль, свой зашифрованный дом) и запись в table[]
 * здесь -- ДВА разных понятия "пользователь". После того как
 * userdisk_login() уже проверил РЕАЛЬНЫЙ пароль по диску, эта функция
 * заводит (или переиспользует, если имя почему-то уже занято встроенной
 * учётной записью -- см. комментарий у вызова ниже) соответствующую
 * запись здесь, ТОЛЬКО чтобы whoami/id/su и проверки прав шелла
 * (shell/shell.c) продолжали видеть "текущего пользователя" как обычно.
 * Пароль в этой записи -- НЕ пароль диска (тот никогда не покидает
 * userimg_verify_password() дольше необходимого) и не должен давать
 * доступ через обычный (табличный) путь входа -- поэтому случайная
 * заглушка, не пароль диска и не пустая строка. */
static void make_disk_backed_placeholder(char out[USER_PASS_MAX]) {
    u8 raw[USER_PASS_MAX - 1];
    bool_t have_rng = crypto_rng_init() && crypto_rng_available() && crypto_rng_bytes(raw, sizeof(raw));
    for (u32 i = 0; i < sizeof(raw); i++) {
        /* Детерминированный fallback, если RDRAND недоступен -- здесь это
           не критично (это не ключевой материал, только заглушка,
           непроходимая обычным путём входа лишь потому, что реальный
           пароль диска её всё равно не содержит), в отличие от
           userimg_header_create() (include/userimg.h), которая честно
           отказывает без RDRAND. */
        u8 b = have_rng ? raw[i] : (u8)(i * 31u + 7u);
        out[i] = (char)(33 + (b % 94)); /* печатаемый ASCII 33..126 -- гарантированно не '\0',
                                            иначе strcmp() в users_authenticate() эффективно
                                            укоротил бы пароль до пустого */
    }
    out[sizeof(raw)] = '\0';
}

/* Три исхода, а не bool_t -- см. users_login_prompt() ниже за тем,
 * почему различие между "этого имени нет ни на одном диске" (падаем
 * на встроенную таблицу, как раньше) и "имя на диске есть, но пароль
 * неверный" (сразу "Login incorrect", НЕ пробуя табличный путь тем же
 * паролем) важно сохранить наружу, а не схлопывать в один bool_t. */
typedef enum {
    DISK_LOGIN_NOT_FOUND = 0, /* этого имени нет ни на одном отсканированном диске */
    DISK_LOGIN_FAILED    = 1, /* имя есть, но userdisk_login() отклонил (неверный пароль/битый слот) */
    DISK_LOGIN_OK         = 2  /* имя есть, пароль верный, дом смонтирован, current_slot указывает на (возможно новую) запись */
} disk_login_result_t;

/* Сканирует ВСЕ диски заново при КАЖДОМ вызове (users_login_prompt()
 * ниже, и потенциально 'logout' из шелла в будущем) -- не кэширует
 * между входами, лишний оверхед не критичен на пути входа, а не
 * кэшировать проще и честнее (диск мог измениться между попытками). */
static disk_login_result_t try_disk_login(const char *name, const char *pass) {
    static userdisk_entry_t disk_accounts[USERDISK_MAX_ENTRIES]; /* static -- см. общий комментарий в kernel/userdisk.c про стек */
    u32 disk_count = userdisk_scan(disk_accounts);

    s32 disk_idx = -1;
    for (u32 i = 0; i < disk_count; i++) {
        if (strcmp(disk_accounts[i].username, name) == 0) { disk_idx = (s32)i; break; }
    }
    if (disk_idx < 0) return DISK_LOGIN_NOT_FOUND;

    if (!userdisk_login(&disk_accounts[disk_idx], pass, strlen(pass))) return DISK_LOGIN_FAILED;

    s32 idx = find_slot(name);
    if (idx < 0) {
        idx = find_free_slot();
        if (idx < 0) { current_slot = -1; return DISK_LOGIN_OK; } /* дом уже смонтирован, но таблица сеанса переполнена --
                                      редкий случай, не откатываем монтирование дома ради него,
                                      только не заводим current_slot (whoami/id увидят "?") */
        char placeholder[USER_PASS_MAX];
        make_disk_backed_placeholder(placeholder);
        memset(&table[idx], 0, sizeof(user_t));
        table[idx].used = TRUE;
        table[idx].uid = next_uid++;
        strncpy(table[idx].name, name, USER_NAME_MAX);
        strncpy(table[idx].password, placeholder, USER_PASS_MAX);
        table[idx].is_root = FALSE; /* userimg.h не хранит понятия root -- дисковые пользователи всегда обычные, см. include/userdisk.h */
        memset(placeholder, 0, sizeof(placeholder));
    }
    current_slot = idx;
    return DISK_LOGIN_OK;
}

/* ---------- экран входа ---------- */

void users_login_prompt(void) {
    char name[USER_NAME_MAX];
    char pass[USER_PASS_MAX];

    current_slot = -1;
    for (;;) {
        kprintf("\n");
        kprintf("MeetlookCoreOS login\n");
        kprintf("login: ");
        console_readline(name, USER_NAME_MAX);
        trim(name);
        if (name[0] == '\0') continue;

        kprintf("password: ");
        users_read_password(pass, USER_PASS_MAX);

        /* Сначала -- зашифрованные образы диска (ATOMIC_PLAN.md, Фаза 5).
           DISK_LOGIN_NOT_FOUND -- это имя не значится ни на одном диске
           -- падаем на встроенную таблицу ниже, ровно как раньше.
           DISK_LOGIN_FAILED -- диск это имя ЗНАЕТ, но пароль неверный --
           сразу "Login incorrect", НЕ пробуя табличный путь тем же
           паролем (иначе два разных пароля -- дисковый и
           табличный-заглушка -- давали бы наблюдателю различимые пути
           отказа для одного и того же имени). */
        disk_login_result_t disk_result = try_disk_login(name, pass);
        if (disk_result == DISK_LOGIN_OK) {
            memset(pass, 0, sizeof(pass));
            kprintf("Welcome, %s.\n", current_slot >= 0 ? table[current_slot].name : name);
            return;
        }
        if (disk_result == DISK_LOGIN_FAILED) {
            memset(pass, 0, sizeof(pass));
            kprintf("Login incorrect.\n");
            continue;
        }

        s32 idx = find_slot(name);
        if (idx >= 0 && strcmp(table[idx].password, pass) == 0) {
            current_slot = idx;
            kprintf("Welcome, %s%s.\n", table[idx].name, table[idx].is_root ? " (root)" : "");
            return;
        }
        kprintf("Login incorrect.\n");
    }
}
