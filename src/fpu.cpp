/*
 * Floating Point Unit (FPU)
 * Streaming SIMD Extensions (SSE)
 * Advanced Vector Extensions (AVX)
 *
 * Copyright (C) 2019 Julian Stecklina, Cyberus Technology GmbH.
 * Copyright (C) 2019-2024 Udo Steinberg, BedRock Systems, Inc.
 * Copyright (C) 2024 Alexander Boettcher
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

#include "fpu.hpp"

Fpu::State_xsv Fpu::hst_xsv;

ALIGNED(Fpu::alignment) static Fpu empty;

void Fpu::init()
{
    empty.load();
}

void Fpu::save()
{
#ifdef __x86_64__
    if (Cpu::feature (Cpu::Feature::XSAVE)) {
        if (compact && !no_compact)
            asm volatile ("xsaves64 %0" : "=m" (*this)
                                        :  "d" (unsigned(managed >> 32)),
                                           "a" (unsigned(managed))
                                        : "memory");
        else
        if (Cpu::feature (Cpu::FEAT_XSAVEOPT))
            asm volatile ("xsaveopt %0" : "=m" (*this)
                                        :  "d" (unsigned(managed >> 32)),
                                           "a" (unsigned(managed))
                                        : "memory");
        else
            asm volatile ("xsave64 %0" : "=m" (*this)
                                       :  "d" (unsigned(managed >> 32)),
                                          "a" (unsigned(managed))
                                       : "memory");
    }
    else
#endif
        asm volatile ("fxsave %0" : "=m" (*this));
}

/* catch GP in case FPU state of vCPU of VM is bad */
#define FIXUP(insn)                                           \
    "clc\n"                                                   \
    "1: " insn "; 2:\n"                                       \
    ".section .fixup,\"a\"; .align 8;" EXPAND(WORD) " 1b,2b; .previous"

void Fpu::load()
{
    bool bad = false;

#ifdef __x86_64__
    if (Cpu::feature (Cpu::Feature::XSAVE))
        if (compact && !no_compact)
            asm volatile (FIXUP("xrstors64  %1")
                          : "=@ccc"(bad)
                          : "m" (*this),
                            "d" (unsigned(managed >> 32)),
                            "a" (unsigned(managed))
                          : "memory");
        else
            asm volatile (FIXUP("xrstor64  %1")
                          : "=@ccc"(bad)
                          : "m" (*this),
                            "d" (unsigned(managed >> 32)),
                            "a" (unsigned(managed))
                          : "memory");
    else
#endif
        asm volatile (FIXUP("fxrstor %1") : "=@ccc"(bad) : "m" (*this));

    if (bad) {
        if (Cpu::feature (Cpu::Feature::XSAVE) && compact)
            no_compact = true;

        Fpu::init();
    }
}

void Fpu::probe()
{
    if (Cpu::bsp)
        empty = Fpu();

    if (!Cpu::feature (Cpu::Feature::XSAVE))
        return;

    // Enable supervisor state components in IA32_XSS
    if (compact)
        Msr::write (Msr::IA32_XSS, hst_xsv.xss);

    // Enable user state components in XCR0
    set_xcr (0, hst_xsv.xcr);

    unsigned size = 0, dummy = 0; 
    Cpu::cpuid (0xd, compact, dummy, size, dummy, dummy);

    /* Use largest context size reported by any CPU */
    Fpu::size = max (Fpu::size, static_cast<size_t>(size));

    static_assert (sizeof(Fpu) == sizeof(Fpu::legacy) + sizeof(Fpu::header) +
                                  sizeof(Fpu::no_compact) + sizeof(Fpu::data));

    if (Fpu::size > sizeof(Fpu) - sizeof(Fpu::no_compact)) {
        trace(0, "FPU: size %zu too large -> use legacy X87 FPU", Fpu::size);
        Cpu::defeature (Cpu::Feature::XSAVE);
        Fpu::size = 512;
    }
}
