/*
 * DMA Remapping Unit (DMAR)
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

#include "bits.hpp"
#include "iommu_intel.hpp"
#include "lapic.hpp"
#include "pd.hpp"
#include "stdio.hpp"
#include "vectors.hpp"

INIT_PRIORITY (PRIO_SLAB)
Slab_cache  Dmar::cache (sizeof (Dmar), 8);

Dmar *      Dmar::list;
Dmar_ctx *  Dmar::ctx = new (Pd::kern.quota) Dmar_ctx;
Dmar_irt *  Dmar::irt = new (Pd::kern.quota) Dmar_irt;
uint32      Dmar::gcmd = GCMD_TE;

Dmar::Dmar (Paddr p) : List<Dmar> (list), reg_base ((hwdev_addr -= PAGE_SIZE) | (p & PAGE_MASK)), invq (static_cast<Dmar_qi *>(Buddy::allocator.alloc (ord, Pd::kern.quota, Buddy::FILL_0))), invq_idx (0)
{
    Pd::kern.Space_mem::delreg (Pd::kern.quota, Pd::kern.mdb_cache, p & ~PAGE_MASK);
    Pd::kern.Space_mem::insert (Pd::kern.quota, reg_base, 0, Hpt::HPT_NX | Hpt::HPT_G | Hpt::HPT_UC | Hpt::HPT_W | Hpt::HPT_P, p & ~PAGE_MASK);

    cap  = read<uint64>(REG_CAP);
    ecap = read<uint64>(REG_ECAP);

    if (invalid()) {
        Console::print("DMAR at address %lx is invalid (cap=%llx, ecap=%llx) - IOMMU protection is DISABLED\n", p, cap, ecap);
        return;
    }

    Dpt::ord = min (Dpt::ord, static_cast<mword>(bit_scan_reverse (static_cast<mword>(cap >> 34) & 0xf) + 2) * Dpt::bpl() - 1);

    write<uint32>(REG_FEADDR, 0xfee00000 | Cpu::apic_id[0] << 12);
    write<uint32>(REG_FEDATA, VEC_MSI_DMAR);
    write<uint32>(REG_FECTL,  0);

    write<uint64>(REG_RTADDR, Buddy::ptr_to_phys (ctx));
    command (GCMD_SRTP);

    if (ir()) {
        write<uint64>(REG_IRTA, Buddy::ptr_to_phys (irt) | 7);
        command (GCMD_SIRTP);
        gcmd |= GCMD_IRE;
    }

    if (qi()) {
        write<uint64>(REG_IQT, 0);
        write<uint64>(REG_IQA, Buddy::ptr_to_phys (invq));
        command (GCMD_QIE);
        gcmd |= GCMD_QIE;
    }
}

void Dmar::assign (uint16 rid, Pd *p)
{
    if (invalid())
        return;

    mword lev = bit_scan_reverse (read<mword>(REG_CAP) >> 8 & 0x1f);

    Lock_guard <Spinlock> guard (lock);

    Dmar_ctx *r = ctx + (rid >> 8);
    if (!r->present())
        r->set (0, Buddy::ptr_to_phys (new (p->quota) Dmar_ctx) | 1);

    Dmar_ctx *c = static_cast<Dmar_ctx *>(Buddy::phys_to_ptr (r->addr())) + (rid & 0xff);
    if (c->present())
        c->set (0, 0);

    flush_ctx();

    c->set (lev | p->did << 8, p->dpt.root (p->quota, lev + 1) | 1);

    p->assign_rid(rid);

    if (p != &Pd::kern && read<uint32>(REG_FECTL) & (1UL << 31)) {
        trace(TRACE_IOMMU, "IOMMU:%p - re-enabling fault reporting", this);
        write<uint32>(REG_FECTL, 0);
    }
}

void Dmar::release (uint16 rid, Pd *p)
{
    for (Dmar *dmar = list; dmar; dmar = dmar->next) {

        if (dmar->invalid())
            continue;

        Lock_guard <Spinlock> guard (dmar->lock);

        Dmar_ctx *r = ctx + (rid >> 8);
        if (!r->present())
            continue;

        Dmar_ctx *c = static_cast<Dmar_ctx *>(Buddy::phys_to_ptr (r->addr())) + (rid & 0xff);
        if (!c->present())
            continue;

        mword lev = bit_scan_reverse (dmar->read<mword>(REG_CAP) >> 8 & 0x1f);

        if (!c->match(lev | p->did << 8, p->dpt.root (p->quota, lev + 1) | 1))
            continue;

        for (unsigned i = 0; i < PAGE_SIZE / sizeof(irt[0]); i++) {
            if ((irt[i].high() & 0xffff) == rid)
                irt[i].set(0, 0);
        }

        c->set (0, 0);
        dmar->flush_ctx();
    }
}

void Dmar::fault_handler()
{
    unsigned fault_counter = 0;
    bool disabled = false;

    for (uint32 fsts; fsts = read<uint32>(REG_FSTS), fsts & 0xff;) {

        if (fsts & 0x2) {
            uint64 hi, lo;
            for (unsigned frr = fsts >> 8 & 0xff; read (frr, hi, lo), hi & 1ull << 63; frr = (frr + 1) % nfr()) {

                if (disabled)
                    continue;

                trace (TRACE_IOMMU, "IOMMU:%p FRR:%u FR:%#x BDF:%x:%x:%x FI:%#010llx (%u)",
                       this,
                       frr,
                       static_cast<uint32>(hi >> 32) & 0xff,
                       static_cast<uint32>(hi >> 8) & 0xff,
                       static_cast<uint32>(hi >> 3) & 0x1f,
                       static_cast<uint32>(hi) & 0x7,
                       lo, fault_counter++);

                uint16 const rid = hi & 0xffff;

                if (disable_reporting(rid)) {
                    write<uint32>(REG_FECTL, 1UL << 31);
                    disabled = true;
                }
            }
        }

        write<uint32>(REG_FSTS, 0x7d);

        if (!fault_counter)
            fault_counter ++;
    }

    if (!fault_counter)
        return;

    update_reporting(disabled);
}

void Dmar::vector (unsigned vector)
{
    unsigned msi = vector - VEC_MSI;

    if (EXPECT_TRUE (msi == 0))
        for (Dmar *dmar = list; dmar; dmar = dmar->next)
            if (!dmar->invalid()) dmar->fault_handler();
}
