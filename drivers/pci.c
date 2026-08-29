#include "pci.h"
#include "io.h"

#define PCI_CONFIG_ADDRESS 0xCF8
#define PCI_CONFIG_DATA    0xCFC

static u32 pci_make_address(u8 bus, u8 device, u8 function, u8 offset) {
    return (1u << 31)
         | ((u32)bus << 16)
         | ((u32)device << 11)
         | ((u32)function << 8)
         | ((u32)offset & 0xFC);
}

u32 pci_config_read32(u8 bus, u8 device, u8 function, u8 offset) {
    outl(PCI_CONFIG_ADDRESS, pci_make_address(bus, device, function, offset));
    return inl(PCI_CONFIG_DATA);
}

void pci_config_write32(u8 bus, u8 device, u8 function, u8 offset, u32 value) {
    outl(PCI_CONFIG_ADDRESS, pci_make_address(bus, device, function, offset));
    outl(PCI_CONFIG_DATA, value);
}

u16 pci_config_read16(u8 bus, u8 device, u8 function, u8 offset) {
    u32 dword = pci_config_read32(bus, device, function, offset & 0xFC);
    return (u16)((dword >> ((offset & 2) * 8)) & 0xFFFF);
}

void pci_config_write16(u8 bus, u8 device, u8 function, u8 offset, u16 value) {
    u32 dword = pci_config_read32(bus, device, function, offset & 0xFC);
    u32 shift = (offset & 2) * 8;
    dword &= ~(0xFFFFu << shift);
    dword |= ((u32)value) << shift;
    pci_config_write32(bus, device, function, offset & 0xFC, dword);
}

static bool_t probe_function(u8 bus, u8 device, u8 function,
                              u8 class_code, u8 subclass, u8 prog_if,
                              pci_device_t *out) {
    u32 id = pci_config_read32(bus, device, function, 0x00);
    u16 vendor_id = (u16)(id & 0xFFFF);
    if (vendor_id == 0xFFFF) return FALSE; /* нет устройства */

    u32 class_reg = pci_config_read32(bus, device, function, 0x08);
    u8 dev_class    = (u8)((class_reg >> 24) & 0xFF);
    u8 dev_subclass = (u8)((class_reg >> 16) & 0xFF);
    u8 dev_prog_if  = (u8)((class_reg >> 8) & 0xFF);

    if (dev_class != class_code || dev_subclass != subclass) return FALSE;
    if (prog_if != 0 && dev_prog_if != prog_if) return FALSE;

    out->bus = bus;
    out->device = device;
    out->function = function;
    out->vendor_id = vendor_id;
    out->device_id = (u16)((id >> 16) & 0xFFFF);
    out->class_code = dev_class;
    out->subclass = dev_subclass;
    out->prog_if = dev_prog_if;
    for (int i = 0; i < 6; i++) {
        out->bar[i] = pci_config_read32(bus, device, function, 0x10 + i * 4);
    }
    return TRUE;
}

bool_t pci_find_device_by_id(u16 vendor_id, u16 device_id, pci_device_t *out) {
    /* Тот же обход шины 0, что и pci_find_device() ниже, но по
       vendor/device id, а не по классу устройства -- probe_function()
       заточен под "класс важен, id -- нет", здесь наоборот, поэтому
       свой маленький цикл, а не общий хелпер на оба случая сразу. */
    for (u16 device = 0; device < 32; device++) {
        u32 id = pci_config_read32(0, (u8)device, 0, 0x00);
        if ((id & 0xFFFF) == 0xFFFF) continue;

        u8 header_type = (u8)((pci_config_read32(0, (u8)device, 0, 0x0C) >> 16) & 0xFF);
        bool_t multifunction = (header_type & 0x80) != 0;
        u8 max_function = multifunction ? 8 : 1;

        for (u8 function = 0; function < max_function; function++) {
            u32 fid = pci_config_read32(0, (u8)device, function, 0x00);
            u16 vendor = (u16)(fid & 0xFFFF);
            if (vendor == 0xFFFF) continue;
            u16 dev = (u16)((fid >> 16) & 0xFFFF);
            if (vendor != vendor_id || dev != device_id) continue;

            out->bus = 0;
            out->device = (u8)device;
            out->function = function;
            out->vendor_id = vendor;
            out->device_id = dev;
            u32 class_reg = pci_config_read32(0, (u8)device, function, 0x08);
            out->class_code = (u8)((class_reg >> 24) & 0xFF);
            out->subclass   = (u8)((class_reg >> 16) & 0xFF);
            out->prog_if    = (u8)((class_reg >> 8) & 0xFF);
            for (int i = 0; i < 6; i++) {
                out->bar[i] = pci_config_read32(0, (u8)device, function, 0x10 + i * 4);
            }
            return TRUE;
        }
    }
    return FALSE;
}

bool_t pci_find_device(u8 class_code, u8 subclass, u8 prog_if, pci_device_t *out) {
    /* Простой полный перебор шины 0 (мостов на вторичные шины не
       обходим -- для контроллера AHCI на стандартных платформах QEMU/
       большинства ПК этого достаточно, он всегда сидит на шине 0). */
    for (u16 device = 0; device < 32; device++) {
        u32 id = pci_config_read32(0, (u8)device, 0, 0x00);
        if ((id & 0xFFFF) == 0xFFFF) continue;

        u8 header_type = (u8)((pci_config_read32(0, (u8)device, 0, 0x0C) >> 16) & 0xFF);
        bool_t multifunction = (header_type & 0x80) != 0;
        u8 max_function = multifunction ? 8 : 1;

        for (u8 function = 0; function < max_function; function++) {
            if (probe_function(0, (u8)device, function, class_code, subclass, prog_if, out)) {
                return TRUE;
            }
        }
    }
    return FALSE;
}
