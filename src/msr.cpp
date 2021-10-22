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

    utcb.for_each_word([](mword &msr) {
        switch (msr) {
        case Msr::IA32_APERF:
            if (!Cpu::feature(Cpu::Feature::FEAT_HCFC)) return false;
            msr = Msr::read<uint64>(Msr::IA32_APERF);
            return true;
        case Msr::IA32_MPERF:
            if (!Cpu::feature(Cpu::Feature::FEAT_HCFC)) return false;
            msr = Msr::read<uint64>(Msr::IA32_MPERF);
            return true;
        case Msr::AMD_PSTATE_LIMIT:
            if (!Cpu::feature(Cpu::Feature::FEAT_PSTATE_AMD)) return false;
            msr = Msr::read<uint64>(Msr::AMD_PSTATE_LIMIT);
            return true;
        case Msr::AMD_PSTATE_CTRL:
            if (!Cpu::feature(Cpu::Feature::FEAT_PSTATE_AMD)) return false;
            msr = Msr::read<uint64>(Msr::AMD_PSTATE_CTRL);
            return true;
        case Msr::AMD_PSTATE_STATUS:
            if (!Cpu::feature(Cpu::Feature::FEAT_PSTATE_AMD)) return false;
            msr = Msr::read<uint64>(Msr::AMD_PSTATE_STATUS);
            return true;
        default:
            return false;
        }
    }, [](mword const &msr, mword const &value) {
        switch (msr) {
        case Msr::AMD_PSTATE_CTRL:
            if (!Cpu::feature(Cpu::Feature::FEAT_PSTATE_AMD)) return false;
            Msr::write<uint64>(Msr::AMD_PSTATE_CTRL, value & 0xf);
            return true;
        default:
            return false;
        }
    });
}

void Msr::user_access_intel(Utcb &utcb)
{
    if (Cpu::vendor != Cpu::Vendor::INTEL)
        return;

    utcb.for_each_word([](mword &msr) {
        switch (msr) {
        case Msr::IA32_APERF:
            if (!Cpu::feature(Cpu::Feature::FEAT_HCFC)) return false;
            msr = Msr::read<uint64>(Msr::IA32_APERF);
            return true;
        case Msr::IA32_MPERF:
            if (!Cpu::feature(Cpu::Feature::FEAT_HCFC)) return false;
            msr = Msr::read<uint64>(Msr::IA32_MPERF);
            return true;

        case Msr::IA32_THERM_STATUS:
            if (!Cpu::feature(Cpu::Feature::FEAT_CPU_TEMP)) return false;
            msr = Msr::read<uint64>(Msr::IA32_THERM_STATUS);
            return true;
        case Msr::IA32_THERM_PKG_STATUS:
            if (!Cpu::feature(Cpu::Feature::FEAT_PKG_TEMP)) return false;
            msr = Msr::read<uint64>(Msr::IA32_THERM_PKG_STATUS);
            return true;
        case Msr::MSR_TEMPERATURE_TARGET:
            if (!Cpu::feature(Cpu::Feature::FEAT_CPU_TEMP)) return false;
            msr = Msr::read<uint64>(Msr::MSR_TEMPERATURE_TARGET);
            return true;
        case Msr::IA32_ENERGY_PERF_BIAS:
            if (!Cpu::feature(Cpu::Feature::FEAT_EPB)) return false;
            msr = Msr::read<uint64>(Msr::IA32_ENERGY_PERF_BIAS);
            return true;

        case Msr::IA32_PM_ENABLE:
            if (!Cpu::feature(Cpu::Feature::FEAT_HWP_7)) return false;
            msr = Msr::read<uint64>(Msr::IA32_PM_ENABLE);
            return true;
        case Msr::IA32_HWP_CAPABILITIES:
            if (!Cpu::feature(Cpu::Feature::FEAT_HWP_7)) return false;
            if (!(Msr::read<uint64>(Msr::IA32_PM_ENABLE) & 1)) return false;
            msr = Msr::read<uint64>(Msr::IA32_HWP_CAPABILITIES);
            return true;
        case Msr::IA32_HWP_REQUEST_PKG:
            if (!Cpu::feature(Cpu::Feature::FEAT_HWP_11)) return false;
            if (!(Msr::read<uint64>(Msr::IA32_PM_ENABLE) & 1)) return false;
            msr = Msr::read<uint64>(Msr::IA32_HWP_REQUEST_PKG);
            return true;
        case Msr::IA32_HWP_REQUEST:
            if (!Cpu::feature(Cpu::Feature::FEAT_HWP_7)) return false;
            if (!(Msr::read<uint64>(Msr::IA32_PM_ENABLE) & 1)) return false;
            msr = Msr::read<uint64>(Msr::IA32_HWP_REQUEST);
            return true;

        default:
            return false;
        }
    }, [](mword const &msr, mword const &value) {
        switch (msr) {
        case Msr::IA32_PM_ENABLE:
            if (!Cpu::feature(Cpu::Feature::FEAT_HWP_7)) return false;
            Msr::write<uint64>(Msr::IA32_PM_ENABLE, value & 1);
            return true;
        case Msr::IA32_HWP_REQUEST:
            if (!Cpu::feature(Cpu::Feature::FEAT_HWP_7)) return false;
            if (!(Msr::read<uint64>(Msr::IA32_PM_ENABLE) & 1)) return false;
            Msr::write<uint64>(Msr::IA32_HWP_REQUEST, value);
            return true;
        case Msr::IA32_ENERGY_PERF_BIAS:
            if (!Cpu::feature(Cpu::Feature::FEAT_EPB)) return false;
            Msr::write<uint64>(Msr::IA32_ENERGY_PERF_BIAS, value);
            return true;
        case Msr::IA32_HWP_REQUEST_PKG:
            if (!Cpu::feature(Cpu::Feature::FEAT_HWP_11)) return false;
            if (!(Msr::read<uint64>(Msr::IA32_PM_ENABLE) & 1)) return false;
            Msr::write<uint64>(Msr::IA32_HWP_REQUEST_PKG, value);
            return true;
        default:
            return false;
        }
    });
}
