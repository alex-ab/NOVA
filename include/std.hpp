/*
 * C++ Standard Functions
 *
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

#include "types.hpp"

namespace std
{
    // See https://en.cppreference.com/w/cpp/types/conditional
    template<bool B, class T, class F> struct conditional {};
    template<        class T, class F> struct conditional<true,  T, F> { using type = T; };
    template<        class T, class F> struct conditional<false, T, F> { using type = F; };
    template<bool B, class T, class F> using  conditional_t = typename conditional<B,T,F>::type;

    // See https://en.cppreference.com/w/cpp/types/remove_reference
    template<typename T> struct remove_reference        { using type = T; };    // non-reference
    template<typename T> struct remove_reference<T&>    { using type = T; };    // lvalue reference
    template<typename T> struct remove_reference<T&&>   { using type = T; };    // rvalue reference
    template<typename T> using  remove_reference_t = typename remove_reference<T>::type;

    // See https://en.cppreference.com/w/cpp/utility/forward
    template<typename T> [[nodiscard]] constexpr T&& forward (remove_reference_t<T>&  t) noexcept { return static_cast<T&&>(t); }
    template<typename T> [[nodiscard]] constexpr T&& forward (remove_reference_t<T>&& t) noexcept { return static_cast<T&&>(t); }

    // See https://en.cppreference.com/w/cpp/utility/launder
    template<typename T> [[nodiscard]] constexpr T* launder (T* t) noexcept { return __builtin_launder (t); }

    // See https://en.cppreference.com/w/cpp/utility/move
    template<typename T> [[nodiscard]] constexpr remove_reference_t<T>&& move (T&& t) noexcept { return static_cast<remove_reference_t<T>&&>(t); }

    // See https://en.cppreference.com/w/cpp/algorithm/swap
    template<typename T> constexpr void swap (T& a, T& b) noexcept { T t { move (a) }; a = move (b); b = move (t); }

    // See https://en.cppreference.com/w/cpp/utility/to_underlying
    template<typename T> [[nodiscard]] constexpr auto to_underlying (T t) noexcept { return static_cast<__underlying_type (T)>(t); }
}

// See https://en.cppreference.com/w/cpp/memory/new/operator_new
[[nodiscard]] inline void *operator new   (size_t, void *p) noexcept { return p; }
[[nodiscard]] inline void *operator new[] (size_t, void *p) noexcept { return p; }
