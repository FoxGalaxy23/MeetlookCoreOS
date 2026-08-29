#include "crypto_rng.h"

/* См. include/crypto_rng.h. */

static bool_t g_rdrand_available = FALSE;
static bool_t g_initialized = FALSE;

/* Intel SDM, Vol 2A: RDRAND retry loop должен быть ограничен --
 * рекомендация Intel "RDRAND: Retry recommendation" -- 10 попыток
 * с backoff практически никогда не срабатывает, DRNG не готов только
 * в исключительных случаях (несколько ядер одновременно исчерпывают
 * общий генератор). 10 -- значение из этой рекомендации Intel. */
#define RDRAND_MAX_RETRIES 10u

bool_t crypto_rng_init(void) {
    u32 eax, ebx, ecx, edx;

    /* CPUID.1:ECX.RDRAND = bit 30. */
    __asm__ volatile ("cpuid"
                       : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
                       : "a"(1u), "c"(0u));
    g_rdrand_available = (ecx & (1u << 30)) != 0;

    g_initialized = TRUE;
    return g_rdrand_available;
}

bool_t crypto_rng_available(void) {
    return g_initialized && g_rdrand_available;
}

/* Один вызов RDRAND -> 64-битное слово. Возвращает FALSE, если DRNG
 * не отдал готовое значение за RDRAND_MAX_RETRIES попыток (CF=0 --
 * "не готово", это штатная, документированная ситуация, не ошибка
 * инструкции). */
static bool_t rdrand64(u64 *out) {
    u8 ok;
    u32 tries;

    for (tries = 0; tries < RDRAND_MAX_RETRIES; tries++) {
        __asm__ volatile ("rdrand %0; setc %1"
                           : "=r"(*out), "=qm"(ok)
                           :
                           : "cc");
        if (ok) return TRUE;
    }
    return FALSE;
}

bool_t crypto_rng_bytes(u8 *buf, u32 len) {
    u64 word;
    u32 i;

    if (!g_initialized || !g_rdrand_available) return FALSE;

    i = 0;
    while (i < len) {
        if (!rdrand64(&word)) return FALSE;

        u32 chunk = (len - i) < 8u ? (len - i) : 8u;
        u32 k;
        for (k = 0; k < chunk; k++) {
            buf[i + k] = (u8)(word >> (8u * k));
        }
        i += chunk;
    }
    return TRUE;
}
