/*
 * Hypervisor Information Page (HIP)
 *
 * Copyright (C) 2009-2011 Udo Steinberg <udo@hypervisor.org>
 * Economic rights: Technische Universitaet Dresden (Germany)
 *
 * Copyright (C) 2012-2013 Udo Steinberg, Intel Corporation.
 * Copyright (C) 2014 Udo Steinberg, FireEye, Inc.
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

#include "cmdline.hpp"
#include "cpu.hpp"
#include "hip.hpp"
#include "hpt.hpp"
#include "lapic.hpp"
#include "multiboot.hpp"
#include "multiboot2.hpp"
#include "space_obj.hpp"
#include "pd.hpp"
#include "acpi_rsdp.hpp"
#include "acpi.hpp"

mword Hip::root_addr;
mword Hip::root_size;

void Hip::build (mword magic, mword addr)
{
    Hip *h = hip();

    h->signature  = 0x41564f4e;
    h->cpu_offs   = reinterpret_cast<mword>(h->cpu_desc) - reinterpret_cast<mword>(h);
    h->cpu_size   = static_cast<uint16>(sizeof (Hip_cpu));
    h->mem_offs   = reinterpret_cast<mword>(h->mem_desc) - reinterpret_cast<mword>(h);
    h->mem_size   = static_cast<uint16>(sizeof (Hip_mem));
    h->api_flg    = FEAT_VMX | FEAT_SVM;
    h->api_ver    = CFG_VER;
    h->sel_num    = Space_obj::caps;
    h->sel_gsi    = NUM_GSI;
    h->sel_exc    = NUM_EXC;
    h->sel_vmi    = NUM_VMI;
    h->cfg_page   = PAGE_SIZE;
    h->cfg_utcb   = PAGE_SIZE;

    Hip_mem *mem = h->mem_desc;

    if (magic == Multiboot::MAGIC)
        build_mbi1(mem, addr);

    if (magic == Multiboot2::MAGIC)
        build_mbi2(mem, addr);

    add_mhv (mem);

    h->length = static_cast<uint16>(reinterpret_cast<mword>(mem) - reinterpret_cast<mword>(h));
}

void Hip::build_mbi1(Hip_mem *&mem, mword addr)
{
    Multiboot const *mbi = static_cast<Multiboot const *>(Hpt::remap (Pd::kern.quota, addr));

    uint32 flags       = mbi->flags;
    uint32 cmdline     = mbi->cmdline;
    uint32 mmap_addr   = mbi->mmap_addr;
    uint32 mmap_len    = mbi->mmap_len;
    uint32 mods_addr   = mbi->mods_addr;
    uint32 mods_count  = mbi->mods_count;

    if (flags & Multiboot::CMDLINE)
        Cmdline::init (static_cast<char const *>(Hpt::remap (Pd::kern.quota, cmdline)));

    if (flags & Multiboot::MEMORY_MAP) {
        char const *remap = static_cast<char const *>(Hpt::remap (Pd::kern.quota, mmap_addr));
        mbi->for_each_mem(remap, mmap_len, [&] (Multiboot_mmap const * mmap) { Hip::add_mem(mem, mmap); });
    }

    if (flags & Multiboot::MODULES) {
        Multiboot_module *mod = static_cast<Multiboot_module *>(Hpt::remap (Pd::kern.quota, mods_addr));
        for (unsigned i = 0; i < mods_count; i++, mod++)
            add_mod (mem, mod, mod->cmdline);
    }
}

void Hip::build_mbi2(Hip_mem *&mem, mword addr)
{
    Multiboot2::Header const *mbi = static_cast<Multiboot2::Header const *>(Hpt::remap (Pd::kern.quota, addr));

    mbi->for_each_tag([&](Multiboot2::Tag const * tag) {
        if (tag->type == Multiboot2::TAG_CMDLINE)
            Cmdline::init (tag->cmdline());

        if (tag->type == Multiboot2::TAG_MEMORY)
            tag->for_each_mem([&] (Multiboot2::Memory_map const * mmap) { Hip::add_mem(mem, mmap); });

        if (tag->type == Multiboot2::TAG_MODULE)
            Hip::add_mod(mem, tag->module(), 0); /* XXX cmdline */

        if (tag->type == Multiboot2::TAG_ACPI_2)
            Acpi_rsdp::parse(tag->rsdp());

        if (tag->type == Multiboot2::TAG_FB)
            Hip::add_fb(mem, tag->framebuffer());
    });
}

template <typename T>
void Hip::add_fb(Hip_mem *&mem, T const *fb)
{
    mem->addr  = fb->addr;
    mem->size  = static_cast<uint64>(fb->width) << 40;
    mem->size |= static_cast<uint64>(fb->height & ((1U << 24) - 1)) << 16;
    mem->size |= (fb->type & 0xffu) << 8;
    mem->size |= fb->bpp & 0xffu;
    mem->aux   = fb->pitch;
    mem->type  = Hip_mem::MB2_FB;
    mem++;
}


template <typename T>
void Hip::add_mod(Hip_mem *&mem, T const * mod, uint32 aux)
{
    if (!root_addr) {
        root_addr = mod->s_addr;
        root_size = mod->e_addr - mod->s_addr;
    }

    mem->addr = mod->s_addr;
    mem->size = mod->e_addr - mod->s_addr;
    mem->type = Hip_mem::MB_MODULE;
    mem->aux  = aux;
    mem++;
}

template<typename T>
void Hip::add_mem (Hip_mem *&mem, T const *map)
{
    mem->addr = map->addr;
    mem->size = map->len;
    mem->type = map->type;
    mem->aux  = 0;
    mem++;
}

void Hip::add_mhv (Hip_mem *&mem)
{
    mem->addr = reinterpret_cast<mword>(&LINK_P);
    mem->size = reinterpret_cast<mword>(&LINK_E) - mem->addr;
    mem->type = Hip_mem::HYPERVISOR;
    mem++;
}

void Hip::add_cpu()
{
    Hip_cpu *cpu = hip()->cpu_desc + Cpu::id;

    cpu->acpi_id = Cpu::acpi_id[Cpu::id];
    cpu->package = static_cast<uint8>(Cpu::package);
    cpu->core    = static_cast<uint8>(Cpu::core);
    cpu->thread  = static_cast<uint8>(Cpu::thread);
    cpu->flags   = 1;
}

void Hip::add_check()
{
    Hip *h = hip();

    Hip_mem *mem = reinterpret_cast<Hip_mem *>(reinterpret_cast<mword>(h) + h->length);

    if (Acpi::p_rsdt()) {
        mem->addr = Acpi::p_rsdt();
        mem->size = 0;
        mem->type = Hip_mem::ACPI_RSDT;
        mem++;
    }
    if (Acpi::p_xsdt()) {
        mem->addr = Acpi::p_xsdt();
        mem->size = 0;
        mem->type = Hip_mem::ACPI_XSDT;
        mem++;
    }

    h->length = static_cast<uint16>(reinterpret_cast<mword>(mem) - reinterpret_cast<mword>(h));

    h->freq_tsc = Lapic::freq_tsc;

    uint16 c = 0;
    for (uint16 const *ptr = reinterpret_cast<uint16 const *>(&PAGE_H);
                       ptr < reinterpret_cast<uint16 const *>(&PAGE_H + h->length);
                       c = static_cast<uint16>(c - *ptr++)) ;

    h->checksum = c;
}
