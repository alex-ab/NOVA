/*
 * Protection Domain
 *
 * Copyright (C) 2009-2011 Udo Steinberg <udo@hypervisor.org>
 * Economic rights: Technische Universitaet Dresden (Germany)
 *
 * Copyright (C) 2012 Udo Steinberg, Intel Corporation.
 * Copyright (C) 2015 Alexander Boettcher, Genode Labs GmbH
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

#include "crd.hpp"
#include "iommu_intel.hpp"
#include "space_mem.hpp"
#include "space_obj.hpp"
#include "space_pio.hpp"

class Pd : public Kobject, public Refcount, public Space_mem, public Space_pio, public Space_obj
{
    private:
        static Slab_cache cache;

        WARN_UNUSED_RESULT
        mword clamp (mword,   mword &, mword, mword);

        WARN_UNUSED_RESULT
        mword clamp (mword &, mword &, mword, mword, mword);

        static void pre_free (Rcu_elem * a)
        {
            Pd * pd = static_cast <Pd *>(a);

            Crd crd(Crd::MEM);
            pd->revoke<Space_mem>(crd.base(), crd.order(), crd.attr(), true, false);

            crd = Crd(Crd::PIO);
            pd->revoke<Space_pio>(crd.base(), crd.order(), crd.attr(), true, false);

            crd = Crd(Crd::OBJ);
            pd->revoke<Space_obj>(crd.base(), crd.order(), crd.attr(), true, false);

            pd->release_rid([&](uint16 const rid) {
                Iommu::Interface::release(rid, pd);
            });
        }

        static void free (Rcu_elem * a) {
            Pd * pd = static_cast <Pd *>(a);

            if (pd->del_ref()) {
                assert (pd != Pd::current);
                delete pd;
            }
        }

        uint16 rids[7];
        uint16 rids_u  { 0 };

        static_assert (sizeof(rids_u) * 8 >= sizeof(rids) / sizeof(rids[0]), "rids_u too small");

    public:
        static Pd *current CPULOCAL_HOT;
        static Pd kern, root;

        Quota quota { };

        Slab_cache pt_cache;
        Slab_cache mdb_cache;
        Slab_cache sm_cache;
        Slab_cache sc_cache;
        Slab_cache ec_cache;
        Slab_cache fpu_cache;

        INIT
        Pd (Pd *);
        ~Pd();

        Pd (Pd *own, mword sel, mword a);

        inline void verbose_debug()
        {
            trace(0, "%p usage/limit %lu/%lu o=%lu", this,
                  quota.usage(), quota.limit(), quota.over);

            auto const & slab_stats = [&] (auto const text,
                                           auto const slab_count,
                                           auto const elements_used,
                                           auto const elements_max,
                                           auto const suspicious_count)
            {
                 trace(0, "%p %s: slabs=%lu elements=%lu/%lu, suspicious=%lu",
                       this, text, slab_count, elements_used, elements_max,
                       suspicious_count);
            };

            if (pt_cache.head)  pt_cache.head ->stats("pt ", slab_stats);
            if (mdb_cache.head) mdb_cache.head->stats("mdb", slab_stats);
            if (sm_cache.head)  sm_cache.head ->stats("sm ", slab_stats);
            if (sc_cache.head)  sc_cache.head ->stats("sc ", slab_stats);
            if (ec_cache.head)  ec_cache.head ->stats("ec ", slab_stats);
            if (fpu_cache.head) fpu_cache.head->stats("fpu", slab_stats);
        }

        ALWAYS_INLINE HOT
        inline void make_current()
        {
            mword pcid = did;

            if (EXPECT_FALSE (htlb.chk (Cpu::id)))
                htlb.clr (Cpu::id);

            else {

                if (EXPECT_TRUE (current == this))
                    return;

                if (pcid != NO_PCID)
                    pcid |= static_cast<mword>(1ULL << 63);
            }

            if (current->del_rcu())
                Rcu::call (current);

            current = this;

            bool ok = current->add_ref();
            assert (ok);

            loc[Cpu::id].make_current (Cpu::feature (Cpu::FEAT_PCID) ? pcid : 0);
        }

        ALWAYS_INLINE
        static inline Pd *remote (unsigned c)
        {
            return *reinterpret_cast<volatile typeof current *>(reinterpret_cast<mword>(&current) - CPU_LOCAL_DATA + HV_GLOBAL_CPUS + c * PAGE_SIZE);
        }

        ALWAYS_INLINE
        inline Space *subspace (Crd::Type t)
        {
            switch (t) {
                case Crd::MEM:  return static_cast<Space_mem *>(this);
                case Crd::PIO:  return static_cast<Space_pio *>(this);
                case Crd::OBJ:  return static_cast<Space_obj *>(this);
            }

            return nullptr;
        }

        template <typename>
        bool delegate (Pd *, mword, mword, mword, mword, mword = 0, char const * = nullptr);

        template <typename>
        void revoke (mword, mword, mword, bool, bool);

        void xfer_items (Pd *, Crd, Crd, Xfer *, Xfer *, unsigned long);

        void xlt_crd (Pd *, Crd, Crd &);
        void del_crd (Pd *, Crd, Crd &, mword = 0, mword = 0);
        void rev_crd (Crd, bool, bool, bool);

        void assign_rid(uint16 r);

        template<typename FUNC>
        void release_rid(FUNC const &fn)
        {
            for (unsigned i = 0; i < sizeof(rids) / sizeof(rids[0]); i++) {
                bool free_up = false;
                uint16 rid   = 0;

                {
                    /* avoid cross taking of spinlock in iommu and here */
                    Lock_guard <Spinlock> guard (Kobject::lock);

                    if (rids_u & (1U << i)) {
                        free_up = true;
                        rid     = rids[i];

                        rids_u ^= static_cast<uint16>(1U << i);
                        /* this is called by Rcu::call or ~Pd - no result evaluation */
                        del_rcu();
                    }
                }

                if (free_up)
                    fn(rid);
            }
        }


        void flush_pgt()
        {
            for (unsigned i = 0; i < sizeof(rids) / sizeof(rids[0]); i++) {
                if (rids_u & (1U << i))
                    Iommu::Interface::flush_pgt(rids[i], *this);
            }
        }


        ALWAYS_INLINE
        static inline void *operator new (size_t, Quota &quota) { return cache.alloc(quota); }

        ALWAYS_INLINE
        static inline void operator delete (void *ptr)
        {
            Pd *pd_del = static_cast<Pd *>(ptr);
            Pd *pd_to  = static_cast<Pd *>(static_cast<Space_obj *>(pd_del->space));

            pd_del->quota.free_up(pd_to->quota);

            cache.free (ptr, pd_to->quota);
        }
};
