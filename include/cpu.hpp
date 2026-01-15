/*
 * Central Processing Unit (CPU)
 *
 * Copyright (C) 2009-2011 Udo Steinberg <udo@hypervisor.org>
 * Economic rights: Technische Universitaet Dresden (Germany)
 *
 * Copyright (C) 2012-2013 Udo Steinberg, Intel Corporation.
 * Copyright (C) 2014 Udo Steinberg, FireEye, Inc.
 * Copyright (C) 2019-2025 Udo Steinberg, BlueRock Security, Inc.
 * Copyright (C) 2015-2025 Alexander Boettcher, Genode Labs GmbH
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

#include "arch.hpp"
#include "compiler.hpp"
#include "config.hpp"
#include "kmem.hpp"
#include "types.hpp"
#include "assert.hpp"
#include "macros.hpp"
#include "memory.hpp"

class Cpu
{
    private:
        static constexpr apic_t invalid_topology { BIT_RANGE (31, 0) };

        // Must agree with enum class Vendor below
        static constexpr char const *vendor_string[] { "Unknown", "GenuineIntel", "AuthenticAMD" };

        ALWAYS_INLINE
        static inline void setup_pcid();

        static void enumerate_topology (uint32_t, uint32_t &, uint32_t (&)[4]);
        static void enumerate_features (uint32_t &, uint32_t &, uint32_t (&)[4], uint32_t (&)[12]);

        static void setup_msr();

    public:
        enum class Vendor : uint8_t
        {
            UNKNOWN,
            INTEL,
            AMD,
        };

        enum Core_type
        {
            INTEL_ATOM = 0x20,
            INTEL_CORE = 0x40,
        };

        enum Feature
        {
            // EAX=0x1 (ECX)
            MONITOR_MWAIT           =  0 * 32 +  3,     // MONITOR/MWAIT Support
            VMX                     =  0 * 32 +  5,     // Virtual Machine Extensions
            EIST                    =  0 * 32 +  7,     // Enhanced Intel SpeedStep Technology
            PCID                    =  0 * 32 + 17,     // Process Context Identifiers
            X2APIC                  =  0 * 32 + 21,     // x2APIC Support
            TSC_DEADLINE            =  0 * 32 + 24,     // TSC Deadline Support
            XSAVE                   =  0 * 32 + 26,     // XCR0, XSETBV/XGETBV/XSAVE/XRSTOR Instructions
            RDRAND                  =  0 * 32 + 30,     // RDRAND Instruction
            // EAX=0x1 (EDX)
            MCE                     =  1 * 32 +  7,     // Machine Check Exception
            SEP                     =  1 * 32 + 11,     // SYSENTER/SYSEXIT Instructions
            MCA                     =  1 * 32 + 14,     // Machine Check Architecture
            PAT                     =  1 * 32 + 16,     // Page Attribute Table
            ACPI                    =  1 * 32 + 22,     // Thermal Monitor and Software Controlled Clock Facilities
            HTT                     =  1 * 32 + 28,     // Hyper-Threading Technology
            // EAX=0x6 (EAX)
            CPU_TEMP                =  2 * 32 +  0,
            TURBO_BOOST             =  2 * 32 +  1,     // Turbo Boost Technology
            ARAT                    =  2 * 32 +  2,     // Always Running APIC Timer
            PKG_TEMP                =  2 * 32 +  6,
            HWP                     =  2 * 32 +  7,     // HWP Baseline Resource and Capability
            HWP_NTF                 =  2 * 32 +  8,     // HWP Notification
            HWP_ACT                 =  2 * 32 +  9,     // HWP Activity Window
            HWP_EPP                 =  2 * 32 + 10,     // HWP Energy Performance Preference
            HWP_PLR                 =  2 * 32 + 11,     // HWP Package Level Request
            HWP_CAP                 =  2 * 32 + 15,     // HWP Capabilities
            HWP_PECI                =  2 * 32 + 16,     // HWP PECI Override
            HWP_FLEX                =  2 * 32 + 17,     // HWP Flexible
            HWP_FAM                 =  2 * 32 + 18,     // HWP Fast Access Mode
            // EAX=0x7 ECX=0x0 (EBX)
            SGX                     =  3 * 32 +  2,     // Software Guard Extensions
            SMEP                    =  3 * 32 +  7,     // Supervisor Mode Execution Prevention
            RDT_M                   =  3 * 32 + 12,     // RDT Monitoring (PQM)
            RDT_A                   =  3 * 32 + 15,     // RDT Allocation (PQE)
            RDSEED                  =  3 * 32 + 18,     // RDSEED Instruction
            SMAP                    =  3 * 32 + 20,     // Supervisor Mode Access Prevention

            FEAT_HCFC           = 32 *  6,
            FEAT_EPB            = 32 *  6 + 3,
            FEAT_PSTATE_AMD     = 32 *  7 + 7,
            FEAT_TSC_INVARIANT  = 32 *  7 + 8,
            FEAT_MWAIT_EXT      = 32 *  8 + 0,
            FEAT_MWAIT_IRQ      = 32 *  8 + 1,
            FEAT_XSAVEOPT       = 32 * 10 + 0,
            FEAT_FPU_COMPACT    = 32 * 10 + 3,

            TME                     =  4 * 32 + 13,     // Total Memory Encryption
            PCONFIG                 =  5 * 32 + 18,     // PCONFIG Instruction
            // EAX=0x80000001 (ECX)
            CMP_LEGACY              = 11 * 32 +  1,
            SVM                     = 11 * 32 +  2,
            // EAX=0x80000001 (EDX)
            GB_PAGES                = 12 * 32 + 26,     // 1GB-Pages Support
            RDTSCP                  = 12 * 32 + 27,     // RDTSCP Instruction
            LM                      = 12 * 32 + 29,     // Long Mode Support
        };

        static mword    boot_lock           asm ("boot_lock");

        static unsigned online;
        static uint32   acpi_id[NUM_CPU];
        static bool     apic_x2[NUM_CPU];

        static uint8    package[NUM_CPU];
        static uint8    core[NUM_CPU];
        static uint8    thread[NUM_CPU];

        static uint8    platform[NUM_CPU];
        static uint8    family[NUM_CPU];
        static uint8    model[NUM_CPU];
        static uint8    stepping[NUM_CPU];
        static uint8    core_type[NUM_CPU];
        static unsigned patch[NUM_CPU];

        static cpu_t    id                  CPULOCAL_HOT;
        static apic_t   topology            CPULOCAL_HOT;
        static unsigned hazard              CPULOCAL_HOT;
        static Vendor   vendor              CPULOCAL;
        static unsigned brand               CPULOCAL;
        static unsigned row                 CPULOCAL;

        static uint32_t features[13]        CPULOCAL;
        static bool bsp                     CPULOCAL;
        static bool preemption              CPULOCAL;
        static unsigned mwait_hint          CPULOCAL;

        static void init(bool = false);

        ALWAYS_INLINE
        static inline bool feature (Feature f)
        {
            return features[f / 32] & 1U << f % 32;
        }

        ALWAYS_INLINE
        static inline void defeature (Feature f)
        {
            features[f / 32] &= ~(1U << f % 32);
        }

        ALWAYS_INLINE
        static inline void preempt_disable()
        {
            assert (preemption);

            asm volatile ("cli" : : : "memory");
            preemption = false;
        }

        ALWAYS_INLINE
        static inline void preempt_enable()
        {
            assert (!preemption);

            preemption = true;
            asm volatile ("sti" : : : "memory");
        }

        ALWAYS_INLINE
        static inline bool preempt_status()
        {
            mword flags = 0;
            asm volatile ("pushf; pop %0" : "=r" (flags));
            return flags & 0x200;
        }

        static void preemption_point()      { asm volatile ("sti; nop; cli" : : : "memory"); }

        static void cpuid (unsigned leaf, uint32_t &eax, uint32_t &ebx, uint32_t &ecx, uint32_t &edx)
        {
            asm volatile ("cpuid" : "=a" (eax), "=b" (ebx), "=c" (ecx), "=d" (edx) : "a" (leaf));
        }

        static void cpuid (unsigned leaf, unsigned subleaf, uint32_t &eax, uint32_t &ebx, uint32_t &ecx, uint32_t &edx)
        {
            asm volatile ("cpuid" : "=a" (eax), "=b" (ebx), "=c" (ecx), "=d" (edx) : "a" (leaf), "c" (subleaf));
        }

        static auto remote_topology (cpu_t c) { return *Kmem::loc_to_glb (c, &topology); }

        static auto find_by_topology (uint32_t t)
        {
            for (cpu_t c { 0 }; c < online; c++)
                if (remote_topology (c) == t)
                    return c;

            return static_cast<cpu_t>(-1);
        }

        static void halt_or_mwait(auto const &halt, auto const &mwait)
        {
            if (!Cpu::feature (Cpu::MONITOR_MWAIT) || mwait_hint == ~0U) {
                halt();
                return;
            }

            if (Cpu::feature (Cpu::FEAT_MWAIT_EXT))
                mwait(mwait_hint);
            else
                mwait(0u);
        }
};
