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
#include "string.hpp"

extern char _mempool_e;

enum { MEMORY_AVAIL = 1 };

mword Hip::root_addr;
mword Hip::root_size;

static uint64 kernel_target_size(uint64 const system_mem_max)
{
    uint64 const kernel_mem_min = CONFIG_MEMORY_DYN_MIN; /* preferred min */
    uint64 system_mem = system_mem_max / 1000 * CONFIG_MEMORY_DYN_PER_MILL;
    if (system_mem_max >= kernel_mem_min)
        system_mem = max(kernel_mem_min, system_mem);
    return system_mem;
}

bool Hip::build (mword magic, mword addr)
{
    Hip_guard hip_guard { };

    hip_guard.with_hip([&](auto &h) {
        h.signature = 0x41564f4e;
        h.cpu_offs  = reinterpret_cast<mword>(h.cpu_desc) - reinterpret_cast<mword>(&h);
        h.cpu_size  = static_cast<uint16>(sizeof (Hip_cpu));
        h.mem_offs  = reinterpret_cast<mword>(h.mem_desc) - reinterpret_cast<mword>(&h);
        h.mem_size  = static_cast<uint16>(sizeof (Hip_mem));
        h.api_flg   = FEAT_VMX | FEAT_SVM;
        h.api_ver   = CFG_VER;
        h.sel_num   = Space_obj::caps;
        h.sel_gsi   = NUM_GSI;
        h.sel_exc   = NUM_EXC;
        h.sel_vmi   = NUM_VMI;
        h.cfg_page  = PAGE_SIZE;
        h.cfg_utcb  = PAGE_SIZE;
    });

    if (magic == Multiboot::MAGIC)
        build_mbi1(hip_guard, addr);

    if (magic == Multiboot2::MAGIC)
        build_mbi2(hip_guard, addr);

    add_mhv (hip_guard);

    uint64 const system_mem_max = system_memory(hip_guard);
    uint64       memory_allocated = 0;

    add_buddy (hip_guard, system_mem_max, memory_allocated, true);

    if (memory_allocated < kernel_target_size(system_mem_max))
        add_buddy (hip_guard, system_mem_max, memory_allocated, false);

    return hip_guard.ready();
}

void Hip::build_mbi1(Hip_guard &hg, mword addr)
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
        mbi->for_each_mem(remap, mmap_len, [&] (Multiboot_mmap const * mmap) { Hip::add_mem(hg, mmap); });
    }

    if (flags & Multiboot::MODULES) {
        Multiboot_module *mod = static_cast<Multiboot_module *>(Hpt::remap (Pd::kern.quota, mods_addr));
        for (unsigned i = 0; i < mods_count; i++, mod++)
            add_mod (hg, mod, mod->cmdline);
    }
}

void Hip::build_mbi2(Hip_guard &hg, mword const addr)
{
    Multiboot2::Header const *mbi = static_cast<Multiboot2::Header const *>(Hpt::remap (Pd::kern.quota, addr));

    mbi->for_each_tag([&](Multiboot2::Tag const * tag) {

        if (tag->type == Multiboot2::TAG_SYSTAB_64)
            Hip::add_systab(hg, tag->systab_64());

        if (tag->type == Multiboot2::TAG_CMDLINE)
            Cmdline::init (tag->cmdline());

        if (tag->type == Multiboot2::TAG_MEMORY)
            tag->for_each_mem([&] (Multiboot2::Memory_map const * mmap) { Hip::add_mem(hg, mmap); });

        if (tag->type == Multiboot2::TAG_MODULE)
            Hip::add_mod(hg, tag->module(), 0); /* XXX cmdline */

        if (tag->type == Multiboot2::TAG_ACPI_2)
            Acpi_rsdp::parse(tag->rsdp());

        if (tag->type == Multiboot2::TAG_FB)
            Hip::add_fb(hg, tag->framebuffer());
    });
}

void Hip::add_fb(Hip_guard &hg, auto const *fb)
{
    hg.with_mem_desc([&](auto &mem) {
        mem.addr  = fb->addr;
        mem.size  = static_cast<uint64>(fb->width) << 40;
        mem.size |= static_cast<uint64>(fb->height & ((1U << 24) - 1)) << 16;
        mem.size |= (fb->type & 0xffu) << 8;
        mem.size |= fb->bpp & 0xffu;
        mem.aux   = fb->pitch;
        mem.type  = Hip_mem::MB2_FB;
    });
}


void Hip::add_systab(Hip_guard &hg, auto const *systab)
{
    hg.with_mem_desc([&](auto &mem) {
        mem.addr = systab->pointer;
        mem.size = 0;
        mem.type = Hip_mem::SYSTAB;
    });
}


