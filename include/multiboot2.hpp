/*
 * Multiboot2 support
 *
 * Copyright (C) 2017 Alexander Boettcher, Genode Labs GmbH
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
    class Color_info;
    class Color_info_ega;
    class Color_info_indexed;
    class Color_info_rgb;
    class Header;
    class Framebuffer;
    class Memory_map;
    class Module;
    class Tag;

    enum {
        MAGIC       = 0x36d76289,
        TAG_END     = 0,
        TAG_CMDLINE = 1,
        TAG_MODULE  = 3,
        TAG_MEMORY  = 6,
        TAG_FRAMEBUFFER = 8,
        TAG_ACPI_2  = 15,
    };

};

class Multiboot2::Memory_map
{
    public:

        uint64 addr;
        uint64 len;
        uint32 type;
        uint32 reserved;
};

class Multiboot2::Tag
{
    public:

        uint32  type;
        uint32  size;

        inline const char * cmdline() const
        {
            if (type != TAG_CMDLINE)
                return nullptr;

            return reinterpret_cast<const char *>(this + 1);
        }

        inline Framebuffer const * framebuffer() const
        {
            if (type != TAG_FRAMEBUFFER)
                return nullptr;

            return reinterpret_cast<Framebuffer *>(reinterpret_cast<mword>(this + 1));
        }

        inline Module const * module() const
        {
            if (type != TAG_MODULE)
                return nullptr;

            return reinterpret_cast<Module *>(reinterpret_cast<mword>(this + 1));
        }

        inline mword rsdp() const
        {
            if (type != TAG_ACPI_2)
                return 0;
           
            return reinterpret_cast<mword>(this + 1);
        }

        template <typename FUNC>
        inline void for_each_mem(FUNC const &fn) const
        {
            if (type != TAG_MEMORY)
                return;

            Memory_map const * s = reinterpret_cast<Memory_map *>(reinterpret_cast<mword>(this + 1) + 8);
            Memory_map const * e = reinterpret_cast<Memory_map *>(reinterpret_cast<mword>(this) + size);

            for (Memory_map const * i = s; i < e; i++) fn(i);
        }
};

class Multiboot2::Module
{
    public:

        uint32 s_addr;
        uint32 e_addr;
        char string [0];
};

class Multiboot2::Color_info{};

class Multiboot2::Color_info_indexed : public Color_info
{
    uint32 framebuffer_palette_num_colors;
    struct {
        uint8 red_value;
        uint8 green_value;
        uint8 blue_value;
    } framebuffer_palette[];
};

class Multiboot2::Color_info_rgb : public Color_info
{
    uint8 framebuffer_red_field_position;
    uint8 framebuffer_red_mask_size;
    uint8 framebuffer_green_field_position;
    uint8 framebuffer_green_mask_size;
    uint8 framebuffer_blue_field_position;
    uint8 framebuffer_blue_mask_size;
};

class Multiboot2::Color_info_ega : public Color_info{};

class Multiboot2::Framebuffer
{
    public:

        uint64 framebuffer_addr;
        uint32 framebuffer_pitch;
        uint32 framebuffer_width;
        uint32 framebuffer_height;
        uint8 framebuffer_bpp;
        uint8 framebuffer_type;
        uint8 reserved;
 //       Multiboot2::Color_info color_info;
};

class Multiboot2::Header : public Tag
{
    private:

        inline uint32 total_size() const { return type; }

    public:

        template <typename FUNC>
        inline void for_each_tag(FUNC const &fn) const
        {
            Tag const * s = this + 1; 
            Tag const * e = reinterpret_cast<Tag const *>(reinterpret_cast<mword>(this) + total_size()); 

            for (Tag const * i = s; i < e && !(i->type == TAG_END && i->size == sizeof(Tag));) { 
                 fn(i);
                 i = reinterpret_cast<Tag const *>(reinterpret_cast<mword>(i) + align_up(i->size, sizeof(Tag)));
            }
        }
};
