/* spinlock.c -- см. include/spinlock.h за общим описанием модели. */

#include "spinlock.h"

void spinlock_init(spinlock_t *lock) {
    lock->locked = 0;
}

/* __atomic_exchange_n(..., __ATOMIC_ACQUIRE) компилируется в "lock xchg"
   (xchg с памятью на x86 уже неявно атомарен и имеет полный барьер, но
   явный ACQUIRE говорит компилятору не переставлять последующие чтения/
   записи ДО этой точки -- иначе он вправе был бы, например, заранее
   загрузить что-то из защищаемых локом данных ещё ДО того, как лок
   реально захвачен). "pause" в цикле ожидания -- стандартная подсказка
   процессору при спин-ожидании (Intel SDM Vol.3 §8.10.6.1): снижает
   энергопотребление и, что важнее для нас на гипертрединге/будущих
   ядрах, ускоряет момент, когда другое логическое ядро увидит
   освобождение лока (снимает лишнюю нагрузку с предсказателя переходов
   и буфера упорядочивания памяти при плотном опросе одного адреса). */
void spin_lock(spinlock_t *lock) {
    while (__atomic_exchange_n(&lock->locked, 1u, __ATOMIC_ACQUIRE) != 0) {
        while (__atomic_load_n(&lock->locked, __ATOMIC_RELAXED) != 0) {
            __asm__ volatile ("pause" ::: "memory");
        }
    }
}

bool_t spin_trylock(spinlock_t *lock) {
    return __atomic_exchange_n(&lock->locked, 1u, __ATOMIC_ACQUIRE) == 0;
}

void spin_unlock(spinlock_t *lock) {
    __atomic_store_n(&lock->locked, 0u, __ATOMIC_RELEASE);
}

static inline u64 local_irq_save(void) {
    u64 flags;
    __asm__ volatile ("pushfq; pop %0; cli" : "=r"(flags) : : "memory");
    return flags;
}

static inline void local_irq_restore(u64 flags) {
    __asm__ volatile ("push %0; popfq" : : "r"(flags) : "memory", "cc");
}

u64 spin_lock_irqsave(spinlock_t *lock) {
    /* Порядок принципиален: СНАЧАЛА cli (защита от самих себя -- иначе
       таймерный тик на этом же CPU мог бы прерваться прямо между
       "нашли лок свободным" и "пометили занятым" и сам захотеть тот же
       лок, см. общий комментарий в spinlock.h), ПОТОМ спин на локе
       (защита от других CPU). Если поменять местами, окно между двумя
       шагами оставляло бы ту самую однопроцессорную гонку, ради
       устранения которой был написан kernel/sync.c. */
    u64 flags = local_irq_save();
    spin_lock(lock);
    return flags;
}

void spin_unlock_irqrestore(spinlock_t *lock, u64 flags) {
    spin_unlock(lock);
    local_irq_restore(flags);
}
