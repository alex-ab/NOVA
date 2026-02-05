/*
 * Atomic Operations (Legacy) and Atomic Variables (upstream)
 *
 * Copyright (C) 2009-2011 Udo Steinberg <udo@hypervisor.org>
 * Economic rights: Technische Universitaet Dresden (Germany)
 *
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

#include "compiler.hpp"
#include "std.hpp"

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Weffc++"

/*
 * Atomic Type T
 */
template<typename T, int L = __ATOMIC_RELAXED, int S = __ATOMIC_RELAXED, typename = void> class Atomic;

/*
 * Atomic Integral Type T
 */
template<typename T, int L, int S> class Atomic<T, L, S, std::enable_if_t<std::is_integral<T>::value>> final
{
    private:
        T val;

    public:
        constexpr Atomic() = default;

        constexpr Atomic (T v) : val { v } {}

        ALWAYS_INLINE inline T load (int m = L) const
        {
            return __atomic_load_n (&val, m);
        }

        ALWAYS_INLINE inline void store (T v, int m = S)
        {
            __atomic_store (&val, &v, m);
        }

        ALWAYS_INLINE inline operator T() const { return load(); }
        ALWAYS_INLINE inline T operator= (T v)  { store (v); return v; }

        ALWAYS_INLINE inline T operator++()     { return __atomic_add_fetch (&val, 1, __ATOMIC_ACQ_REL); }
        ALWAYS_INLINE inline T operator--()     { return __atomic_sub_fetch (&val, 1, __ATOMIC_ACQ_REL); }
        ALWAYS_INLINE inline T operator+= (T v) { return __atomic_add_fetch (&val, v, __ATOMIC_ACQ_REL); }
        ALWAYS_INLINE inline T operator-= (T v) { return __atomic_sub_fetch (&val, v, __ATOMIC_ACQ_REL); }
        ALWAYS_INLINE inline T operator^= (T v) { return __atomic_xor_fetch (&val, v, __ATOMIC_ACQ_REL); }
        ALWAYS_INLINE inline T operator|= (T v) { return __atomic_or_fetch  (&val, v, __ATOMIC_ACQ_REL); }
        ALWAYS_INLINE inline T operator&= (T v) { return __atomic_and_fetch (&val, v, __ATOMIC_ACQ_REL); }

        ALWAYS_INLINE inline T operator++(int)  { return __atomic_fetch_add (&val, 1, __ATOMIC_ACQ_REL); }
        ALWAYS_INLINE inline T operator--(int)  { return __atomic_fetch_sub (&val, 1, __ATOMIC_ACQ_REL); }
        ALWAYS_INLINE inline T fetch_add (T v)  { return __atomic_fetch_add (&val, v, __ATOMIC_ACQ_REL); }
        ALWAYS_INLINE inline T fetch_sub (T v)  { return __atomic_fetch_sub (&val, v, __ATOMIC_ACQ_REL); }
        ALWAYS_INLINE inline T fetch_xor (T v)  { return __atomic_fetch_xor (&val, v, __ATOMIC_ACQ_REL); }
        ALWAYS_INLINE inline T fetch_or  (T v)  { return __atomic_fetch_or  (&val, v, __ATOMIC_ACQ_REL); }
        ALWAYS_INLINE inline T fetch_and (T v)  { return __atomic_fetch_and (&val, v, __ATOMIC_ACQ_REL); }

        ALWAYS_INLINE inline void exchange           (T &o, T &n) {        __atomic_exchange           (&val, &n, &o,        __ATOMIC_ACQ_REL); }
        ALWAYS_INLINE inline T    exchange_n         (      T  n) { return __atomic_exchange_n         (&val,  n,            __ATOMIC_ACQ_REL); }
        ALWAYS_INLINE inline bool compare_exchange   (T &o, T &n) { return __atomic_compare_exchange   (&val, &o, &n, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE); }
        ALWAYS_INLINE inline bool compare_exchange_n (T &o, T  n) { return __atomic_compare_exchange_n (&val, &o,  n, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE); }

        ALWAYS_INLINE inline T test_and_set (T v) { return fetch_or   (v) & v; }
        ALWAYS_INLINE inline T test_and_clr (T v) { return fetch_and (~v) & v; }

        Atomic            (Atomic const &) = delete;
        Atomic& operator= (Atomic const &) = delete;
};

/*
 * Atomic Non-Integral Type T
 */
template<typename T, int L, int S> class Atomic<T, L, S, std::enable_if_t<!std::is_integral<T>::value>> final
{
    private:
        T val;

    public:
        constexpr Atomic() = default;

        constexpr Atomic (T v) : val { v } {}

        ALWAYS_INLINE inline T load (int m = L) const
        {
            T v;
            __atomic_load (&val, &v, m);
            return v;
        }

        ALWAYS_INLINE inline void store (T v, int m = S)
        {
            __atomic_store (&val, &v, m);
        }

        ALWAYS_INLINE inline operator T() const     { return load(); }
        ALWAYS_INLINE inline T operator->() const   { return load(); }
        ALWAYS_INLINE inline T operator= (T v)      { store (v); return v; }

        ALWAYS_INLINE inline void exchange         (T &o, T &n) {        __atomic_exchange         (&val, &n, &o,        __ATOMIC_ACQ_REL); }
        ALWAYS_INLINE inline bool compare_exchange (T &o, T &n) { return __atomic_compare_exchange (&val, &o, &n, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE); }

        Atomic            (Atomic const &) = delete;
        Atomic& operator= (Atomic const &) = delete;
};

#pragma GCC diagnostic pop


class Atomic_legacy
{
    public:
        template <typename T>
        ALWAYS_INLINE
        static inline bool cmp_swap (T &ptr, T o, T n) { return __sync_bool_compare_and_swap (&ptr, o, n); }

        template <typename T>
        ALWAYS_INLINE
        static inline T add (T &ptr, T v) { return __sync_add_and_fetch (&ptr, v); }

        template <typename T>
        ALWAYS_INLINE
        static inline T sub (T &ptr, T v) { return __sync_sub_and_fetch (&ptr, v); }

        template <typename T>
        ALWAYS_INLINE
        static inline void set_mask (T &ptr, T v) { __sync_or_and_fetch (&ptr, v); }

        template <typename T>
        ALWAYS_INLINE
        static inline void clr_mask (T &ptr, T v) { __sync_and_and_fetch (&ptr, ~v); }

        template <typename T>
        ALWAYS_INLINE
        static inline bool test_set_bit (T &val, unsigned long bit)
        {
            bool ret;
            asm volatile ("lock; bts%z1 %2, %1; setc %0" : "=q" (ret), "+m" (val) : "ir" (bit) : "cc");
            return ret;
        }

        template <typename T>
        ALWAYS_INLINE
        static inline bool test_clr_bit (T &val, unsigned long bit)
        {
            bool ret;
            asm volatile ("lock; btr%z1 %2, %1; setc %0" : "=q" (ret), "+m" (val) : "ir" (bit) : "cc");
            return ret;
        }
};
