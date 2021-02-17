/*
 * PCI Configuration Space
 *
 * Copyright (C) 2009-2011 Udo Steinberg <udo@hypervisor.org>
 * Economic rights: Technische Universitaet Dresden (Germany)
 *
 * Copyright (C) 2012-2013 Udo Steinberg, Intel Corporation.
 * Copyright (C) 2019-2024 Udo Steinberg, BedRock Systems, Inc.
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
#include "memory.hpp"
#include "slab.hpp"
#include "std.hpp"

class Smmu;

class Pci : public List<Pci>
{
    friend class Acpi_table_mcfg;

    private:
        mword  const        reg_base;
        uint16 const        rid;
        uint16 const        lev;
        Smmu *              smmu;

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
        static inline Pci *find_dev (unsigned long r)
        {
            for (Pci *pci = list; pci; pci = pci->next)
                if (pci->rid == r)
                    return pci;

            return nullptr;
        }

        static constexpr auto seg_shft { 16 };
        static constexpr auto bus_shft {  8 };
        static constexpr auto dev_shft {  3 };

    public:
        static constexpr auto pci (uint16_t s, uint8_t b, uint8_t d, uint8_t f) { return static_cast<pci_t>(s << seg_shft | b << bus_shft | d << dev_shft | f); }
        static constexpr auto pci (            uint8_t b, uint8_t d, uint8_t f) { return static_cast<pci_t>(                b << bus_shft | d << dev_shft | f); }

        static constexpr auto seg (pci_t p) { return static_cast<uint16_t>(p >> seg_shft); }
        static constexpr auto bdf (pci_t p) { return static_cast<uint16_t>(p); }
        static constexpr auto bus (pci_t p) { return static_cast<uint8_t> (p >> bus_shft); }
        static constexpr auto ari (pci_t p) { return static_cast<uint8_t> (p); }
        static constexpr auto dev (pci_t p) { return static_cast<uint8_t> (p >> dev_shft & BIT_RANGE (4, 0)); }
        static constexpr auto fun (pci_t p) { return static_cast<uint8_t> (p             & BIT_RANGE (2, 0)); }

        Pci (unsigned, unsigned);

        ALWAYS_INLINE
        static inline void *operator new (size_t) { return cache.alloc(); }

        ALWAYS_INLINE
        static inline void claim_all (Smmu *s)
        {
            for (Pci *pci = list; pci; pci = pci->next)
                if (!pci->smmu)
                    pci->smmu = s;
        }

        ALWAYS_INLINE
        static inline bool claim_dev (Smmu *s, unsigned r)
        {
            Pci *pci = find_dev (r);

            if (!pci)
                return false;

            unsigned l = pci->lev;
            do pci->smmu = s; while ((pci = pci->next) && pci->lev > l);

            return true;
        }

        static void init (unsigned = 0, unsigned = 0);

        ALWAYS_INLINE
        static inline unsigned phys_to_rid (Paddr p)
        {
            return p - cfg_base < cfg_size ? static_cast<unsigned>((bus_base << 8) + (p - cfg_base) / PAGE_SIZE (0)) : ~0U;
        }

        ALWAYS_INLINE
        static inline Smmu *find_smmu (unsigned long r)
        {
            Pci *pci = find_dev (r);

            return pci ? pci->smmu : nullptr;
        }
};
