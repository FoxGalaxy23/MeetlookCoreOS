#ifndef STRING_H
#define STRING_H

#include "types.h"

u32 strlen(const char *s);
int strcmp(const char *a, const char *b);
int strncmp(const char *a, const char *b, u32 n);
char *strcpy(char *dst, const char *src);
char *strncpy(char *dst, const char *src, u32 n);
char *strcat(char *dst, const char *src);
void *memset(void *dst, int val, u32 n);
void *memcpy(void *dst, const void *src, u32 n);
void *memmove(void *dst, const void *src, u32 n);
int memcmp(const void *a, const void *b, u32 n);
char *strchr(const char *s, int c);
char to_upper(char c);
char to_lower(char c);
int str_starts_with(const char *s, const char *prefix);
void itoa(int value, char *buf, int base);
void utoa(u32 value, char *buf, int base);
int atoi(const char *s);
void trim(char *s);
u32 split_ws(char *s, char **argv, u32 max_args);

#endif
