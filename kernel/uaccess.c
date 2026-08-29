/* uaccess.c -- см. include/uaccess.h. Реальная проверка диапазона
 * теперь целиком делегирована адресному пространству текущей задачи
 * (см. include/paging.h: as_range_is_user_accessible()) -- здесь только
 * тонкая обёртка, получающая это адресное пространство через
 * task_current(). */

#include "uaccess.h"
#include "paging.h"
#include "task.h"

bool_t uaccess_check(const void *ptr, u32 len) {
    if (len == 0) return TRUE;

    task_t *cur = task_current();
    if (!cur || !cur->as) return FALSE;

    return as_range_is_user_accessible(cur->as, (u64)(uintptr_t)ptr, len);
}
