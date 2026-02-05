/*
 * Hypervisor Information Page (HIP)
 *
 * Copyright (C) 2009-2011 Udo Steinberg <udo@hypervisor.org>
 * Economic rights: Technische Universitaet Dresden (Germany)
 *
 * Copyright (C) 2012-2013 Udo Steinberg, Intel Corporation.
 * Copyright (C) 2014 Udo Steinberg, FireEye, Inc.
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

#include "atomic.hpp"
#include "config.hpp"
#include "extern.hpp"

class Hip_cpu
{
    public:
        uint8   flags;
        uint8   thread;
        uint8   core;
        uint8   package;
        uint8   acpi_id;
        uint8   family;
        uint8   model;
        uint8   stepping:4;
        uint8   platform:3;
        uint8   reserved:1;
        uint32  patch;
} PACKED;

class Hip_mem
{
    public:
        enum {
            HYPERVISOR  = -1u,
            MB_MODULE   = -2u,
            ACPI_RSDT   = -3u,
            ACPI_XSDT   = -4u,
            MB2_FB      = -5u,
            HYP_LOG     = -6u,
            SYSTAB      = -7u
        };

        uint64  addr;
        uint64  size;
        uint32  type;
        uint32  aux;
};

class Hip_guard;

class Hip
{
    friend class Hip_guard;

    private:
        uint32  signature;              // 0x0
        uint16  checksum;               // 0x4
        uint16  length;                 // 0x6
        uint16  cpu_offs;               // 0x8
        uint16  cpu_size;               // 0xa
        uint16  mem_offs;               // 0xc
        uint16  mem_size;               // 0xe
        uint32  api_flg;                // 0x10
        uint32  api_ver;                // 0x14
        uint32  sel_num;                // 0x18
        uint32  sel_exc;                // 0x1c
        uint32  sel_vmi;                // 0x20
        uint32  sel_gsi;                // 0x24
        uint32  cfg_page;               // 0x28
        uint32  cfg_utcb;               // 0x2c
        uint32  freq_tsc;               // 0x30
        uint16  tme_kmax;               // 0x34
        uint16  tme_algo;               // 0x36
        Hip_cpu cpu_desc[NUM_CPU];
        Hip_mem mem_desc[];

        ALWAYS_INLINE
        static inline Hip *hip()
        {
            return reinterpret_cast<Hip *>(&PAGE_H);
        }

        Hip(const Hip&);
        Hip &operator = (Hip const &);

        INIT
        static void for_each (Hip &hip, auto const &fn)
        {
            mword const mhv_cnt = (reinterpret_cast<mword>(&hip) + hip.length - reinterpret_cast<mword>(hip.mem_desc)) / sizeof(Hip_mem);

            for (unsigned i = 0; i < mhv_cnt; i++) {
                Hip_mem & m = *(hip.mem_desc + i);

                fn (m);
            }
        }

    public:
        enum Feature {
            FEAT_IOMMU  = 1U << 0,
            FEAT_VMX    = 1U << 1,
            FEAT_SVM    = 1U << 2,
        };

        static mword root_addr;
        static mword root_size;

        static uint32 feature()
        {
            return hip()->api_flg;
        }

        static void set_feature (Feature f)
        {
            Atomic_legacy::set_mask (hip()->api_flg, static_cast<typeof hip()->api_flg>(f));
        }

        static void clr_feature (Feature f)
        {
            Atomic_legacy::clr_mask (hip()->api_flg, static_cast<typeof hip()->api_flg>(f));
        }

        static bool cpu_online (unsigned long cpu)
        {
            return cpu < NUM_CPU && hip()->cpu_desc[cpu].flags & 1;
        }

        static void tme(uint16_t keys, uint16_t algo)
        {
            hip()->tme_kmax = keys;
            hip()->tme_algo = algo;
        }

        INIT
        static bool build (mword, mword);

        INIT
        static void build_mbi1 (Hip_guard &, mword);

        INIT
        static void build_mbi2 (Hip_guard &, mword);
        
        INIT
        static void add_systab (Hip_guard &, auto const *);

        INIT
        static void add_mem (Hip_guard &, auto const *);

        INIT
        static void add_mod (Hip_guard &, auto const *, uint32);

        INIT
        static void add_mhv (Hip_guard &);

        INIT
        static void add_buddy (Hip_guard &, uint64 const, uint64 &, bool);

        INIT
        static void _add_buddy (Hip_guard &, uint64 const, uint64 &, Hip_mem const &);

        INIT
        static uint64 system_memory (Hip_guard &);

        static void add_cpu();
        static void add_check();
};

class Hip_guard
{

    private:

        Hip & hip;

        bool fail { };

    public:

        Hip_guard() : hip(*Hip::hip())
        {
            static_assert (PAGE_H_SIZE <= (1u << (sizeof(hip.length) * 8)), "HIP length field too small");

            if (!hip.length)
                hip.length = static_cast<uint16>(reinterpret_cast<mword>(hip.mem_desc) - reinterpret_cast<mword>(&hip));
        }

        void with_mem_desc(auto const &fn)
        {
            auto const mem_desc_size = sizeof (hip.mem_desc[0]);

            auto const i = (hip.length + reinterpret_cast<mword>(&hip) - reinterpret_cast<mword>(hip.mem_desc)) / mem_desc_size;

            if (mword(hip.length) + mem_desc_size > PAGE_H_SIZE) {
                fail = true;
                return;
            }

            fn (hip.mem_desc[i]);

            hip.length += mem_desc_size;
        }

        void for_each(auto const &fn) { hip.for_each(hip, fn); }
        void with_hip(auto const &fn) { fn(hip); }

        bool ready() const { return !fail; }
};
