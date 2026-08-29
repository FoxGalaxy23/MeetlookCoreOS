#ifndef KEYBOARD_H
#define KEYBOARD_H

#include "types.h"

void keyboard_init(void);
char keyboard_getchar(void);   /* блокирующее чтение символа (poll) */
bool_t keyboard_haskey(void);

#endif
