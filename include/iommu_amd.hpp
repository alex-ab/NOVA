/*
 * IOMMU - AMD-V
 *
 * Copyright (C) 2020 Alexander Boettcher <alexander.boettcher@genode-labs.com>
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

#include "iommu.hpp"
#include "list.hpp"
#include "slab.hpp"
#include "vectors.hpp"

class Pd;

namespace Iommu {
    class Amd;
    class Dte;
};

struct Iommu::Dte
{
    uint64 x[4];

    void   domain_id(uint64 const dom_id) { x[1] = (x[1] & ~0xffffull) | (dom_id & 0xffffull); }
    uint16 domain_id() const { return x[1] & 0xffffu; }

    static uint64 p_mask() { return ~0xfffull & ((1ull << 52) - 1); }

    uint64 hptrp() const { return x[0] & p_mask(); }
    void   hptrp(uint64 const paddr) {
        x[0] &= ~p_mask();
        x[0] |= paddr & p_mask();
    }

    void dma_enable() { x[0] |= 2ull; }
    void dma_disable() { x[0] &= ~2ull; }
    bool dma_active() const { return x[0] & 2; }

    void enable()  { x[0] |= 1ull; }
    bool enabled() const { return x[0] & 1; }

    void paging_mode(uint64 mode) {
        x[0] &= ~(7ull << 9);
        x[0] |= (mode & 0x7ull) << 9;
    }

    void ir() { x[0] |= 1ull << 61; }
    void iw() { x[0] |= 1ull << 62; }

    void iv()             { x[2] |= 1ull; }
    void inttablen256()   { x[2] |= 8ull <<  1; }
    void intctl10()       { x[2] |= 2ull << 60; }
    void intctl_disable() { x[2] &= ~(3ULL << 60); }
    bool intctl() const { return x[2] & (3ull << 60); }

    uint64 _itrp_mask() const { return ((1ull << 45) - 1) & ~0x3full; }
    uint64 itrp()       const { return x[2] & _itrp_mask(); }
    void   itrp(uint64 paddr)
    {
        x[2] &= ~_itrp_mask();
        x[2] |= paddr & _itrp_mask();
    };
};

class Iommu::Amd : public Iommu::Interface, public List<Amd>
{
    private:
        mword const       reg_base;

        uint64            cmd_base   { 0 };
        uint64            event_base { 0 };

        Spinlock          lock { };

        uint16 const      iommu_rid;
        bool const        efeat_valid;

        /* start - one large area as long as we don't support segmentation */
        static Dte *      dtb;
        static uint32     dtsize;
        /* end */
        static Amd *      list;
        static Slab_cache cache;

        enum Reg
        {
            REG_DTB   = 0x00,
            REG_CMD   = 0x08,
            REG_EVENT = 0x10,
            REG_CTRL  = 0x18,
            REG_EFEAT = 0x30,
            REG_MSI_HEADER = 0x158,
            REG_CMD_HEAD   = 0x2000,
            REG_CMD_TAIL   = 0x2008,
            REG_EVENT_HEAD = 0x2010,
            REG_EVENT_TAIL = 0x2018,
            REG_STATUS     = 0x2020,
        };

        enum { EVENT_ORDER = 0, CMD_ORDER = 0 };

        template <typename T>
        ALWAYS_INLINE
        inline T read (Reg reg)
        {
            return *reinterpret_cast<T volatile *>(reg_base + reg);
        }

        template <typename T>
        ALWAYS_INLINE
        inline void write (Reg reg, T val)
        {
            *reinterpret_cast<T volatile *>(reg_base + reg) = val;
        }

        ALWAYS_INLINE
        inline void disable_events(uint64 const ctrl)
        {
            write(REG_CTRL, ctrl & ~(4ULL | 8ULL));
        }

        bool enable_events(uint64 const ctrl)
        {
            bool const enabled  = (ctrl & 4) || (ctrl & 8);
            bool reenable = !enabled;

            uint64 status = read<uint64>(REG_STATUS);
            if (status & 0x1) {
                trace(TRACE_SMMU, "IOMMU:%p event overflow", this);

                disable_events(ctrl);
                reenable = true;

                /* reset head/tail to 0 */
                write<uint64>(REG_EVENT_TAIL, read<uint64>(REG_EVENT_TAIL) | ~ring_mask());
                write<uint64>(REG_EVENT_HEAD, read<uint64>(REG_EVENT_HEAD) | ~ring_mask());

            }

            /* ack */
            write<uint64>(REG_STATUS, status);

            if (reenable)
                write<uint64>(REG_CTRL, ctrl | 8ULL | 4ULL);

            return !enabled;
        }

        void enable_dtb();

        void start ();

        Dte* dte(uint16) const;

        ALWAYS_INLINE
        inline uint64 mask(uint64 paddr) const { return paddr & Dte::p_mask(); }

        ALWAYS_INLINE
        inline uint64 ring_mask() const { return (((1ull << 19) - 1) & ~0xfull); }

        ALWAYS_INLINE
        inline uint64 ring_mask(uint64 const value) const {
            return value & ring_mask(); }

        void fault_handler();

        ALWAYS_INLINE
        void flush_dte(unsigned rid, bool wait) { flush(rid, 2, wait); }
        ALWAYS_INLINE
        void flush_irt(unsigned rid, bool wait) { flush(rid, 5, wait); }

        void flush_pgt(Pd &);
        void flush(unsigned, unsigned, bool);

        void release(Pd *, uint16);
        void release(Dte *, uint16);

    public:
        INIT
        Amd (Paddr, uint16, bool);

        ALWAYS_INLINE
        static inline void *operator new (size_t, Quota &quota) { return cache.alloc(quota); }

        ALWAYS_INLINE
        static inline void enable ()
        {
            for (Amd *iommu = list; iommu; iommu = iommu->next)
                if (!iommu->invalid()) iommu->start ();
        }

        ALWAYS_INLINE
        bool invalid() const { return false; }

        void alias (uint16, uint16);
        void alloc_dtb (unsigned short, unsigned short, unsigned short, uint8);
        void assign (uint16, Pd *) override;

        static void release (uint16 rid, Pd *pd)
        {
            for (Amd *iommu = list; iommu; iommu = iommu->next)
                iommu->release(pd, rid);
        }

        static void flush_pgt(uint16, Pd &);

        static void vector (unsigned const vector)
        {
            unsigned msi = vector - VEC_MSI;

            if (EXPECT_TRUE (msi == 0))
                for (Amd *iommu = list; iommu; iommu = iommu->next)
                    if (!iommu->invalid()) iommu->fault_handler ();
        }

        static void set_irt (unsigned, unsigned, unsigned, unsigned, unsigned);

        ALWAYS_INLINE
        static bool online () { return list; }
};