void Hip::add_mod(Hip_guard &hg, auto const * mod, uint32 aux)
{
    if (!root_addr) {
        root_addr = mod->s_addr;
        root_size = mod->e_addr - mod->s_addr;
    }

    hg.with_mem_desc([&](auto &mem) {
        mem.addr = mod->s_addr;
        mem.size = mod->e_addr - mod->s_addr;
        mem.type = Hip_mem::MB_MODULE;
        mem.aux  = aux;
    });
}

void Hip::add_mem (Hip_guard &hg, auto const *map)
{
    hg.with_mem_desc([&](auto &mem) {

        mem.addr = map->addr;
        mem.size = map->len;
        mem.type = map->type;
        mem.aux  = 0;

        if (Cmdline::logmem && !PAGE_L &&
            mem.size >= 2 * PAGE_SIZE &&
            mem.addr + mem.size < ~0U)
        {
            PAGE_L     = static_cast<mword>(((    mem.addr +     mem.size) & ~(0xFFFUL)) - PAGE_SIZE);
            mem.size -= ((    mem.addr +     mem.size) & (0xFFFUL)) + PAGE_SIZE;
        }
    });
}

void Hip::add_mhv (Hip_guard &hg)
{
    hg.with_mem_desc([&](auto &mem) {
        /* exclude init code which is reused during suspend/resume */
        mem.addr = LOAD_ADDR;
        mem.size = reinterpret_cast<mword>(&LOAD_E) - mem.addr;
        mem.type = Hip_mem::HYPERVISOR;

        auto mem_remove = mem.addr;
        while (mem_remove < mem.addr + mem.size) {
            Pd::kern.Space_mem::delreg(Pd::kern.quota, Pd::kern.mdb_cache, static_cast<mword>(mem_remove));
            mem_remove += PAGE_SIZE;
        }
    });

    hg.with_mem_desc([&](auto &mem) {
        /* exclude page where the 16bit AP boot-up & resume code is placed */
        mem.addr = AP_BOOT_PADDR;
        mem.size = PAGE_SIZE;
        mem.type = Hip_mem::HYPERVISOR;
        Pd::kern.Space_mem::delreg(Pd::kern.quota, Pd::kern.mdb_cache, static_cast<mword>(mem.addr));
    });

    hg.with_mem_desc([&](auto &mem) {
        /* exclude kernel memory */
        mem.addr = reinterpret_cast<mword>(&LINK_P);
        mem.size = reinterpret_cast<mword>(&LINK_E) - mem.addr;
        mem.type = Hip_mem::HYPERVISOR;
    });
}

void Hip::add_cpu()
{
    Hip_guard guard { };

    guard.with_hip([](auto &hip) {

        auto & cpu = hip.cpu_desc[Cpu::id];

        /* XXX */
        cpu.acpi_id  = Cpu::acpi_id[Cpu::id];
        cpu.package  = Cpu::package[Cpu::id];
        cpu.core     = Cpu::core[Cpu::id];
        cpu.thread   = Cpu::thread[Cpu::id];
        cpu.flags    = 1u | ((Cpu::core_type[Cpu::id] == Cpu::INTEL_CORE) ? 2u :
                             (Cpu::core_type[Cpu::id] == Cpu::INTEL_ATOM) ? 4u : 0u);
        cpu.family   = Cpu::family[Cpu::id];
        cpu.model    = Cpu::model[Cpu::id];
        cpu.stepping = Cpu::stepping[Cpu::id] & 0xf;
        cpu.platform = Cpu::platform[Cpu::id] & 0x7;
        cpu.patch    = Cpu::patch[Cpu::id];
    });
}

void Hip::add_check()
{
    Hip_guard hg { };

    if (Acpi::p_rsdt()) {
        hg.with_mem_desc([&](auto &mem) {
            mem.addr = Acpi::p_rsdt();
            mem.size = 0;
            mem.type = Hip_mem::ACPI_RSDT;
        });
    }
    if (Acpi::p_xsdt()) {
        hg.with_mem_desc([&](auto &mem) {
            mem.addr = Acpi::p_xsdt();
            mem.size = 0;
            mem.type = Hip_mem::ACPI_XSDT;
        });
    }

    if (PAGE_L) {
        hg.with_mem_desc([&](auto &mem) {
            mem.addr = PAGE_L;
            mem.size = PAGE_SIZE;
            mem.type = Hip_mem::HYP_LOG;
            mem.aux  = 0;
        });
    }

    hg.with_hip([](auto &hip) {

        hip.freq_tsc = Lapic::freq_tsc;

        uint16 c = 0;
        for (auto ptr = reinterpret_cast<uint16 const *>(&PAGE_H);
                  ptr < reinterpret_cast<uint16 const *>(mword(&PAGE_H) + hip.length);
                  c = static_cast<uint16>(c - *ptr++)) ;

        hip.checksum = c;
    });
}

