/*
 * Host Memory Space
 *
 * Copyright (C) 2019-2024 Udo Steinberg, BlueRock Security, Inc.
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

#include "ptab_npt.hpp"
#include "space_mem.hpp"

class Space_hst final : public Space_mem<Space_hst>
{
    private:
        Vmid const  vmid;
        Nptp        nptp;

        Space_hst();

        Space_hst (Refptr<Pd> &p) : Space_mem { Kobject::Subtype::HST, p } {}

        void collect() override final
        {
            trace (TRACE_DESTROY, "KOBJ: HST %p collected", static_cast<void *>(this));
        }

    public:
        static Space_hst nova;

        static constexpr auto selectors() { return BIT64 (Npt::ibits - PAGE_BITS); }
        static           auto max_order() { return Npt::lev_ord(); }

        [[nodiscard]] static Space_hst *create (Status &s, Slab_cache &cache, Pd *pd)
        {
            // Acquire reference
            Refptr<Pd> ref_pd { pd };

            // Failed to acquire reference
            if (!ref_pd) [[unlikely]]
                s = Status::ABORTED;

            else {

                auto const hst { new (cache) Space_hst { ref_pd } };

                // If we created hst, then reference must have been consumed
                assert (!hst || !ref_pd);

                if (hst) [[likely]] {

                    if (hst->nptp.root_init()) [[likely]]
                        return hst;

                    operator delete (hst, cache);
                }

                s = Status::MEM_OBJ;
            }

            return nullptr;
        }

        void destroy()
        {
            auto &cache { get_pd()->hst_cache };

            this->~Space_hst();

            operator delete (this, cache);
        }

        auto lookup (uint64_t v, uint64_t &p, unsigned &o, Memattr &ma) const { return nptp.lookup (v, p, o, ma); }

        auto update (uint64_t v, uint64_t p, unsigned o, Paging::Permissions pm, Memattr ma) { return nptp.update (v, p, o, pm, ma); }

        void sync() { nptp.invalidate (vmid); }

        void make_current() { nptp.make_current (vmid); }

        static void access_ctrl (uint64_t phys, size_t size, Paging::Permissions perm) { Space_mem::access_ctrl (nova, phys, size, perm, Memattr::dev()); }
};
