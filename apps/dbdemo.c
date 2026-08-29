/* dbdemo.c -- интерактивная технодемка библиотеки MLDB (include/mldb.h,
 * lib/mldb.c): позволяет с клавиатуры создать базу, создать таблицы с
 * типизированными колонками, вставлять/смотреть/удалять строки,
 * сохранять базу ОДНИМ файлом на диск и загружать обратно.
 *
 * Требует caps=fs_read,fs_write (см. rootfs/containers/dbdemo.manifest)
 * -- без них savedb/loaddb вернут ошибку прав, всё остальное (работа с
 * базой в памяти) от файловых прав не зависит.
 *
 * Команды (см. cmd_help ниже): createdb, tables, createtable, schema,
 * insert, select, delrow, savedb, loaddb, help, exit. */

/* Частная куча этого бинарника увеличена с 256 KiB (значение по
 * умолчанию из include/libc.h) до 768 KiB -- см. Makefile, цель
 * build/apps/dbdemo_libc.o: buf для mldb_load() (см. cmd_loaddb ниже) и
 * сами строки таблиц лежат именно в ней. Значение задаётся при сборке
 * lib/libc.c (-DLIBC_HEAP_SIZE=786432), а не здесь -- сама арена кучи
 * (g_heap_arena) статически лежит внутри lib/libc.c как отдельной
 * единицы трансляции, так что #define в этом файле её бы не изменил.
 * Всё ещё далеко от бюджета APP_MAX_SIZE = 2 MiB на весь образ
 * бинарника, см. include/loader.h. */

#include "mlapp.h"
#include "libc.h"
#include "mldb.h"

#define MAX_LINE     256
#define MAX_ARGS     32
#define MAX_LOAD_SIZE (256u * 1024u) /* верхняя граница на размер файла базы при loaddb */

static mldb_t *g_db = 0;

/* ---- утилиты ---- */

/* Копирует value (TEXT/BLOB) в null-terminated буфер для printf("%s"),
 * т.к. значения в базе НЕ хранят завершающий '\0' -- см. общий
 * комментарий в mldb.h. Обрезает, если не влезло в bufsize. */
static void value_to_cstr(const mldb_value_t *v, char *buf, u32 bufsize) {
    u32 n = v->len < bufsize - 1 ? v->len : bufsize - 1;
    if (n > 0) memcpy(buf, v->bytes, n);
    buf[n] = 0;
}

static const char *type_name(mldb_type_t t) {
    switch (t) {
        case MLDB_INT:  return "int";
        case MLDB_TEXT: return "text";
        case MLDB_BLOB: return "blob";
        default:        return "?";
    }
}

static bool_t parse_type(const char *s, mldb_type_t *out) {
    if (strcmp(s, "int") == 0)  { *out = MLDB_INT;  return TRUE; }
    if (strcmp(s, "text") == 0) { *out = MLDB_TEXT; return TRUE; }
    if (strcmp(s, "blob") == 0) { *out = MLDB_BLOB; return TRUE; }
    return FALSE;
}

/* Разбивает токен вида "name:type[:maxlen]" на составляющие, разрезая
 * строку in-place (как split_ws) -- s должен быть модифицируемым. */
static bool_t parse_column_spec(char *spec, mldb_column_t *col) {
    char *name = spec;
    char *type_s = strchr(spec, ':');
    if (!type_s) return FALSE;
    *type_s = 0; type_s++;

    char *maxlen_s = strchr(type_s, ':');
    if (maxlen_s) { *maxlen_s = 0; maxlen_s++; }

    mldb_type_t type;
    if (!parse_type(type_s, &type)) return FALSE;

    memset(col, 0, sizeof(*col));
    strncpy(col->name, name, MLDB_NAME_LEN - 1);
    col->type = type;
    col->max_len = maxlen_s ? (u32)atoi(maxlen_s) : 0;
    return TRUE;
}

static void require_db(void) {
    if (!g_db) {
        printf("Нет открытой базы -- сначала 'createdb' или 'loaddb <file>'\n");
    }
}

/* ---- команды ---- */

static void cmd_help(void) {
    puts("Команды:");
    puts("  createdb                            -- новая пустая база в памяти");
    puts("  tables                              -- список таблиц");
    puts("  createtable <table> <col:type[:maxlen]> ...");
    puts("                                       -- type = int|text|blob");
    puts("  droptable <table>");
    puts("  schema <table>                      -- показать колонки таблицы");
    puts("  insert <table> <v1> <v2> ...         -- по одному значению на колонку, по порядку");
    puts("  select <table>                      -- показать все строки");
    puts("  delrow <table> <row_index>");
    puts("  savedb <file>                       -- сохранить базу целиком в один файл");
    puts("  loaddb <file>                       -- загрузить базу из файла (заменяет текущую)");
    puts("  help");
    puts("  exit");
}

