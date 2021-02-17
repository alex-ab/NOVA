/*
 * System Memory Management Unit (Intel IOMMU)
 *
 * Copyright (C) 2009-2011 Udo Steinberg <udo@hypervisor.org>
 * Economic rights: Technische Universitaet Dresden (Germany)
 *
 * Copyright (C) 2012-2013 Udo Steinberg, Intel Corporation.
 * Copyright (C) 2014 Udo Steinberg, FireEye, Inc.
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

#include "bits.hpp"
#include "pd.hpp"
#include "smmu.hpp"
#include "stdio.hpp"
#include "vectors.hpp"

INIT_PRIORITY (PRIO_SLAB) Slab_cache Smmu::cache { sizeof (Smmu), alignof (Smmu) };

Smmu::Smmu (uint64_t p) : List { list }, phys_base { p }, mmio_base { mmap }, invq { static_cast<Inv_dsc *>(Buddy::alloc (ord, Buddy::Fill::BITS0)) }
{
    mmap += PAGE_SIZE (0);

    // Reserve MMIO region
    Space_hst::access_ctrl (p, PAGE_SIZE (0), Paging::NONE);

    Hptp::master_map (mmio_base, phys_base, 0, Paging::Permissions (Paging::G | Paging::W | Paging::R), Memattr::dev());

    cap  = read (Reg64::CAP);
    ecap = read (Reg64::ECAP);

    // DPT maximum leaf level: 1 + { 1 (1GB), 0 (2MB), -1 (4KB) }
    Dptp::set_mll (1 + bit_scan_msb (cap >> 34 & BIT_RANGE (1, 0)));

    // Treat DPT as noncoherent if at least one SMMU requires it
    Dpt::noncoherent |= !feature (Ecap::PWC);

    // If the SMMU does not support interrupt remapping, then disable it
    ir &= feature (Ecap::IR);

    // If the SMMU does not support x2APIC, then disable it
    Lapic::x2apic &= feature (Ecap::EIM);

    auto const ver { read (Reg32::VER) };

    trace (TRACE_SMMU, "SMMU: %#010lx %u.%u CAP:%#018lx ECAP:%#018lx", phys_base, ver >> 4 & BIT_RANGE (3, 0), ver & BIT_RANGE (3, 0), cap, ecap);
}

void Smmu::init()
{
    write (Reg32::FEADDR, Lapic::msi_base | Lapic::id[0] << 12);
    write (Reg32::FEDATA, VEC_FLT);
    write (Reg32::FECTL,  0);

    // Clear any pending faults from platform boot
    write (Reg32::FSTS, Fault::ITE | Fault::ICE | Fault::IQE | Fault::APF | Fault::AFO | Fault::PFO);

    if (feature (Ecap::QI)) {
        write (Reg64::IQT, 0);
        write (Reg64::IQA, Kmem::ptr_to_phys (invq));
        command (Cmd::QIE);
    }

    set_irt (Kmem::ptr_to_phys (irt));
    set_rtp (Kmem::ptr_to_phys (ctx));

    // Disable PMR once DMA remapping is enabled
    if (feature (Cap::PLMR) || feature (Cap::PHMR))
        clr_pmr();
}

bool Smmu::configure (Space_dma *dma, uintptr_t dad, bool inv)
{
    auto const pci { static_cast<pci_t>(dad) };
    auto const lev { min (Dpt::lev(), 2U + bit_scan_msb (cap >> 8 & BIT_RANGE (4, 0))) };

    auto const sdid { dma->get_sdid() };
    auto const ptab { dma->get_ptab (lev - 1) };

    if (!ptab) [[unlikely]]
        return false;

    uint16_t zap;

    {   Lock_guard <Spinlock> guard { cfg_lock };

        auto const r { ctx + Pci::bus (pci) };

        if (!r->present())
            r->set (0, Kmem::ptr_to_phys (new Entry_ctx) | BIT (0));

        auto const c { static_cast<Entry_ctx *>(Kmem::phys_to_ptr (r->addr())) + Pci::ari (pci) };

        zap = c->did();

        if (!c->present())
            inv = false;
        else
            c->set (0, 0);

        c->set (sdid << 8 | (lev - 2), Kmem::ptr_to_phys (ptab) | BIT (0));
    }

    if (inv) [[likely]]
        invalidate_ctx (Pci::bdf (pci), zap);

    trace (TRACE_SMMU, "SMMU: %#010lx Device %04x:%02x:%02x.%x assigned to Domain %u", phys_base, Pci::seg (pci), Pci::bus (pci), Pci::dev (pci), Pci::fun (pci), static_cast<unsigned>(sdid));

    return true;
}

void Smmu::fault()
{
    auto const fsts { read (Reg32::FSTS) };

    if (fsts & Fault::PPF) [[unlikely]] {
        uint64_t hi, lo;
        for (unsigned frr { fsts >> 8 & BIT_RANGE (7, 0) }; read (frr, hi, lo), hi & BIT64 (63); frr = (frr + 1) % nfr()) {
            pci_t const sid { static_cast<uint16_t>(hi) };
            trace (TRACE_SMMU, "SMMU: %#010lx FRR:%u FR:%#x SID:%02x:%02x.%x FI:%#010lx", phys_base, frr, static_cast<uint8_t>(hi >> 32), Pci::bus (sid), Pci::dev (sid), Pci::fun (sid), lo);
        }
    }

    // Queued Invalidation Interface Errors
    if (fsts & (Fault::ITE | Fault::ICE | Fault::IQE)) [[unlikely]] {

        auto const error { read (Reg64::IQERCD) };

        // Invalidation Timeout Error
        if (fsts & Fault::ITE) [[unlikely]] {
            pci_t const sid { static_cast<uint16_t>(error >> 32) };
            trace (TRACE_SMMU, "SMMU: %#010lx ITE from SID:%02x:%02x.%x", phys_base, Pci::bus (sid), Pci::dev (sid), Pci::fun (sid));
        }

        // Invalidation Completion Error
        if (fsts & Fault::ICE) [[unlikely]] {
            pci_t const sid { static_cast<uint16_t>(error >> 48) };
            trace (TRACE_SMMU, "SMMU: %#010lx ICE from SID:%02x:%02x.%x", phys_base, Pci::bus (sid), Pci::dev (sid), Pci::fun (sid));
        }

        // Invalidation Queue Error
        if (fsts & Fault::IQE) [[unlikely]]
            trace (TRACE_SMMU, "SMMU: %#010lx IQE %lu", phys_base, error & BIT_RANGE (3, 0));
    }

    write (Reg32::FSTS, Fault::ITE | Fault::ICE | Fault::IQE | Fault::APF | Fault::AFO | Fault::PFO);
}
