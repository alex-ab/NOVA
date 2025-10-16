/*
 * Local Advanced Programmable Interrupt Controller (LAPIC)
 *
 * Copyright (C) 2009-2011 Udo Steinberg <udo@hypervisor.org>
 * Economic rights: Technische Universitaet Dresden (Germany)
 *
 * Copyright (C) 2012-2013 Udo Steinberg, Intel Corporation.
 * Copyright (C) 2014 Udo Steinberg, FireEye, Inc.
 * Copyright (C) 2019-2024 Udo Steinberg, BlueRock Security, Inc.
 *
 * Copyright (C) 2015-2024 Alexander Boettcher, Genode Labs GmbH
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
#include "barrier.hpp"
#include "cmdline.hpp"
#include "ec.hpp"
#include "hip.hpp"
#include "lapic.hpp"
#include "msr.hpp"
#include "rcu.hpp"
#include "stdio.hpp"
#include "timeout.hpp"
#include "vectors.hpp"

unsigned    Lapic::freq_tsc;
unsigned    Lapic::freq_bus;

void Lapic::init_cpuid()
{
    auto const apic_base { static_cast<mword>(Msr::read (Msr::Reg64::IA32_APIC_BASE)) };

    Pd::kern.Space_mem::delreg (Pd::kern.quota, Pd::kern.mdb_cache, apic_base & ~PAGE_MASK);
    Hptp (Hpt::current()).update (Pd::kern.quota, CPU_LOCAL_APIC, 0, Hpt::HPT_NX | Hpt::HPT_G | Hpt::HPT_UC | Hpt::HPT_W | Hpt::HPT_P, apic_base & ~PAGE_MASK);

    auto apic_id = (apic_base & BIT (10)) ? read_x2apic (Reg32::IDR)
                                          : read_legacy (Reg32::IDR) >> 24;

    Cpu::id  = Lapic::lookup (apic_id);
    Cpu::bsp = apic_base & 0x100;
}

void Lapic::init(bool const invariant_tsc)
{
    auto const apic_base { Msr::read (Msr::Reg64::IA32_APIC_BASE) };

    // HW enable
    Msr::write (Msr::Reg64::IA32_APIC_BASE, apic_base | BIT (11) | BIT (10) * x2apic);

    // SW enable
    write (Reg32::SVR, read (Reg32::SVR) | BIT (8));

    bool const dl { Cpu::feature (Cpu::Feature::TSC_DEADLINE) && !Cmdline::nodl };

    switch (lvt_max()) {
        default:            // 7 entries since NHM
            set_lvt (Reg32::LVT_CMCHK, Delivery::DLV_FIXED, VEC_LVT_CMCHK);
            [[fallthrough]];
        case 5:             // 6 entries since WMT
            set_lvt (Reg32::LVT_THERM, Delivery::DLV_FIXED, VEC_LVT_THERM);
            [[fallthrough]];
        case 4:             // 5 entries since P6
            set_lvt (Reg32::LVT_PERFM, Delivery::DLV_FIXED, VEC_LVT_PERFM);
            [[fallthrough]];
        case 3:             // 4 entries since P5
            set_lvt (Reg32::LVT_ERROR, Delivery::DLV_FIXED, VEC_LVT_ERROR);
            [[fallthrough]];
        case 2:
            set_lvt (Reg32::LVT_LINT1, Delivery::DLV_NMI, 0);
            [[fallthrough]];
        case 1:
            set_lvt (Reg32::LVT_LINT0, Delivery::DLV_EXTINT, 0, BIT (16));
            [[fallthrough]];
        case 0:
            set_lvt (Reg32::LVT_TIMER, Delivery::DLV_FIXED, VEC_LVT_TIMER, BIT (18) * dl);
    }

    write (Reg32::TPR, 0x10);
    write (Reg32::TMR_DCR, 0xb);

    if (Cpu::bsp) {
        bool measured = !read_tsc_freq();

        send_exc (0, Delivery::DLV_INIT);

        if (!freq_tsc) {
            uint32 const delay = (dl || !invariant_tsc) ? 10 : 500;

            write (Reg32::TMR_ICR, ~0U);

            uint32 v1 = read (Reg32::TMR_CCR);
            uint32 t1 = static_cast<uint32>(rdtsc());
            Acpi::delay (delay);
            uint32 v2 = read (Reg32::TMR_CCR);
            uint32 t2 = static_cast<uint32>(rdtsc());

            freq_tsc = (t2 - t1) / delay;
            freq_bus = (v1 - v2) / delay;
            measured = true;
        }

        trace (0, "TSC:%u kHz BUS:%u kHz%s%s", freq_tsc, freq_bus, measured ? " (measured)" : "", dl ? " DL" : "");

        if (Cpu::online > 1) {
            send_exc (AP_BOOT_PADDR >> PAGE_BITS, Delivery::DLV_SIPI);
            Acpi::delay (1);
            send_exc (AP_BOOT_PADDR >> PAGE_BITS, Delivery::DLV_SIPI);
        }
    }

    write (Reg32::TMR_ICR, 0);

    // Enforce ordering between the LVT MMIO write that enables TSC deadline mode and later WRMSRs to IA32_TSC_DEADLINE
    Barrier::fmb();
}

bool Lapic::read_tsc_freq()
{
    if (Cpu::vendor != Cpu::Vendor::INTEL)
        return false;

    if (freq_tsc || freq_bus)
        return true;

    unsigned const model  = Cpu::model[Cpu::id];
    unsigned const family = Cpu::family[Cpu::id];

    bool const dl = Cpu::feature (Cpu::Feature::TSC_DEADLINE) && !Cmdline::nodl;

    enum { CPU_ID_CLOCK = 0x15 };

    uint32 eax = 0, ebx = 0, ecx = 0, edx = 0;
    Cpu::cpuid (0, eax, ebx, ecx, edx);

    if (eax >= CPU_ID_CLOCK) {
        /* Intel manual: 18.7.3 Determining the Processor Base Frequency */
        Cpu::cpuid (CPU_ID_CLOCK, eax, ebx, ecx, edx);

        if (eax && ebx) {
            if (ecx) {
                freq_tsc = static_cast<unsigned>((uint64(ecx) * ebx) / eax / 1000);
                return true;
            }

            if (family == 6) {
                if (model == 0x5c) /* Goldmont */
                    freq_tsc = static_cast<unsigned>((19200ull * ebx) / eax);
                if (model == 0x55) /* Xeon */
                    freq_tsc = static_cast<unsigned>((25000ull * ebx) / eax);
            }

            if (!freq_tsc && family >= 6)
                freq_tsc = static_cast<unsigned>((24000ull * ebx) / eax);

            if (freq_tsc)
                return true;
        }
    }

    if (family != 6)
        return false;

    if (model == 0x2a || model == 0x2d || /* Sandy Bridge */
        model >= 0x3a) { /* Ivy Bridge and later */
        uint64 ratio = (Msr::read (Msr::MSR_PLATFORM_INFO) >> 8) & 0xff;
        freq_tsc = static_cast<unsigned>(ratio * 100000);
        freq_bus = dl ? 0 : 100000;
    } else if (model == 0x1a || model == 0x1e || model == 0x1f || model == 0x2e || /* Nehalem */
               model == 0x25 || model == 0x2c || model == 0x2f) { /* Xeon Westmere */
        uint64 ratio = (Msr::read (Msr::MSR_PLATFORM_INFO) >> 8) & 0xff;
        freq_tsc = static_cast<unsigned>(ratio * 133330);
        freq_bus = dl ? 0 : 133330;
    } else if (model == 0x17 || model == 0xf) { /* Core 2 */
        freq_bus = Msr::read (Msr::MSR_FSB_FREQ) & 0x7;
        switch (freq_bus) {
            case 0b101: freq_bus = 100000; break;
            case 0b001: freq_bus = 133330; break;
            case 0b011: freq_bus = 166670; break;
            case 0b010: freq_bus = 200000; break;
            case 0b000: freq_bus = 266670; break;
            case 0b100: freq_bus = 333330; break;
            case 0b110: freq_bus = 400000; break;
            default:    freq_bus = 0;      break;
        }

        uint64 ratio = (Msr::read (Msr::IA32_PLATFORM_ID) >> 8) & 0x1f;

        freq_tsc = static_cast<unsigned>(freq_bus * ratio);
    }

    return freq_tsc;
}

