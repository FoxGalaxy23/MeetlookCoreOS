/* acpi.c -- см. include/acpi.h за общим описанием и границами того, что
 * здесь (сознательно) НЕ разбирается. */

#include "acpi.h"
#include "string.h"

typedef struct __attribute__((packed)) {
    char sig[8];         /* "RSD PTR " (со вставленным пробелом, ровно 8 байт) */
    u8   checksum;
    char oem_id[6];
    u8   revision;        /* 0 -- ACPI 1.0 (только RSDT), >=2 -- ACPI 2.0+ (есть и XSDT) */
    u32  rsdt_addr;
    /* -- поля ниже существуют, только если revision >= 2 -- */
    u32  length;
    u64  xsdt_addr;
    u8   ext_checksum;
    u8   reserved[3];
} acpi_rsdp_t;

typedef struct __attribute__((packed)) {
    char sig[4];
    u32  length;
    u8   revision;
    u8   checksum;
    char oem_id[6];
    char oem_table_id[8];
    u32  oem_revision;
    u32  creator_id;
    u32  creator_revision;
} acpi_sdt_header_t;

typedef struct __attribute__((packed)) {
    acpi_sdt_header_t header;
    u32 local_apic_addr;
    u32 flags;
    /* далее -- записи переменной длины, см. acpi_madt_entry_header_t */
} acpi_madt_t;

typedef struct __attribute__((packed)) {
    u8 type;
    u8 length;
} acpi_madt_entry_header_t;

#define ACPI_MADT_TYPE_LOCAL_APIC 0u

typedef struct __attribute__((packed)) {
    acpi_madt_entry_header_t header;
    u8  acpi_processor_id;
    u8  apic_id;
    u32 flags; /* бит 0 -- Enabled (ACPI Spec §5.2.12.2) */
} acpi_madt_local_apic_entry_t;

#define ACPI_LOCAL_APIC_ENABLED (1u << 0)

static bool_t checksum_ok(const void *data, u32 len) {
    const u8 *p = (const u8 *)data;
    u8 sum = 0;
    for (u32 i = 0; i < len; i++) sum = (u8)(sum + p[i]);
    return sum == 0;
}

/* EBDA (Extended BIOS Data Area): сегмент указан 16-битным словом по
   фиксированному физическому адресу 0x40E (BIOS Data Area, см. любое
   описание карты памяти реального режима) -- физический адрес EBDA
   получается сдвигом этого сегмента на 4 бита влево, как обычный
   real-mode segment:offset с offset=0. */
static const acpi_rsdp_t *find_rsdp(void) {
    u16 ebda_seg = 0;
    memcpy(&ebda_seg, (const void *)(uintptr_t)0x40Eu, sizeof(ebda_seg));
    u64 ebda_addr = ((u64)ebda_seg) << 4;

    /* Область поиска 1: первый 1 KiB EBDA (ACPI Spec §5.2.5.1). Область
       поиска 2: BIOS ROM 0xE0000-0xFFFFF (тот же параграф) -- сигнатура
       всегда выровнена на границу 16 байт. Оба диапазона внутри первого
       1 MiB, который boot.s identity-мапит наравне со всем остальным
       (см. общий комментарий про identity map в include/pmm.h), так что
       обычное разыменование указателя работает без каких-либо
       дополнительных отображений. */
    u64 ranges[2][2] = {
        { ebda_addr, ebda_addr + 1024u },
        { 0xE0000u,  0x100000u },
    };

    for (u32 r = 0; r < 2; r++) {
        if (ranges[r][0] == 0) continue; /* EBDA не сообщён (ebda_seg == 0) -- пропускаем */
        for (u64 addr = ranges[r][0]; addr < ranges[r][1]; addr += 16) {
            const acpi_rsdp_t *candidate = (const acpi_rsdp_t *)(uintptr_t)addr;
            if (memcmp(candidate->sig, "RSD PTR ", 8) == 0) {
                /* Контрольная сумма ACPI 1.0-части (первые 20 байт)
                   обязана сходиться независимо от версии -- у ACPI 2.0+
                   есть ЕЩЁ и отдельная extended-контрольная сумма на всю
                   структуру, но нам для одного лишь RSDT/XSDT-адреса
                   достаточно проверить только первую. */
                if (checksum_ok(candidate, 20)) {
                    return candidate;
                }
            }
        }
    }
    return 0;
}

static void process_madt(const acpi_madt_t *madt, acpi_cpu_info_t *out_cpus, u32 max_cpus, u32 *count) {
    const u8 *end = (const u8 *)madt + madt->header.length;
    const u8 *p = (const u8 *)(madt + 1);

    while (p + sizeof(acpi_madt_entry_header_t) <= end) {
        const acpi_madt_entry_header_t *eh = (const acpi_madt_entry_header_t *)p;
        if (eh->length < sizeof(acpi_madt_entry_header_t)) break; /* защита от битой/зацикленной таблицы */

        if (eh->type == ACPI_MADT_TYPE_LOCAL_APIC && *count < max_cpus) {
            const acpi_madt_local_apic_entry_t *e = (const acpi_madt_local_apic_entry_t *)p;
            out_cpus[*count].apic_id = e->apic_id;
            out_cpus[*count].enabled = (e->flags & ACPI_LOCAL_APIC_ENABLED) != 0;
            (*count)++;
        }

        p += eh->length;
    }
}

u32 acpi_find_cpus(acpi_cpu_info_t *out_cpus, u32 max_cpus) {
    const acpi_rsdp_t *rsdp = find_rsdp();
    if (!rsdp) return 0;

    const acpi_sdt_header_t *root = 0;
    bool_t is_xsdt = FALSE;

    if (rsdp->revision >= 2 && rsdp->xsdt_addr != 0) {
        root = (const acpi_sdt_header_t *)(uintptr_t)rsdp->xsdt_addr;
        is_xsdt = TRUE;
    } else {
        root = (const acpi_sdt_header_t *)(uintptr_t)rsdp->rsdt_addr;
        is_xsdt = FALSE;
    }
    if (!root) return 0;
    if (!checksum_ok(root, root->length)) return 0;

    u32 entry_count = (root->length - sizeof(acpi_sdt_header_t)) / (is_xsdt ? 8u : 4u);
    const u8 *entries = (const u8 *)(root + 1);

    u32 count = 0;
    for (u32 i = 0; i < entry_count; i++) {
        u64 table_addr;
        if (is_xsdt) {
            table_addr = ((const u64 *)entries)[i];
        } else {
            table_addr = ((const u32 *)entries)[i];
        }
        if (table_addr == 0) continue;

        const acpi_sdt_header_t *table = (const acpi_sdt_header_t *)(uintptr_t)table_addr;
        if (memcmp(table->sig, "APIC", 4) == 0 && checksum_ok(table, table->length)) {
            process_madt((const acpi_madt_t *)table, out_cpus, max_cpus, &count);
            break; /* в системе ровно одна MADT -- дальше искать нечего */
        }
    }

    return count;
}
