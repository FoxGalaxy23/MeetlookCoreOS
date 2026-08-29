#ifndef USERS_H
#define USERS_H

#include "types.h"

/* users.h -- простая многопользовательская модель поверх шелла.
 *
 * Как и остальной MeetlookCoreOS, это не защита на уровне железа (нет
 * ring 3, нет отдельных адресных пространств -- см. include/kapi.h),
 * а модель на уровне ОС/шелла: таблица учётных записей в памяти ядра,
 * экран входа перед запуском шелла, и проверка прав (root / обычный
 * пользователь) перед выполнением "опасных" команд (shutdown, format,
 * useradd и т.д. -- см. shell/shell.c).
 *
 * Таблица пользователей живёт в куче ядра и существует один раз за
 * сеанс работы (как и все остальные RAM-структуры этой ОС -- сбрасывается
 * при перезагрузке). При старте всегда создаются две базовые учётные
 * записи, как и требуется live-окружению:
 *   root : root   (uid 0, полные права)
 *   user : user   (uid 1, обычный пользователь)
 * Дальше можно свободно добавлять/удалять/менять пользователей командами
 * шелла useradd/userdel/passwd (см. shell/shell.c).
 */

#define USERS_MAX          32
#define USER_NAME_MAX       32
#define USER_PASS_MAX       32

typedef struct {
    bool_t used;                    /* слот занят учётной записью */
    u32    uid;
    char   name[USER_NAME_MAX];
    char   password[USER_PASS_MAX];
    bool_t is_root;                 /* принадлежит группе root (uid 0 всегда root) */
} user_t;

/* Создаёт таблицу пользователей и базовые учётные записи root/user.
   Вызывается один раз при старте ядра, до users_login_prompt(). */
void users_init(void);

/* Блокирующий экран входа: запрашивает логин/пароль в цикле, пока не
   будет введена верная пара. Пароль вводится с маскировкой ('*').
   После успешного входа делает нового пользователя "текущим"
   (users_current_*). Используется и при первом входе, и при 'logout'. */
void users_login_prompt(void);

/* ---- текущий (вошедший) пользователь ---- */
u32         users_current_uid(void);
const char *users_current_name(void);
bool_t      users_current_is_root(void);

/* ---- доступ к таблице учётных записей ---- */
u32     users_count(void);          /* число существующих учётных записей */
user_t *users_get_slot(u32 index);  /* прямой доступ по индексу слота (может быть NULL/used==FALSE) */
user_t *users_find(const char *name);

/* Возвращает имя пользователя с данным uid, или "?", если такого
   пользователя сейчас нет (например, владелец файла был удалён
   командой userdel). Используется для отображения владельца файлов
   (ls, см. fs/ramfs.c). */
const char *users_name_by_uid(u32 uid);

/* ---- управление учётными записями ---- */
/* Создаёт нового пользователя. FALSE, если имя занято, некорректно или
   таблица заполнена. */
bool_t users_add(const char *name, const char *password, bool_t is_root);

/* Удаляет пользователя по имени. Не позволяет удалить последнюю root-
   учётную запись в системе и текущего вошедшего пользователя. */
bool_t users_del(const char *name);

/* Меняет пароль существующего пользователя. */
bool_t users_set_password(const char *name, const char *password);

/* Проверяет пару логин/пароль, ничего не меняя. */
bool_t users_authenticate(const char *name, const char *password);

/* Делает пользователя с этим именем текущим без повторного экрана
   входа (пароль уже должен быть проверен вызывающей стороной, кроме
   случая, когда переключается root -- см. команду 'su' в shell.c).
   FALSE, если такого пользователя нет. */
bool_t users_switch_current(const char *name);

/* Читает пароль с клавиатуры с маскировкой символами '*'. */
u32 users_read_password(char *buf, u32 maxlen);

#endif
