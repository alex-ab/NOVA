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

#pragma once

#include "io.hpp"
#include "list.hpp"
#include "slab.hpp"

namespace Iommu {
    class Interface;
};

class Pci : public List<Pci>
{
    friend class Acpi_table_mcfg;

    private:
        mword  const        reg_base;
        uint16 const        rid;
        uint16 const        lev;
        Iommu::Interface *  iommu;

        static unsigned     bus_base;
        static Paddr        cfg_base;
        static size_t       cfg_size;

        static Pci *        list;
        static Slab_cache   cache;

        static struct quirk_map
        {
            uint16 vid, did;
            void (Pci::*func)();
        } map[];

        enum Register
        {
            REG_VID         = 0x0,
            REG_DID         = 0x2,
            REG_HDR         = 0xe,
            REG_SBUSN       = 0x19,
            REG_MAX         = 0xfff,
        };

        template <typename T>
        ALWAYS_INLINE
        inline unsigned read (Register r) { return *reinterpret_cast<T volatile *>(reg_base + r); }

        template <typename T>
        ALWAYS_INLINE
        inline void write (Register r, T v) { *reinterpret_cast<T volatile *>(reg_base + r) = v; }

        ALWAYS_INLINE
        inline uint16 readx (uint16 const c) { return *reinterpret_cast<uint16 volatile *>(reg_base + c); }

        template <typename T>
        ALWAYS_INLINE
        inline void writex (unsigned const c, T v) { *reinterpret_cast<T volatile *>(reg_base + c) = v; }

        ALWAYS_INLINE
        static inline Pci *find_dev (unsigned long r)
        {
            for (Pci *pci = list; pci; pci = pci->next)
                if (pci->rid == r)
                    return pci;

            return nullptr;
        }

    public:
        INIT
        Pci (unsigned, unsigned);

        ALWAYS_INLINE
        static inline void *operator new (size_t, Quota &quota) { return cache.alloc(quota); }

        ALWAYS_INLINE
        static inline void claim_all (Iommu::Interface *d)
        {
            for (Pci *pci = list; pci; pci = pci->next)
                if (!pci->iommu)
                    pci->iommu = d;
        }

        ALWAYS_INLINE
        static inline bool claim_dev (Iommu::Interface *d, unsigned r, bool single = false)
        {
            Pci *pci = find_dev (r);

            if (!pci)
                return false;

            unsigned l = pci->lev;
            do pci->iommu = d; while (!single && (pci = pci->next) && pci->lev > l);

            return true;
        }

        ALWAYS_INLINE
        static inline bool claim_dev_single (Iommu::Interface *d, unsigned r) {
            return claim_dev(d, r, true); }

        INIT
        static void init (unsigned = 0, unsigned = 0);

        ALWAYS_INLINE
        static inline unsigned phys_to_rid (Paddr p)
        {
            return p - cfg_base < cfg_size ? static_cast<unsigned>((bus_base << 8) + (p - cfg_base) / PAGE_SIZE (0)) : ~0U;
        }

        static Iommu::Interface *find_iommu (unsigned long);

        static void enable_msi(unsigned);
};
