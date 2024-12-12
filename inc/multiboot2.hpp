/*
 * Multiboot2 support
 *
 * Copyright (C) 2017-2024 Alexander Boettcher, Genode Labs GmbH
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
#include "bits.hpp"

namespace Multiboot2
{
    class Header;
    class Framebuffer;
    class Memory_map;
    class Systab_64;
    class Module;
    class Tag;

    enum {
        MAGIC         = 0x36d76289,
        TAG_END       = 0,
        TAG_CMDLINE   = 1,
        TAG_MODULE    = 3,
        TAG_MEMORY    = 6,
        TAG_FB        = 8,
        TAG_SYSTAB_64 = 12,
        TAG_ACPI_1    = 14,
        TAG_ACPI_2    = 15,
    };

};

class Multiboot2::Memory_map
{
    public:

        uint64_t addr;
        uint64_t len;
        uint32_t type;
        uint32_t reserved;
};

class Multiboot2::Tag
{
    public:

        uint32_t  type;
        uint32_t  size;

        inline const char * cmdline() const
        {
            if (type != TAG_CMDLINE)
                return nullptr;

            return reinterpret_cast<const char *>(this + 1);
        }

        inline Framebuffer const * framebuffer() const
        {
            if (type != TAG_FB)
                return nullptr;

            return reinterpret_cast<Framebuffer *>(reinterpret_cast<uintptr_t>(this + 1));
        }

        inline Systab_64 const * systab_64() const
        {
            if (type != TAG_SYSTAB_64)
                return nullptr;

            return reinterpret_cast<Systab_64 *>(reinterpret_cast<uintptr_t>(this + 1));
        }

        inline Module const * module() const
        {
            if (type != TAG_MODULE)
                return nullptr;

            return reinterpret_cast<Module *>(reinterpret_cast<uintptr_t>(this + 1));
        }

        inline uintptr_t rsdp() const
        {
            if (type != TAG_ACPI_2 && type != TAG_ACPI_1)
                return 0;
           
            return reinterpret_cast<uintptr_t>(this + 1);
        }

        template <typename FUNC>
        inline void for_each_mem(FUNC const &fn) const
        {
            if (type != TAG_MEMORY)
                return;

            Memory_map const * s = reinterpret_cast<Memory_map *>(reinterpret_cast<uintptr_t>(this + 1) + 8);
            Memory_map const * e = reinterpret_cast<Memory_map *>(reinterpret_cast<uintptr_t>(this) + size);

            for (Memory_map const * i = s; i < e; i++) fn(i);
        }
};

class Multiboot2::Module
{
    public:

        uint32_t s_addr;
        uint32_t e_addr;
        char string [1];
};

class Multiboot2::Systab_64
{
    public:

        uint64_t pointer;
};

class Multiboot2::Framebuffer
{
    public:

        uint64_t addr;
        uint32_t pitch;
        uint32_t width;
        uint32_t height;
        uint8_t  bpp;
        uint8_t  type;
} PACKED;

class Multiboot2::Header : public Tag
{
    private:

        inline auto align_dn (auto val, auto align) const
        {
            val &= ~(align - 1);                // Expect power-of-2
            return val;
        }

        inline auto align_up (auto val, auto align) const
        {
            val += (align - 1);                 // Expect power-of-2
            return align_dn (val, align);
        }

    public:

        inline uint32_t total_size() const { return type; }

        template <typename FUNC>
        inline void for_each_tag(FUNC const &fn) const
        {
            Tag const * s = this + 1; 
            Tag const * e = reinterpret_cast<Tag const *>(reinterpret_cast<uintptr_t>(this) + total_size()); 

            for (Tag const * i = s; i < e && i->type != TAG_END;) { 
                if (fn(i)) return;
                i = reinterpret_cast<Tag const *>(reinterpret_cast<uintptr_t>(i) + align_up(uint64_t(i->size), sizeof(Tag)));
            }
        }
};
