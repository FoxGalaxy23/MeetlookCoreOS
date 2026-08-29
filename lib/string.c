#include "string.h"

u32 strlen(const char *s) {
    u32 n = 0;
    while (s[n]) n++;
    return n;
}

int strcmp(const char *a, const char *b) {
    while (*a && (*a == *b)) { a++; b++; }
    return (unsigned char)*a - (unsigned char)*b;
}

int strncmp(const char *a, const char *b, u32 n) {
    while (n && *a && (*a == *b)) { a++; b++; n--; }
    if (n == 0) return 0;
    return (unsigned char)*a - (unsigned char)*b;
}

char *strcpy(char *dst, const char *src) {
    char *ret = dst;
    while ((*dst++ = *src++));
    return ret;
}

char *strncpy(char *dst, const char *src, u32 n) {
    u32 i = 0;
    for (; i < n && src[i]; i++) dst[i] = src[i];
    for (; i < n; i++) dst[i] = 0;
    return dst;
}

char *strcat(char *dst, const char *src) {
    char *ret = dst;
    while (*dst) dst++;
    while ((*dst++ = *src++));
    return ret;
}

void *memset(void *dst, int val, u32 n) {
    u8 *p = (u8*)dst;
    for (u32 i = 0; i < n; i++) p[i] = (u8)val;
    return dst;
}

void *memcpy(void *dst, const void *src, u32 n) {
    u8 *d = (u8*)dst;
    const u8 *s = (const u8*)src;
    for (u32 i = 0; i < n; i++) d[i] = s[i];
    return dst;
}

/* В отличие от memcpy(), корректно работает и при пересекающихся
   dst/src: копирует с конца, если dst лежит после src (иначе можно было
   бы затереть ещё не скопированные байты источника). */
void *memmove(void *dst, const void *src, u32 n) {
    u8 *d = (u8*)dst;
    const u8 *s = (const u8*)src;
    if (d == s || n == 0) return dst;
    if (d < s) {
        for (u32 i = 0; i < n; i++) d[i] = s[i];
    } else {
        for (u32 i = n; i > 0; i--) d[i - 1] = s[i - 1];
    }
    return dst;
}

int memcmp(const void *a, const void *b, u32 n) {
    const u8 *pa = (const u8*)a, *pb = (const u8*)b;
    for (u32 i = 0; i < n; i++) {
        if (pa[i] != pb[i]) return pa[i] - pb[i];
    }
    return 0;
}

char *strchr(const char *s, int c) {
    while (*s) {
        if (*s == (char)c) return (char*)s;
        s++;
    }
    return (c == 0) ? (char*)s : NULL;
}

char to_upper(char c) {
    if (c >= 'a' && c <= 'z') return c - 32;
    return c;
}

char to_lower(char c) {
    if (c >= 'A' && c <= 'Z') return c + 32;
    return c;
}

int str_starts_with(const char *s, const char *prefix) {
    while (*prefix) {
        if (*s != *prefix) return 0;
        s++; prefix++;
    }
    return 1;
}

void itoa(int value, char *buf, int base) {
    char tmp[34];
    int i = 0;
    u32 uval;
    bool_t neg = FALSE;

    if (value < 0 && base == 10) {
        neg = TRUE;
        uval = (u32)(-value);
    } else {
        uval = (u32)value;
    }

    if (uval == 0) tmp[i++] = '0';
    while (uval) {
        int d = uval % base;
        tmp[i++] = (d < 10) ? ('0' + d) : ('a' + d - 10);
        uval /= base;
    }
    if (neg) tmp[i++] = '-';

    int j = 0;
    while (i > 0) buf[j++] = tmp[--i];
    buf[j] = '\0';
}

void utoa(u32 value, char *buf, int base) {
    char tmp[34];
    int i = 0;
    if (value == 0) tmp[i++] = '0';
    while (value) {
        int d = value % base;
        tmp[i++] = (d < 10) ? ('0' + d) : ('a' + d - 10);
        value /= base;
    }
    int j = 0;
    while (i > 0) buf[j++] = tmp[--i];
    buf[j] = '\0';
}

int atoi(const char *s) {
    int res = 0;
    int sign = 1;
    if (*s == '-') { sign = -1; s++; }
    while (*s >= '0' && *s <= '9') {
        res = res * 10 + (*s - '0');
        s++;
    }
    return res * sign;
}

void trim(char *s) {
    u32 len = strlen(s);
    u32 start = 0;
    while (s[start] == ' ' || s[start] == '\t') start++;
    u32 end = len;
    while (end > start && (s[end-1] == ' ' || s[end-1] == '\t' || s[end-1] == '\n' || s[end-1] == '\r')) end--;
    u32 j = 0;
    for (u32 i = start; i < end; i++) s[j++] = s[i];
    s[j] = '\0';
}

u32 split_ws(char *s, char **argv, u32 max_args) {
    u32 argc = 0;
    char *p = s;
    while (*p && argc < max_args) {
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) break;
        argv[argc++] = p;
        while (*p && *p != ' ' && *p != '\t') p++;
        if (*p) { *p = '\0'; p++; }
    }
    return argc;
}