static void cmd_createdb(void) {
    if (g_db) mldb_free(g_db);
    g_db = mldb_new();
    if (!g_db) { printf("createdb: не хватило памяти\n"); return; }
    printf("Новая пустая база создана в памяти.\n");
}

static void cmd_tables(void) {
    require_db(); if (!g_db) return;
    u32 n = mldb_table_count(g_db);
    if (n == 0) { printf("Таблиц нет.\n"); return; }
    for (u32 i = 0; i < n; i++) {
        const char *name = mldb_table_name_at(g_db, i);
        mldb_table_t *t = mldb_find_table(g_db, name);
        printf("  %s (%u колонок, %u строк)\n",
               name, mldb_table_ncolumns(t), mldb_table_nrows(t));
    }
}

static void cmd_createtable(u32 argc, char **argv) {
    require_db(); if (!g_db) return;
    if (argc < 3) { printf("Использование: createtable <table> <col:type[:maxlen]> ...\n"); return; }

    const char *table = argv[1];
    mldb_column_t cols[MLDB_MAX_COLUMNS];
    u32 ncols = argc - 2;
    if (ncols > MLDB_MAX_COLUMNS) { printf("createtable: слишком много колонок (максимум %d)\n", MLDB_MAX_COLUMNS); return; }

    for (u32 i = 0; i < ncols; i++) {
        if (!parse_column_spec(argv[2 + i], &cols[i])) {
            printf("createtable: не могу разобрать '%s' (ожидалось name:type[:maxlen])\n", argv[2 + i]);
            return;
        }
    }

    s32 rc = mldb_create_table(g_db, table, cols, ncols);
    if (rc < 0) { printf("createtable: ошибка %d\n", rc); return; }
    printf("Таблица '%s' создана (%u колонок).\n", table, ncols);
}

static void cmd_droptable(u32 argc, char **argv) {
    require_db(); if (!g_db) return;
    if (argc < 2) { printf("Использование: droptable <table>\n"); return; }
    s32 rc = mldb_drop_table(g_db, argv[1]);
    if (rc < 0) { printf("droptable: ошибка %d (нет такой таблицы?)\n", rc); return; }
    printf("Таблица '%s' удалена.\n", argv[1]);
}

static void cmd_schema(u32 argc, char **argv) {
    require_db(); if (!g_db) return;
    if (argc < 2) { printf("Использование: schema <table>\n"); return; }
    mldb_table_t *t = mldb_find_table(g_db, argv[1]);
    if (!t) { printf("schema: нет таблицы '%s'\n", argv[1]); return; }

    u32 nc = mldb_table_ncolumns(t);
    printf("Таблица '%s': %u колонок, %u строк\n", argv[1], nc, mldb_table_nrows(t));
    for (u32 i = 0; i < nc; i++) {
        const mldb_column_t *c = mldb_table_column(t, i);
        if (c->max_len) printf("  [%u] %s : %s (max_len=%u)\n", i, c->name, type_name(c->type), c->max_len);
        else            printf("  [%u] %s : %s\n", i, c->name, type_name(c->type));
    }
}

static void cmd_insert(u32 argc, char **argv) {
    require_db(); if (!g_db) return;
    if (argc < 2) { printf("Использование: insert <table> <v1> <v2> ...\n"); return; }

    const char *table = argv[1];
    mldb_table_t *t = mldb_find_table(g_db, table);
    if (!t) { printf("insert: нет таблицы '%s'\n", table); return; }

    u32 nc = mldb_table_ncolumns(t);
    if (argc - 2 != nc) {
        printf("insert: таблица '%s' ожидает %u значений, передано %u\n", table, nc, argc - 2);
        return;
    }

    mldb_value_t values[MLDB_MAX_COLUMNS];
    for (u32 i = 0; i < nc; i++) {
        const mldb_column_t *c = mldb_table_column(t, i);
        if (c->type == MLDB_INT) values[i] = mldb_v_int(atoi(argv[2 + i]));
        else                     values[i] = mldb_v_text(argv[2 + i]); /* blob с консоли вводится как текст */
    }

    s32 idx = mldb_insert(g_db, table, values, nc);
    if (idx < 0) { printf("insert: ошибка %d\n", idx); return; }
    printf("Строка добавлена, индекс %d.\n", idx);
}

typedef struct { const mldb_table_t *t; } select_ctx_t;

