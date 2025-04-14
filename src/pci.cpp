/*
 * PCI Configuration Space
 *
 * Copyright (C) 2009-2011 Udo Steinberg <udo@hypervisor.org>
 * Economic rights: Technische Universitaet Dresden (Germany)
 *
 * Copyright (C) 2012-2013 Udo Steinberg, Intel Corporation.
 *
 * This file is part of the NOVA microhypervisor.
 *
 * NOVA is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * NOVA is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License version 2 for more details.
 */

#include "pci.hpp"
#include "pd.hpp"
#include "iommu.hpp"
#include "vectors.hpp"


INIT_PRIORITY (PRIO_SLAB)
Slab_cache Pci::cache (sizeof (Pci), 8);

unsigned    Pci::bus_base;
Paddr       Pci::cfg_base;
size_t      Pci::cfg_size;
Pci *       Pci::list;

struct Pci::quirk_map Pci::map[] INITDATA =
{
};

Pci::Pci (unsigned r, unsigned l) : List<Pci> (list), reg_base (hwdev_addr -= PAGE_SIZE), rid (static_cast<uint16>(r)), lev (static_cast<uint16>(l)), iommu(nullptr)
{
    Pd::kern.Space_mem::insert (Pd::kern.quota, reg_base, 0, Hpt::HPT_NX | Hpt::HPT_G | Hpt::HPT_UC | Hpt::HPT_W | Hpt::HPT_P, cfg_base + (rid << PAGE_BITS));

    for (unsigned i = 0; i < sizeof map / sizeof *map; i++)
        if (read<uint16>(REG_VID) == map[i].vid && read<uint16>(REG_DID) == map[i].did)
            (this->*map[i].func)();
}

void Pci::init (unsigned b, unsigned l)
{
    for (unsigned r = b << 8; r < (b + 1) << 8; r++) {

        if (*static_cast<uint32 *>(Hpt::remap (Pd::kern.quota, cfg_base + (r << PAGE_BITS))) == ~0U)
            continue;

        Pci *p = new (Pd::kern.quota) Pci (r, l);

        unsigned h = p->read<uint8>(REG_HDR);

        if ((h & 0x7f) == 1)
            init (p->read<uint8>(REG_SBUSN), l + 1);

        if (!(r & 0x7) && !(h & 0x80))
            r += 7;
    }
}

Iommu::Interface *Pci::find_iommu (unsigned long r)
{
    Pci *pci = find_dev (r);

    return pci ? pci->iommu : nullptr;
}

void Pci::enable_msi(unsigned const rid)
{
    Pci *pci = find_dev (rid);

    if (!pci) return;

    uint16 cap = pci->readx(0x34) & 0xff;
    for (uint16 val = 0; cap; cap = (val >> 8) & 0xff) {
        val = pci->readx(cap);
        if ((val & 0xff) != 0x5) continue;

        uint16 msi_val = pci->readx(uint16(cap + 2));
        bool   const msi64   = msi_val & 0x80;

        pci->writex(cap + 0x4, uint32(0xfee00000) | uint32(Cpu::apic_id[0]) << 12);
        if (msi64) {
            pci->writex(cap + 0x8, uint32(0));
            pci->writex(cap + 0xc, short(VEC_MSI_DMAR));
        } else
            pci->writex(cap + 0x8, short(VEC_MSI_DMAR));

        pci->writex(cap + 2, msi_val | 0x1);
        msi_val = pci->readx(uint16(cap + 2));
        if (msi_val & 0x1)
            return;
        else
            break;
    }

    Console::print("Enabling MSI for %x:%x.%x failed",
                   (rid >> 8) & 0xff, (rid >> 3) & 0x1f, rid & 0x7);
}
