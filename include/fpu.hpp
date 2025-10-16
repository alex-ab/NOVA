/*
 * Floating Point Unit (FPU)
 * Streaming SIMD Extensions (SSE)
 * Advanced Vector Extensions (AVX)
 *
 * Copyright (C) 2009-2011 Udo Steinberg <udo@hypervisor.org>
 * Economic rights: Technische Universitaet Dresden (Germany)
 *
 * Copyright (C) 2012-2013 Udo Steinberg, Intel Corporation.
 * Copyright (C) 2019-2024 Udo Steinberg, BedRock Systems, Inc.
 *
 * Copyright (C) 2018-2024 Alexander Boettcher
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

#include "cpu.hpp"
#include "hazards.hpp"
#include "slab.hpp"
#include "x86.hpp"
#include "pd.hpp"
#include "string.hpp"
#include "macros.hpp"

class Fpu
{
    private:

        // State Components
        enum Component
        {
            APX_F           = BIT (19),             // XCR (enabled)
            XTILEDATA       = BIT (18),             // XCR (enabled)
            XTILECFG        = BIT (17),             // XCR (enabled)
            HWP             = BIT (16),             // XSS (managed)
            LBR             = BIT (15),             // XSS (managed)
            UINTR           = BIT (14),             // XSS (managed)
            HDC             = BIT (13),             // XSS (managed)
            CET_S           = BIT (12),             // XSS (managed)
            CET_U           = BIT (11),             // XSS (managed)
            PASID           = BIT (10),             // XSS (managed)
            PKRU            = BIT (9),              // XCR (managed)
            PT              = BIT (8),              // XSS (managed)
            AVX512          = BIT_RANGE (7, 5),     // XCR (enabled)
            MPX             = BIT_RANGE (4, 3),     // XCR (enabled)
            AVX             = BIT (2),              // XCR (enabled)
            SSE             = BIT (1),              // XCR (managed)
            X87             = BIT (0),              // XCR (managed)
        };

        // Legacy Region: x87 State, SSE State
        struct Legacy
        {
            uint16_t    fcw             { 0x37f };  // x87 FPU Control Word
            uint16_t    fsw             { 0 };      // x87 FPU Status Word
            uint16_t    ftw             { 0 };      // x87 FPU Tag Word
            uint16_t    fop             { 0 };      // x87 FPU Opcode
            uint64_t    fip             { 0 };      // x87 FPU Instruction Pointer Offset
            uint64_t    fdp             { 0 };      // x87 FPU Instruction Data Pointer Offset
            uint32_t    mxcsr           { 0x1f80 }; // SIMD Control/Status Register
            uint32_t    mxcsr_mask      { 0 };      // SIMD Control/Status Register Mask Bits
            uint64_t    mmx[8][2]       {{0}};      //  8  80bit MMX registers
            uint64_t    xmm[16][2]      {{0}};      // 16 128bit XMM registers
            uint64_t    unused[6][2]    {{0}};      //  6 128bit reserved
        };

        static_assert (__is_standard_layout (Legacy) && sizeof (Legacy) == 512);

        // XSAVE Header
        struct Header
        {
            uint64_t    xstate          { 0 };
            uint64_t    xcomp           { static_cast<uint64_t>(compact) << 63 };
            uint64_t    unused[6]       { 0 };
        };

        static_assert (__is_standard_layout (Header) && sizeof (Header) == 64);

        // XSAVE Area
        Legacy  legacy { };
        Header  header { };

        char data [2559] { };
        bool no_compact  { };

        ALWAYS_INLINE
        static auto get_xcr (unsigned xcr)
        {
            uint32 hi, lo;

            asm volatile ("xgetbv" : "=d" (hi), "=a" (lo) : "c" (xcr));

            return (uint64(hi) << 32) | lo;
        }

        ALWAYS_INLINE
        static void set_xcr (unsigned xcr, uint64 val)
        {
            asm volatile ("xsetbv" : : "d" (uint32(val >> 32)),
                                       "a" (uint32(val)),
                                       "c" (xcr));
        }

    public:

        struct State_xsv
        {
            uint64_t    xcr             { Component::X87 };
            uint64_t    xss             { 0 };

            /*
             * Switch XSAVE state between guest/host
             *
             * VMM-provided guest state was sanitized by constrain_* functions below
             *
             * @param o     Old live state
             * @param n     New live state
             */
            ALWAYS_INLINE
            static inline void make_current (State_xsv const &o, State_xsv const &n)
            {
                if (!Cpu::feature (Cpu::Feature::XSAVE))
                    return;

                if (EXPECT_FALSE (o.xcr != n.xcr))
                    set_xcr (0, n.xcr);

                if (EXPECT_FALSE (o.xss != n.xss))
                    Msr::write (Msr::IA32_XSS, n.xss);
            }

            /*
             * Constrain XCR0 value to ensure XSETBV does not fault
             *
             * @param v     XCR0 value provided by VMM
             * @return      Constrained value
             */
            ALWAYS_INLINE
            static inline uint64_t constrain_xcr (uint64_t v)
            {
                // Setting any AVX512 bit requires all AVX512 bits and the AVX bit
                if (v & Component::AVX512)
                    v |= Component::AVX512 | Component::AVX;

                // Setting the AVX bit requires the SSE bit
                if (v & Component::AVX)
                    v |= Component::SSE;

                // The X87 bit is always required
                v |= Component::X87;

                // Constrain to bits that are manageable
                return hst_xsv.xcr & v;
            }

            /*
             * Constrain XSS value to ensure WRMSR does not fault
             *
             * @param v     XSS value provided by VMM
             * @return      Constrained value
             */
            ALWAYS_INLINE
            static inline uint64_t constrain_xss (uint64_t v)
            {
                // Constrain to bits that are manageable
                return hst_xsv.xss & v;
            }
        };

        static State_xsv hst_xsv CPULOCAL;

        // XSAVE area format: XSAVES/compact (true), XSAVE/standard (false)
        static inline constinit bool compact { false };

        // XSAVE context size
        static inline constinit size_t size { sizeof (Legacy) };

        // XSAVE context alignment
        static constexpr size_t alignment { 64 };

        // XSAVE state components managed by NOVA
        static constexpr uint64_t managed { Component::AVX512 | Component::AVX | Component::SSE | Component::X87 };

        void save();
        void load();

        /*
         * Disable FPU and clear FPU hazard
         */
        ALWAYS_INLINE
        static inline void disable()
        {
            set_cr0 (get_cr0() | Cpu::CR0_TS);

            Cpu::hazard &= ~HZD_FPU;
        }

        /*
         * Enable FPU and set FPU hazard
         */
        ALWAYS_INLINE
        static inline void enable()
        {
            asm volatile ("clts");

            Cpu::hazard |= HZD_FPU;
        }


        ALWAYS_INLINE
        static inline void *operator new (size_t, Pd &pd) { return pd.fpu_cache.alloc(pd.quota); }

        ALWAYS_INLINE
        static inline void destroy(Fpu *obj, Pd &pd) { obj->~Fpu(); pd.fpu_cache.free (obj, pd.quota); }

        ALWAYS_INLINE
        inline void export_data(void *p) { memcpy(p, this, Fpu::size); }

        ALWAYS_INLINE
        inline void import_data(void *p) { memcpy(this, p, Fpu::size); }

        static void init();
        static void probe();
};
