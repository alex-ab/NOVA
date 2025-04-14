/*
 * Secure Virtual Machine (SVM)
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

#include "cmdline.hpp"
#include "cpu.hpp"
#include "hip.hpp"
#include "msr.hpp"
#include "stdio.hpp"
#include "svm.hpp"
#include "pd.hpp"


struct Msr_bitmap
{
    uint8 range0[2048];
    uint8 range1[2048];
    uint8 range2[2048];
    uint8 range3[2048]; /* reserved & unused */

    void disable_msr_exit(Msr::Register const msr)
    {
        auto const valid_range = msr & 0x1ffffu;
        auto const bit         = 2 * (valid_range % 4);
        auto const index       = 2 * (valid_range / 8)
                               + ((valid_range % 8) > 3) ? 1 : 0;
        auto const mask        = uint8(~(3u << bit));

        uint8 *range = nullptr;

        switch (unsigned(msr)) {
        case 0x0000'0000 ... 0x0000'1fff: range = range0; break;
        case 0xc000'0000 ... 0xc000'1fff: range = range1; break;
        case 0xc001'0000 ... 0xc001'1fff: range = range2; break;
        default: return;
        }

        if (range) range[index] &= mask;
    }

    ALWAYS_INLINE
    static inline void *operator new (size_t, Quota &quota)
    {
        /* allocate two pages and set all bits */
        return Buddy::allocator.alloc(1, quota, Buddy::FILL_1);
    }

    ALWAYS_INLINE
    static inline void destroy(Msr_bitmap *obj, Quota &quota)
    {
        Buddy::allocator.free(reinterpret_cast<mword>(obj), quota);
    }
};

Paddr       Vmcb::root;
unsigned    Vmcb::asid_ctr;
uint32      Vmcb::svm_version;
uint32      Vmcb::svm_feature;

Queue<Vmcb_state> Vmcb_state::queue;

INIT_PRIORITY (PRIO_SLAB)
Slab_cache Vmcb_state::cache (sizeof (Vmcb_state), 8);

Vmcb::Vmcb (Quota &quota, mword bmp, mword nptp, unsigned id) : base_io (bmp), asid (id), int_control (1ul << 24), npt_cr3 (nptp), efer (Cpu::EFER_SVME), g_pat (0x7040600070406ull)
{
    auto &msr_bitmap = *new (quota) Msr_bitmap;

    base_msr = Buddy::ptr_to_phys(&msr_bitmap);

    msr_bitmap.disable_msr_exit(Msr::Register::IA32_FS_BASE);
    msr_bitmap.disable_msr_exit(Msr::Register::IA32_GS_BASE);
    msr_bitmap.disable_msr_exit(Msr::Register::IA32_KERNEL_GS_BASE);
}

void Vmcb::destroy(Vmcb &obj, Quota &quota)
{
    if (obj.base_msr)
        Msr_bitmap::destroy (reinterpret_cast<Msr_bitmap *>(Buddy::phys_to_ptr(static_cast<Paddr>(obj.base_msr))), quota);

    obj.~Vmcb();

    Buddy::allocator.free (reinterpret_cast<mword>(&obj), quota);
}

void Vmcb::init()
{
    if (!Cpu::feature (Cpu::FEAT_SVM) || (Msr::read<uint64>(Msr::AMD_SVM_VM_CR) & 0x10)) {
        Hip::clr_feature (Hip::FEAT_SVM);
        return;
    }

    if (Cmdline::vtlb)
        svm_feature &= ~1;

    Msr::write (Msr::IA32_EFER, Msr::read<uint32>(Msr::IA32_EFER) | Cpu::EFER_SVME);
    if (!root)
        root = Buddy::ptr_to_phys (new (Pd::kern.quota) Vmcb(Space_mem::NO_ASID_ID));
    Msr::write (Msr::AMD_SVM_HSAVE_PA, root);

    trace (TRACE_SVM, "VMCB:%#010lx REV:%#x NPT:%d", root, svm_version, has_npt());
}
