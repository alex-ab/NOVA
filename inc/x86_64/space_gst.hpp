/*
 * Guest Memory Space
 *
 * Copyright (C) 2009-2011 Udo Steinberg <udo@hypervisor.org>
 * Economic rights: Technische Universitaet Dresden (Germany)
 *
 * Copyright (C) 2012-2013 Udo Steinberg, Intel Corporation.
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

#include "cpuset.hpp"
#include "ptab_ept.hpp"
#include "space_mem.hpp"
#include "tlb.hpp"

class Space_gst final : public Space_mem<Space_gst>
{
    private:
        Eptp    eptp;

        Space_gst (Refptr<Pd> &p) : Space_mem { Kobject::Subtype::GST, p } {}

        void collect() override final
        {
            trace (TRACE_DESTROY, "KOBJ: GST %p collected", static_cast<void *>(this));
        }

    public:
        Cpuset  gtlb;

        static constexpr auto selectors() { return BIT64 (Ept::ibits - PAGE_BITS); }
        static           auto max_order() { return Ept::lev_ord(); }

        [[nodiscard]] static Space_gst *create (Status &s, Slab_cache &cache, Pd *pd)
        {
            // Acquire reference
            Refptr<Pd> ref_pd { pd };

            // Failed to acquire reference
            if (!ref_pd) [[unlikely]]
                s = Status::ABORTED;

            else {

                auto const gst { new (cache) Space_gst { ref_pd } };

                // If we created gst, then reference must have been consumed
                assert (!gst || !ref_pd);

                if (gst) [[likely]] {

                    if (gst->eptp.root_init()) [[likely]]
                        return gst;

                    operator delete (gst, cache);
                }

                s = Status::MEM_OBJ;
            }

            return nullptr;
        }

        void destroy()
        {
            auto &cache { get_pd()->gst_cache };

            this->~Space_gst();

            operator delete (this, cache);
        }

        auto lookup (uint64_t v, uint64_t &p, unsigned &o, Memattr &ma) const { return eptp.lookup (v, p, o, ma); }

        auto update (uint64_t v, uint64_t p, unsigned o, Paging::Permissions pm, Memattr ma) { return eptp.update (v, p, o, pm, ma); }

        void sync() { gtlb.set(); Tlb::shootdown (this); }

        void invalidate() { eptp.invalidate(); }

        auto get_phys() const { return eptp.root_addr(); }
};