uint64 Hip::system_memory (Hip_guard &hg)
{
    uint64 system_mem_max = 0;

    hg.for_each([&] (Hip_mem &block) {
        if (block.type != MEMORY_AVAIL)
            return;
        system_mem_max += block.size;
    });

    return system_mem_max;
}

void Hip::add_buddy (Hip_guard &hg, uint64 const system_mem_max,
                     uint64 &memory_allocated, bool close)
{
    mword const mhv_end = reinterpret_cast<mword>(&LINK_E);
    bool done = false;

    hg.for_each([&] (Hip_mem &m) {
        if (done || (m.type != MEMORY_AVAIL))
            return;

        uint64 const memory_before = memory_allocated;

        /* find memory close behind hypervisor for close = true */
        if (!close || (m.addr <= mhv_end && mhv_end < m.addr + m.size))
            _add_buddy(hg, system_mem_max, memory_allocated, m);

        done = (memory_before != memory_allocated);
    });
}

void Hip::_add_buddy (Hip_guard &hg, uint64 const system_mem_max,
                      uint64 &memory_allocated, Hip_mem const &cmp)
{
    enum { MEMORY_AVAIL = 1 };

    mword const mhv_end = reinterpret_cast<mword>(&LINK_E);
    uint64 region_start = mhv_end;
    uint64 region_end   = cmp.addr + cmp.size;

    if (region_end <= region_start)
         return;

    /* exclude all reserved memory part of region */
    hg.for_each([&] (Hip_mem &m) {
        uint64 m_end = m.addr + m.size;

        if (m.type == Hip_mem::MB2_FB)
            m_end = m.addr + m.aux * (m.size >> 40);

        if (m.type == MEMORY_AVAIL)
            return;
        if (m.addr >= region_end)
            return;
        if (m_end <= region_start)
            return;

        if (region_start <= m.addr) {
            uint64 const new_end  = min (region_end, m.addr);
            uint64 const new_size = new_end - region_start;
            if (region_end > m_end && region_end - m_end > new_size)
                region_start = m_end;
            else
                region_end = new_end;
        } else
        if (region_start <= m_end)
            region_start = m_end;
    });

    /* align region_size and region_addr */
    uint64 region_size = region_end - region_start;

    uint64 const mem_log   = (sizeof(void *) == 4) ? 22 : 21;
    uint64 const mask_size = 1ULL << mem_log;
    uint64 const mask      = (mask_size) - 1;

    if (region_start & mask) {
        uint64 const add = mask_size - (region_start & mask);
        if (region_size >= add) {
            region_size  -= add;
            region_start += add;
        } else
            region_size = 0;
    }

    mword const buddy_start = static_cast<mword>(region_start);

    /* limit to virtual available memory */
    mword const v_buddy = reinterpret_cast<mword>(&_mempool_e) + (buddy_start - mhv_end);
    if (v_buddy >= BUDDY_V_MAX)
        return;

    if (v_buddy + region_size >= BUDDY_V_MAX)
        region_size = (BUDDY_V_MAX - v_buddy);

    uint64 system_mem = kernel_target_size(system_mem_max);
    if (memory_allocated < system_mem)
        system_mem -= memory_allocated;

    uint64 const buddy_size = min(system_mem, region_size) & ~mask;
    if (!buddy_size)
        return;

    hg.with_mem_desc([&](auto &mem) {

        for (unsigned i = 0; i < (buddy_size / PAGE_SIZE); i++) {
            Paddr const p_buddy = buddy_start + i * PAGE_SIZE;
            Pd::kern.Space_mem::delreg(Pd::kern.quota, Pd::kern.mdb_cache, p_buddy);

            if (!(p_buddy & mask))
                Pd::kern.Space_mem::insert (Pd::kern.quota, v_buddy + i * PAGE_SIZE, mem_log - 12, Hpt::HPT_NX | Hpt::HPT_G | Hpt::HPT_W | Hpt::HPT_P, p_buddy);
        }

        memset(reinterpret_cast<void *>(v_buddy), 0, static_cast<mword>(buddy_size));

        /* allocate new buddy */
        new (Pd::kern.quota) Buddy(buddy_start, v_buddy, v_buddy, static_cast<mword>(buddy_size));

        mem.addr = buddy_start;
        mem.size = buddy_size;
        mem.type = Hip_mem::HYPERVISOR;

        memory_allocated += buddy_size;
    });
}
