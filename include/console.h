#ifndef CONSOLE_H
#define CONSOLE_H

#include "types.h"

/* Читает строку с клавиатуры с поддержкой backspace, выводит эхо на экран.
   Возвращает длину строки (без \n). */
u32 console_readline(char *buf, u32 maxlen);

#endif
