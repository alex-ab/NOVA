/*
 * DMA Memory Space
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

#include "ptab_dpt.hpp"
#include "smmu.hpp"
#include "space_mem.hpp"

class Space_dma final : public Space_mem<Space_dma>
{
    private:
        Sdid const  sdid;
        Dptp        dptp;

        Space_dma() : Space_mem { Kobject::Subtype::DMA } {}

        Space_dma (Refptr<Pd> &p) : Space_mem { Kobject::Subtype::DMA, p } {}

        void collect() override final
        {
            trace (TRACE_DESTROY, "KOBJ: DMA %p collected", static_cast<void *>(this));
        }

    public:
        static Space_dma nova;

        static constexpr auto selectors() { return BIT64 (Dpt::ibits - PAGE_BITS); }
        static           auto max_order() { return Dpt::lev_ord(); }

        [[nodiscard]] auto get_ptab (unsigned l) { return dptp.root_init (l); }

        [[nodiscard]] static Space_dma *create (Status &s, Slab_cache &cache, Pd *pd)
        {
            // Acquire reference
            Refptr<Pd> ref_pd { pd };

            // Failed to acquire reference
            if (!ref_pd) [[unlikely]]
                s = Status::ABORTED;

            else {

                auto const dma { new (cache) Space_dma { ref_pd } };

                // If we created dma, then reference must have been consumed
                assert (!dma || !ref_pd);

                if (dma) [[likely]] {

                    if (dma->dptp.root_init()) [[likely]]
                        return dma;

                    operator delete (dma, cache);
                }

                s = Status::MEM_OBJ;
            }

            return nullptr;
        }

        void destroy()
        {
            auto &cache { get_pd()->dma_cache };

            this->~Space_dma();

            operator delete (this, cache);
        }

        auto update (uint64_t v, uint64_t p, unsigned o, Paging::Permissions pm, Memattr ma) { return dptp.update (v, p, o, pm, ma); }

        void sync() { Smmu::invalidate_tlb_all (sdid); }

        auto get_sdid() const { return sdid; }

        static void access_ctrl (uint64_t phys, size_t size, Paging::Permissions perm) { Space_mem::access_ctrl (nova, phys, size, perm, Memattr::ram()); }
};
