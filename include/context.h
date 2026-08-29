#ifndef CONTEXT_H
#define CONTEXT_H

#include "types.h"

/* context.h -- объявления для kernel/context.s.
 *
 * context_switch сохраняет callee-saved регистры текущей задачи на её
 * собственном стеке, записывает получившийся rsp в *old_rsp, переходит
 * на стек new_rsp и восстанавливает с него callee-saved регистры новой
 * задачи, после чего "ret" передаёт управление туда, куда указывает
 * значение на вершине нового стека -- либо обратно в код, который в
 * прошлый раз вызвал context_switch() для этой задачи (обычная
 * ситуация при вытеснении), либо в task_trampoline(), если задача
 * запускается впервые (см. kernel/task.c: task_create()).
 *
 * Особо важное свойство: эта функция НЕ трогает rax/rcx/rdx/rsi/rdi/
 * r8-r11 (caller-saved) -- ей не нужно, вызывающий код (schedule() в
 * task.c) не рассчитывает на них после вызова, как и с любой обычной
 * функцией по System V AMD64 ABI. */
void context_switch(u64 *old_rsp, u64 new_rsp);

/* Точка входа новой задачи. Ожидает, что context_switch() только что
   восстановил в r12 указатель на entry-функцию, а в r13 -- аргумент
   для неё (так их туда кладёт task_create(), см. kernel/task.c) --
   обычному коду вызывать эту функцию незачем. */
void task_trampoline(void);

#endif
