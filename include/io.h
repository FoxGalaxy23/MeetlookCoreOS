#ifndef IO_H
#define IO_H

#include "types.h"

static inline void outb(u16 port, u8 val) {
    __asm__ volatile ("outb %0, %1" : : "a"(val), "Nd"(port));
}

static inline u8 inb(u16 port) {
    u8 ret;
    __asm__ volatile ("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

static inline void outw(u16 port, u16 val) {
    __asm__ volatile ("outw %0, %1" : : "a"(val), "Nd"(port));
}

static inline u16 inw(u16 port) {
    u16 ret;
    __asm__ volatile ("inw %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

static inline void insw(u16 port, void *addr, u32 count) {
    __asm__ volatile ("rep insw" : "+D"(addr), "+c"(count) : "d"(port) : "memory");
}

static inline void outsw(u16 port, const void *addr, u32 count) {
    __asm__ volatile ("rep outsw" : "+S"(addr), "+c"(count) : "d"(port));
}

static inline void outl(u16 port, u32 val) {
    __asm__ volatile ("outl %0, %1" : : "a"(val), "Nd"(port));
}

static inline u32 inl(u16 port) {
    u32 ret;
    __asm__ volatile ("inl %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

static inline void io_wait(void) {
    outb(0x80, 0);
}

/* ---- Доступ к memory-mapped регистрам устройств (MMIO) ----
   Используется драйверами, работающими через BAR PCI-устройств
   (например AHCI), а не через порты ввода-вывода. Поскольку ядро
   identity-map'ит физическую память 1:1 на виртуальную, "addr" здесь --
   это одновременно и физический, и виртуальный адрес. volatile обязателен,
   иначе компилятор может убрать или переупорядочить обращения к регистрам
   устройства как "бесполезные". */
static inline u32 mmio_read32(u32 addr) {
    return *(volatile u32 *)(u64)addr;
}

static inline void mmio_write32(u32 addr, u32 val) {
    *(volatile u32 *)(u64)addr = val;
}

#endif