static bool_t print_row(u32 row_index, const mldb_value_t *values, u32 nvalues, void *ctx) {
    (void)ctx;
    char buf[128];
    printf("  [%u]", row_index);
    for (u32 i = 0; i < nvalues; i++) {
        if (values[i].type == MLDB_INT) {
            printf(" %ld", (s64)values[i].i);
        } else {
            value_to_cstr(&values[i], buf, sizeof(buf));
            printf(" \"%s\"", buf);
        }
    }
    printf("\n");
    return TRUE; /* продолжить перебор */
}

static void cmd_select(u32 argc, char **argv) {
    require_db(); if (!g_db) return;
    if (argc < 2) { printf("Использование: select <table>\n"); return; }

    mldb_table_t *t = mldb_find_table(g_db, argv[1]);
    if (!t) { printf("select: нет таблицы '%s'\n", argv[1]); return; }

    u32 nc = mldb_table_ncolumns(t);
    printf("Таблица '%s':\n", argv[1]);
    for (u32 i = 0; i < nc; i++) printf(" %s", mldb_table_column(t, i)->name);
    printf("\n");

    s32 n = mldb_select(g_db, argv[1], print_row, 0);
    if (n < 0) { printf("select: ошибка %d\n", n); return; }
    if (n == 0) printf("  (пусто)\n");
}

static void cmd_delrow(u32 argc, char **argv) {
    require_db(); if (!g_db) return;
    if (argc < 3) { printf("Использование: delrow <table> <row_index>\n"); return; }
    u32 idx = (u32)atoi(argv[2]);
    s32 rc = mldb_delete_row(g_db, argv[1], idx);
    if (rc < 0) { printf("delrow: ошибка %d\n", rc); return; }
    printf("Строка %u таблицы '%s' удалена.\n", idx, argv[1]);
}

static void cmd_savedb(u32 argc, char **argv) {
    require_db(); if (!g_db) return;
    if (argc < 2) { printf("Использование: savedb <file>\n"); return; }
    s32 rc = mldb_save(g_db, argv[1]);
    if (rc < 0) { printf("savedb: ошибка %d (нет прав fs_write? см. манифест)\n", rc); return; }
    printf("База сохранена в '%s' одним файлом.\n", argv[1]);
}

static void cmd_loaddb(u32 argc, char **argv) {
    if (argc < 2) { printf("Использование: loaddb <file>\n"); return; }
    mldb_t *loaded = mldb_load(argv[1], MAX_LOAD_SIZE);
    if (!loaded) { printf("loaddb: не удалось загрузить '%s' (нет файла/прав/не MLDB-формат)\n", argv[1]); return; }
    if (g_db) mldb_free(g_db);
    g_db = loaded;
    printf("База загружена из '%s': %u таблиц.\n", argv[1], mldb_table_count(g_db));
}

/* ---- главный цикл ---- */

APP_ENTRY s32 app_main(const kapi_t *kapi, u32 argc, char **argv) {
    (void)kapi; (void)argc; (void)argv;

    printf("=== MLDB demo -- однофайловая табличная база MeetlookCoreOS ===\n");
    printf("Введите 'help' за списком команд, 'exit' для выхода.\n");

    char line[MAX_LINE];
    char *cmd_argv[MAX_ARGS];

    for (;;) {
        printf("mldb> ");
        s32 len = readline(line, sizeof(line));
        if (len < 0) break; /* EOF/ошибка ввода */

        u32 n = split_ws(line, cmd_argv, MAX_ARGS);
        if (n == 0) continue;

        const char *cmd = cmd_argv[0];
        if (strcmp(cmd, "help") == 0)             cmd_help();
        else if (strcmp(cmd, "createdb") == 0)    cmd_createdb();
        else if (strcmp(cmd, "tables") == 0)      cmd_tables();
        else if (strcmp(cmd, "createtable") == 0) cmd_createtable(n, cmd_argv);
        else if (strcmp(cmd, "droptable") == 0)   cmd_droptable(n, cmd_argv);
        else if (strcmp(cmd, "schema") == 0)      cmd_schema(n, cmd_argv);
        else if (strcmp(cmd, "insert") == 0)      cmd_insert(n, cmd_argv);
        else if (strcmp(cmd, "select") == 0)      cmd_select(n, cmd_argv);
        else if (strcmp(cmd, "delrow") == 0)      cmd_delrow(n, cmd_argv);
        else if (strcmp(cmd, "savedb") == 0)      cmd_savedb(n, cmd_argv);
        else if (strcmp(cmd, "loaddb") == 0)      cmd_loaddb(n, cmd_argv);
        else if (strcmp(cmd, "exit") == 0)        break;
        else printf("Неизвестная команда '%s' (см. 'help')\n", cmd);
    }

    if (g_db) mldb_free(g_db);
    printf("Выход из dbdemo.\n");
    sys_exit(0);
    return 0;
}
