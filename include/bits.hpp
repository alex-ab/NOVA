/*
 * Bit Functions
 *
 * Copyright (C) 2009-2011 Udo Steinberg <udo@hypervisor.org>
 * Economic rights: Technische Universitaet Dresden (Germany)
 *
 * Copyright (C) 2012-2013 Udo Steinberg, Intel Corporation.
 * Copyright (C) 2019-2025 Udo Steinberg, BlueRock Security, Inc.
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

#include "macros.hpp"
#include "util.hpp"

/*
 * Determine the number of bits for type T
 */
template<typename T> static constexpr unsigned type_bits() { return 8 * sizeof (T); }

/*
 * Determine the most significant bit number for type T
 */
template<typename T> static constexpr unsigned type_msbn() { return type_bits<T>() - 1; }

/*
 * Determine the bit number of the most significant 1-bit
 */
static constexpr int bit_scan_msb (unsigned long v)
{
    return !v ? -1 : type_msbn<decltype (v)>() - __builtin_clzl (v);
}

/*
 * Determine the bit number of the least significant 1-bit
 */
static constexpr int bit_scan_lsb (unsigned long v)
{
    return !v ? -1 : __builtin_ctzl (v);
}

ALWAYS_INLINE
inline long int bit_scan_reverse (mword val)
{
    if (EXPECT_FALSE (!val))
        return -1;

    asm volatile ("bsr %1, %0" : "=r" (val) : "rm" (val));

    return val;
}

ALWAYS_INLINE
inline long int bit_scan_forward (mword val)
{
    if (EXPECT_FALSE (!val))
        return -1;

    asm volatile ("bsf %1, %0" : "=r" (val) : "rm" (val));

    return val;
}

ALWAYS_INLINE
inline unsigned long max_order (mword base, size_t size)
{
    long int o = bit_scan_reverse (size);

    if (base)
        o = min (bit_scan_forward (base), o);

    return o;
}

ALWAYS_INLINE
inline uint64 div64 (uint64 n, uint32 d, uint32 *r)
{
    uint64 q;

#ifdef __i386__
    asm volatile ("divl %5;"
                  "xchg %1, %2;"
                  "divl %5;"
                  "xchg %1, %3;"
                  : "=A" (q),   // quotient
                    "=r" (*r)   // remainder
                  : "a"  (static_cast<uint32>(n >> 32)),
                    "d"  (0),
                    "1"  (static_cast<uint32>(n)),
                    "rm" (d));
#else
     q = n / d;
    *r = static_cast<uint32>(n % d);
#endif

    return q;
}

/*
 * Determine the largest order under size and alignment constraints
 *
 * @param size  Must be non-zero and >= 2^o
 * @param addr  Must be a multiple of 2^o
 * @return      The largest o that satisfies the above constraints
 */
template<typename... T> static constexpr unsigned aligned_order (size_t size, T... addr)
{
    return min (bit_scan_msb (size), bit_scan_lsb (BITN (type_msbn<uintptr_t>()) | (addr | ...)));
}

/*
 * Align value down
 *
 * @param a     Alignment (must be a power of 2)
 * @param v     Value
 * @return      Aligned value
 */
static constexpr auto aligned_dn (uintptr_t a, uintptr_t v)
{
    v &= ~(a - 1);
    return v;
}

/*
 * Align value up
 *
 * @param a     Alignment (must be a power of 2)
 * @param v     Value
 * @return      Aligned value
 */
static constexpr auto aligned_up (uintptr_t a, uintptr_t v)
{
    v += (a - 1);
    return aligned_dn (a, v);
}

static constexpr auto align_up (uintptr_t val, uintptr_t align)
{
    return aligned_up(align, val);
}

// Sanity checks
static_assert (type_msbn<uint16_t>() == 15);
static_assert (type_msbn<uint32_t>() == 31);
static_assert (type_msbn<uint64_t>() == 63);
static_assert (bit_scan_lsb (0) == -1);
static_assert (bit_scan_msb (0) == -1);
#ifdef __x86_64__
static_assert (bit_scan_lsb (BIT64_RANGE (55, 5)) == 5);
static_assert (bit_scan_msb (BIT64_RANGE (55, 5)) == 55);
#endif