void Lapic::handle_timer()
{
    bool expired = (freq_bus ? read (Reg32::TMR_CCR) : Msr::read (Msr::IA32_TSC_DEADLINE)) == 0;
    if (expired)
        Timeout::check();

    Rcu::update();
}

void Lapic::handle_error()
{
    write (Reg32::ESR, 0);
    write (Reg32::ESR, 0);
}

void Lapic::handle_perfm() {}

void Lapic::handle_therm() {}

void Lapic::handle_cmchk()
{
}

void Lapic::lvt_vector (unsigned vector)
{
    unsigned lvt = vector - VEC_LVT;

    switch (vector) {
        case VEC_LVT_TIMER: handle_timer(); break;
        case VEC_LVT_ERROR: handle_error(); break;
        case VEC_LVT_PERFM: handle_perfm(); break;
        case VEC_LVT_THERM: handle_therm(); break;
        case VEC_LVT_CMCHK: handle_cmchk(); break;
    }

    eoi();

    if (lvt < NUM_LVT)
        Counter::print<1,16> (++Counter::lvt[lvt], Console_vga::COLOR_LIGHT_BLUE, lvt + SPN_LVT);
}

void Lapic::ipi_vector (unsigned vector)
{
    unsigned ipi = vector - VEC_IPI;

    switch (vector) {
        case VEC_IPI_RRQ: Sc::rrq_handler(); break;
        case VEC_IPI_RKE: Sc::rke_handler(); break;
        case VEC_IPI_IDL: Ec::idl_handler(); break;
        case VEC_IPI_HLT:
            /* hlt handler does not return */
            ++Counter::ipi[ipi];
            Ec::hlt_handler();
            break;
    }

    eoi();

    if (ipi < NUM_IPI)
        Counter::print<1,16> (++Counter::ipi[ipi], Console_vga::COLOR_LIGHT_GREEN, ipi + SPN_IPI);
}

