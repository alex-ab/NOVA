/*
 * Guarded Model-Specific Registers (MSR) access
 *
 * Copyright (C) 2021-2023 Alexander Boettcher
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

#include "msr.hpp"
#include "utcb.hpp"
#include "utcb.hpp"

Kobject * Msr::msr_cap {};

void Msr::user_access(Utcb &utcb)
{
    if (Cpu::vendor == Cpu::Vendor::INTEL)
        user_access_intel(utcb);
    else
    if (Cpu::vendor == Cpu::Vendor::AMD)
        user_access_amd(utcb);
}

void Msr::user_access_amd(Utcb &utcb)
{
    if (Cpu::vendor != Cpu::Vendor::AMD)
        return;

    utcb.for_each_word([](uint64 &msr) {

        switch (msr) {
        case Msr::IA32_APERF:
            if (!Cpu::feature(Cpu::Feature::FEAT_HCFC)) return false;
            break;
        case Msr::IA32_MPERF:
            if (!Cpu::feature(Cpu::Feature::FEAT_HCFC)) return false;
            break;
        case Msr::AMD_PSTATE_LIMIT:
            if (!Cpu::feature(Cpu::Feature::FEAT_PSTATE_AMD)) return false;
            break;
        case Msr::AMD_PSTATE_CTRL:
            if (!Cpu::feature(Cpu::Feature::FEAT_PSTATE_AMD)) return false;
            break;
        case Msr::AMD_PSTATE_STATUS:
            if (!Cpu::feature(Cpu::Feature::FEAT_PSTATE_AMD)) return false;
            break;
        default:
            return false;
        }

        return Msr::guard_read(static_cast<enum Register>(msr), msr);

    }, [](uint64 const &msr, uint64 const &value) {

        auto write_value = value;

        switch (msr) {
        case Msr::AMD_PSTATE_CTRL:
            if (!Cpu::feature(Cpu::Feature::FEAT_PSTATE_AMD)) return false;
            write_value &= 0xfull;
            break;
        default:
            return false;
        }

        return Msr::guard_write(static_cast<enum Register>(msr), write_value);
    });
}

void Msr::user_access_intel(Utcb &utcb)
{
    if (Cpu::vendor != Cpu::Vendor::INTEL)
        return;

    utcb.for_each_word([](uint64 &msr) {

        switch (msr) {
        case Msr::IA32_APERF:
            if (!Cpu::feature(Cpu::Feature::FEAT_HCFC)) return false;
            break;
        case Msr::IA32_MPERF:
            if (!Cpu::feature(Cpu::Feature::FEAT_HCFC)) return false;
            break;
        case Msr::IA32_THERM_STATUS:
            if (!Cpu::feature(Cpu::Feature::FEAT_CPU_TEMP)) return false;
            break;
        case Msr::IA32_THERM_PKG_STATUS:
            if (!Cpu::feature(Cpu::Feature::FEAT_PKG_TEMP)) return false;
            break;
        case Msr::MSR_TEMPERATURE_TARGET:
            if (!Cpu::feature(Cpu::Feature::FEAT_CPU_TEMP)) return false;
            break;
        case Msr::IA32_ENERGY_PERF_BIAS:
            if (!Cpu::feature(Cpu::Feature::FEAT_EPB)) return false;
            break;
        case Msr::IA32_PM_ENABLE:
            if (!Cpu::feature(Cpu::Feature::FEAT_HWP_7)) return false;
            break;
        case Msr::IA32_HWP_CAPABILITIES:
            if (!Cpu::feature(Cpu::Feature::FEAT_HWP_7)) return false;
            if (!(Msr::read<uint64>(Msr::IA32_PM_ENABLE) & 1)) return false;
            break;
        case Msr::IA32_HWP_REQUEST_PKG:
            if (!Cpu::feature(Cpu::Feature::FEAT_HWP_11)) return false;
            if (!(Msr::read<uint64>(Msr::IA32_PM_ENABLE) & 1)) return false;
            break;
        case Msr::IA32_HWP_REQUEST:
            if (!Cpu::feature(Cpu::Feature::FEAT_HWP_7)) return false;
            if (!(Msr::read<uint64>(Msr::IA32_PM_ENABLE) & 1)) return false;
            break;

        case MSR_CORE_C1_RESIDENCY:
        case MSR_CORE_C3_RESIDENCY:
        case MSR_CORE_C6_RESIDENCY:
        case MSR_CORE_C7_RESIDENCY:

        case MSR_PKG_C2_RESIDENCY:
        case MSR_PKG_C3_RESIDENCY:
        case MSR_PKG_C6_RESIDENCY:
        case MSR_PKG_C7_RESIDENCY:
        case MSR_PKG_C8_RESIDENCY:
        case MSR_PKG_C9_RESIDENCY:
        case MSR_PKG_C10_RESIDENCY:
            if (!Cpu::feature (Cpu::FEAT_MONITOR_MWAIT)) return false;
            if (!Cpu::feature (Cpu::FEAT_MWAIT_EXT))     return false;
            break;

        case MSR_RAPL_POWER_UNIT:

        case MSR_PKG_POWER_LIMIT:
        case MSR_PKG_ENERGY_STATUS:
        case MSR_PKG_PERF_STATUS:
        case MSR_PKG_POWER_INFO:

        case MSR_DRAM_POWER_LIMIT:
        case MSR_DRAM_ENERGY_STATUS:
        case MSR_DRAM_PERF_STATUS:
        case MSR_DRAM_POWER_INFO:

        case MSR_PP0_POWER_LIMIT:
        case MSR_PP0_ENERGY_STATUS:
        case MSR_PP0_POLICY:
        case MSR_PP0_PERF_STATUS:

        case MSR_PP1_POWER_LIMIT:
        case MSR_PP1_ENERGY_STATUS:
        case MSR_PP1_POLICY:
            break;

        default:
            return false;
        }

        return Msr::guard_read(static_cast<enum Register>(msr), msr);

    }, [](uint64 const &msr, uint64 const &value) {

        auto write_value = value;

        switch (msr) {
        case Msr::DUMMY_MWAIT_HINT:
            Cpu::mwait_hint = unsigned(write_value);
            return true;
        case Msr::IA32_PM_ENABLE:
            if (!Cpu::feature(Cpu::Feature::FEAT_HWP_7)) return false;
            write_value &= 1ull;
            break;
        case Msr::IA32_HWP_REQUEST:
            if (!Cpu::feature(Cpu::Feature::FEAT_HWP_7)) return false;
            if (!(Msr::read<uint64>(Msr::IA32_PM_ENABLE) & 1)) return false;
            break;
        case Msr::IA32_ENERGY_PERF_BIAS:
            if (!Cpu::feature(Cpu::Feature::FEAT_EPB)) return false;
            break;
        case Msr::IA32_HWP_REQUEST_PKG:
            if (!Cpu::feature(Cpu::Feature::FEAT_HWP_11)) return false;
            if (!(Msr::read<uint64>(Msr::IA32_PM_ENABLE) & 1)) return false;
            break;
        default:
            return false;
        }

        return Msr::guard_write(static_cast<enum Register>(msr), write_value);
    });
}
