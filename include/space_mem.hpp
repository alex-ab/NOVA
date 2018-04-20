/*
 * Memory Space
 *
 * Copyright (C) 2009-2011 Udo Steinberg <udo@hypervisor.org>
 * Economic rights: Technische Universitaet Dresden (Germany)
 *
 * Copyright (C) 2012 Udo Steinberg, Intel Corporation.
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

#include "config.hpp"
#include "cpu.hpp"
#include "cpuset.hpp"
#include "dpt.hpp"
#include "ept.hpp"
#include "hpt.hpp"
#include "space.hpp"

class Pd;

class Space_mem : public Space
{
    public:
        Hpt loc[NUM_CPU];
        Hpt hpt;
        Dpt dpt;
        union {
            Ept ept;
            Hpt npt;
        };

        mword did;

        Cpuset cpus;
        Cpuset htlb;
        Cpuset gtlb;

        static mword did_c [4096 / 8 / sizeof(mword)];
        static mword did_f;

        enum { LAST_PCID = sizeof(Space_mem::did_c) / sizeof(Space_mem::did_c [0]) - 1 };
        enum { NO_PCID = 2 };

        ALWAYS_INLINE
        static inline void boot_init()
        {
            bool res = !Atomic::test_set_bit (did_c[0], NO_PCID);
            assert (res);
        }

        ALWAYS_INLINE
        inline Space_mem() : cpus(0), htlb(~0UL), gtlb(~0UL)
        {
            for (mword i = ACCESS_ONCE(did_f), j = 0; j <= LAST_PCID; i++, j++)
            {
                i %= (LAST_PCID + 1);

                if (ACCESS_ONCE(did_c[i]) == ~0UL)
                    continue;

                long b = bit_scan_forward (~did_c[i]);
                if (b == -1) b = 0;

                if (Atomic::test_set_bit (did_c[i], b)) {
                    j--;
                    i--;
                    continue;
                }

                did = i * sizeof(did_c[0]) * 8 + b;

                if (did_c[i] != ~0UL && did_f != i)
                    did_f = i;

                return;
            }

            did = NO_PCID;
        }

        ALWAYS_INLINE
        inline ~Space_mem()
        {
            if (did == NO_PCID)
               return;

            mword i = did / (sizeof(did_c[0]) * 8);
            mword b = did % (sizeof(did_c[0]) * 8);

            assert (!((i == 0 && b == 0) || (i == 0 && b == 1)));
            assert (i <= LAST_PCID);

            bool s = Atomic::test_clr_bit (did_c[i], b);
            assert(s);
        }

        ALWAYS_INLINE
        inline size_t lookup (mword virt, Paddr &phys)
        {
            mword attr;
            return hpt.lookup (virt, phys, attr);
        }

        ALWAYS_INLINE
        inline void insert (Quota &quota, mword virt, unsigned o, mword attr, Paddr phys)
        {
            hpt.update (quota, virt, o, phys, attr);
        }

        ALWAYS_INLINE
        inline Paddr replace (Quota &quota, mword v, Paddr p)
        {
            return hpt.replace (quota, v, p);
        }

        INIT
        void insert_root (Quota &quota, uint64, uint64, mword = 0x7);

        bool insert_utcb (Quota &quota, mword, mword = 0);

        bool remove_utcb (mword);

        bool update (Quota_guard &quota, Mdb *, mword = 0);

        static void shootdown(Pd *);

        void init (Quota &quota, unsigned);

        ALWAYS_INLINE
        inline mword sticky_sub(mword s) { return s & 0x4; }
};