bool Lapic::hlt_other_cpus()
{
    bool success = true;

    for (cpu_t cpu = 0; cpu < NUM_CPU; cpu++) {

        if (!Hip::cpu_online (cpu))
            continue;

        if (Cpu::id == cpu)
            continue;

        unsigned ctr = Counter::remote (cpu, VEC_IPI_HLT - VEC_IPI);

        Lapic::send_cpu (VEC_IPI_HLT, cpu);

        bool sent = Lapic::pause_loop_until(500, [&] {
            return (Counter::remote (cpu, VEC_IPI_HLT - VEC_IPI) == ctr); });

        if (!sent) {
            trace (0, "IPI timeout hlt %u->%u", Cpu::id, cpu);
            success = false;
        }
    }

    return success;
}

extern "C" char __start_ap    [];
extern "C" char __start_ap_end[];

void Lapic::ap_code_manage(bool const prepare)
{
    static char backup[128] { };
    static bool valid { };

    if (!prepare) {

        if (!valid)
            return;

        memcpy(Hpt::remap (Pd::kern.quota, AP_BOOT_PADDR), backup, sizeof(backup));
        valid = false;

        return;
    }

    assert (static_cast<size_t>(__start_ap_end - __start_ap) < sizeof(backup));

    if (valid)
        return;

    void * const ap_ptr { Hpt::remap (Pd::kern.quota, AP_BOOT_PADDR)};

    memcpy(backup, ap_ptr,     sizeof(backup));
    memcpy(ap_ptr, __start_ap, sizeof(backup));

    valid = true;
}
