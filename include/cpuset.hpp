/*
 * CPU Set
 *
 * Copyright (C) 2009-2011 Udo Steinberg <udo@hypervisor.org>
 * Economic rights: Technische Universitaet Dresden (Germany)
 *
 * Copyright (C) 2012 Udo Steinberg, Intel Corporation.
 * Copyright (C) 2012 Alexander Boettcher, Genode Labs GmbH.
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

#include "atomic.hpp"
#include "types.hpp"

class Cpuset
{
    private:

        enum { CPUS_PER_VALUE = sizeof(mword) * 8 };

        static_assert (NUM_CPU > 0, "Pointless CPU configuration");

        mword raw [1 + (NUM_CPU - 1) / CPUS_PER_VALUE] { };

        ALWAYS_INLINE
        inline mword & value(unsigned const cpu) {
            return raw[cpu / CPUS_PER_VALUE]; }

        ALWAYS_INLINE
        inline mword const & value(unsigned const cpu) const {
            return raw[cpu / CPUS_PER_VALUE]; }

        ALWAYS_INLINE
        inline mword bit_cpu(unsigned const cpu) const {
            return cpu % CPUS_PER_VALUE; }

    public:

        ALWAYS_INLINE
        inline explicit Cpuset(mword const v)
        {
            for (unsigned i = 0; i < sizeof(raw) / sizeof(raw[0]); i++)
                raw[i] = v;
        }

        ALWAYS_INLINE
        inline bool chk (unsigned const cpu) const {
            return value(cpu) & (1UL << bit_cpu(cpu)); }

        ALWAYS_INLINE
        inline bool set (unsigned const cpu) {
            return !Atomic_legacy::test_set_bit (value(cpu), bit_cpu(cpu)); }

        ALWAYS_INLINE
        inline void clr (unsigned const cpu) {
            Atomic_legacy::clr_mask (value(cpu), 1UL << bit_cpu(cpu)); }

        ALWAYS_INLINE
        inline void merge (Cpuset const &s)
        {
            for (unsigned i = 0; i < sizeof(raw) / sizeof(raw[0]); i++)
                Atomic_legacy::set_mask (  value(i * CPUS_PER_VALUE),
                                         s.value(i * CPUS_PER_VALUE));
        }
};
