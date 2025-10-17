/*
 * Initialization Code
 *
 * Copyright (C) 2009-2011 Udo Steinberg <udo@hypervisor.org>
 * Economic rights: Technische Universitaet Dresden (Germany)
 *
 * Copyright (C) 2012-2013 Udo Steinberg, Intel Corporation.
 * Copyright (C) 2019-2025 Udo Steinberg, BlueRock Security, Inc.
 * Copyright (C) 2017-2025 Alexander Boettcher, Genode Labs GmbH
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

#include "acpi.hpp"
#include "compiler.hpp"
#include "console_mem.hpp"
#include "console_serial.hpp"
#include "console_vga.hpp"
#include "gsi.hpp"
#include "hip.hpp"
#include "hpt.hpp"
#include "idt.hpp"
#include "keyb.hpp"
#include "patch.hpp"
#include "pd.hpp"
#include "multiboot.hpp"
#include "multiboot2.hpp"


static inline unsigned apic_id()
{
    uint32_t leaf {}, id_apic {}, unused {};

    Cpu::cpuid (0, leaf, unused, unused, unused);

    auto try_leaf = [](auto const id, auto &apic_edx) {
        uint32_t leaf_ecx { 0 }, cpus_ebx {}, unused_eax {};

        Cpu::cpuid (id, unused_eax, cpus_ebx, leaf_ecx, apic_edx);

        return !!cpus_ebx;
    };

    if (leaf >= 0x1f && try_leaf(0x1f, id_apic)) return id_apic;
    if (leaf >= 0x0b && try_leaf(0x0b, id_apic)) return id_apic;

    uint32_t ebx { };

    Cpu::cpuid (1, unused, ebx, unused, unused);

    return ebx >> 24;
}

extern "C" INIT
mword kern_ptab_setup()
{
    static Paddr cr3[NUM_CPU];

    auto const cpuid = Lapic::lookup (apic_id());

    if (cpuid < NUM_CPU && cr3[cpuid]) {
        if (cpuid == 0) {
            /* reinit Acpi on resume */
            Acpi::init();
        }

        return cr3[cpuid];
    }

    Hptp hpt;

    // Allocate and map cpu page
    hpt.update (Pd::kern.quota, CPU_LOCAL_DATA, 0,
                Buddy::ptr_to_phys (Buddy::allocator.alloc (0, Pd::kern.quota, Buddy::FILL_0)),
                Hpt::HPT_NX | Hpt::HPT_G | Hpt::HPT_W | Hpt::HPT_P, Memattr::ram());

    // Allocate and map kernel stack
    hpt.update (Pd::kern.quota, CPU_LOCAL_STCK, 0,
                Buddy::ptr_to_phys (Buddy::allocator.alloc (0, Pd::kern.quota, Buddy::FILL_0)),
                Hpt::HPT_NX | Hpt::HPT_G | Hpt::HPT_W | Hpt::HPT_P, Memattr::ram());

    // Sync kernel code and data
    hpt.sync_master_range (Pd::kern.quota, LINK_ADDR, CPU_LOCAL);

    if (cpuid < NUM_CPU)
        cr3[cpuid] = hpt.addr();

    return hpt.addr();
}

extern "C" INIT REGPARM (2)
void init (mword magic, mword mbi)
{
    // Setup 0-page and 1-page
    memset (reinterpret_cast<void *>(&PAGE_0),  0,  PAGE_SIZE);
    memset (reinterpret_cast<void *>(&PAGE_1), ~0u, PAGE_SIZE);

    Patch::detect();

    for (void (**func)() = &CTORS_G; func != &CTORS_E; (*func++)()) ;

    bool hip_ok = Hip::build (magic, mbi);

    for (void (**func)() = &CTORS_C; func != &CTORS_G; (*func++)()) ;

    // Now we're ready to talk to the world
    Console::print ("\fNOVA Microhypervisor v%d-%07lx (%s): [%s] [%s]\n", CFG_VER, reinterpret_cast<mword>(&GIT_VER), ARCH, COMPILER_STRING, magic == Multiboot::MAGIC ? "MBI" : (magic==Multiboot2::MAGIC ? "MBI2" : ""));

    if (!hip_ok)
        Console::print ("error: HIP is incomplete\n");

    Idt::build();
    Gsi::setup();
    Acpi::setup();

    Console_mem::con.setup();
    Console_vga::con.setup();

    Keyb::init();
}
