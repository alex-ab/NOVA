/*
 * Bit Functions
 *
 * Copyright (C) 2009-2011 Udo Steinberg <udo@hypervisor.org>
 * Economic rights: Technische Universitaet Dresden (Germany)
 *
 * Copyright (C) 2012-2013 Udo Steinberg, Intel Corporation.
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

#pragma once

#include "macros.hpp"
#include "types.hpp"
#include "util.hpp"

/*
 * Compute the bit index of the least significant 1-bit
 */
static constexpr int bit_scan_lsb (unsigned long v)
{
    return !v ? -1 : __builtin_ctzl (v);
}

/*
 * Compute the bit index of the most significant 1-bit
 */
static constexpr int bit_scan_msb (unsigned long v)
{
    return !v ? -1 : 8 * sizeof (v) - 1 - __builtin_clzl (v);
}

/*
 * Compute the largest order under size and alignment constraints
 *
 * @param size  Must be non-zero and >= 2^o
 * @param addr  Must be a multiple of 2^o
 * @return      The largest o that satisfies the above constraints
 */
static constexpr unsigned aligned_order (size_t size, uintptr_t addr)
{
    auto o { bit_scan_msb (size) };

    if (addr) [[likely]]
        o = min (o, bit_scan_lsb (addr));

    return o;
}

/*
 * Compute the largest order under size and alignment constraints
 *
 * @param size  Must be non-zero and >= 2^o
 * @param addr1 Must be a multiple of 2^o
 * @param addr2 Must be a multiple of 2^o
 * @return      The largest o that satisfies the above constraints
 */
static constexpr unsigned aligned_order (size_t size, uintptr_t addr1, uintptr_t addr2)
{
    auto o { bit_scan_msb (size) };

    if (addr1) [[likely]]
        o = min (o, bit_scan_lsb (addr1));

    if (addr2) [[likely]]
        o = min (o, bit_scan_lsb (addr2));

    return o;
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

// Sanity checks
static_assert (bit_scan_lsb (0) == -1);
static_assert (bit_scan_msb (0) == -1);
static_assert (bit_scan_lsb (BIT64_RANGE (55, 5)) == 5);
static_assert (bit_scan_msb (BIT64_RANGE (55, 5)) == 55);
