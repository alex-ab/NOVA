/*
 * IOMMU - AMD-V
 *
 * Copyright (C) 2020 Alexander Boettcher <alexander.boettcher@genode-labs.com>
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

#include "barrier.hpp"
#include "iommu_amd.hpp"
#include "ioapic.hpp"
#include "pci.hpp"
#include "pd.hpp"
#include "stdio.hpp"
#include "string.hpp"

INIT_PRIORITY (PRIO_SLAB)
Slab_cache  Iommu::Amd::cache (sizeof (Iommu::Amd), 8);

Iommu::Amd * Iommu::Amd::list;
Iommu::Dte * Iommu::Amd::dtb;
uint32       Iommu::Amd::dtsize;

Iommu::Amd::Amd (Paddr const base, uint16 const rid, bool valid)
: List<Iommu::Amd> (list), reg_base ((hwdev_addr -= 3 * PAGE_SIZE) | (base & PAGE_MASK)), iommu_rid(rid), efeat_valid(valid)
{
    for (unsigned i = 0; i < 3; i++) {
        Paddr const p = (base & ~0xffful) + 0x1000ul * i;
        Pd::kern.Space_mem::delreg (Pd::kern.quota, Pd::kern.mdb_cache, p);
        Pd::kern.Space_mem::insert (Pd::kern.quota, reg_base + 0x1000ul * i, 0, Hpt::HPT_NX | Hpt::HPT_G | Hpt::HPT_UC | Hpt::HPT_W | Hpt::HPT_P, p);
    }

    /* XXX */
    Ipt::ord = 9; /* XXX 21 - 12 -> 2M on 64bit */
}

void Iommu::Amd::fault_handler()
{
    if (!event_base) return;

    uint64 const ctrl = read<uint64>(REG_CTRL);
    if (!(ctrl & 4)) return;

    uint64 tail = ring_mask(read<uint64>(REG_EVENT_TAIL));
    uint64 head = ring_mask(read<uint64>(REG_EVENT_HEAD));

    uint64 const ring_size = 1ull << (12 + EVENT_ORDER);

    if (((head > ring_size - 16)) || (tail > ring_size - 16)) {

        trace(TRACE_IOMMU, "IOMMU:%p event ring access out of bound", this);

        disable_events(ctrl);
        return;
    }

    bool disabled = false;

    if (head != tail) {
        do {
            uint64 const *info = reinterpret_cast<uint64 *>(event_base + head);
            uint16 const rid   = info[0] & 0xffff;
            uint8  const type  = uint8((info[0] >> 60) & 0xfu);

            trace (TRACE_IOMMU, "IOMMU:%p FR:%#010llx FI:%#010llx type:%#x BDF:%02x:%02x.%x",
                   this, info[0], info[1], type,
                   (rid >> 8) & 0xff, (rid >> 3) & 0x1f, rid & 0x7);

            head = (head + 16) % ring_size;

            if (!disabled)
                disabled = disable_reporting(rid);
        } while (head != tail);

        write<uint64>(REG_EVENT_HEAD, head);
    }

    update_reporting(disabled);

    if (disabled) {
        disable_events(ctrl);
        return;
    }

    /* checks for overflow */
    enable_events(ctrl);
}

void Iommu::Amd::start()
{
    bool use_pci = true;

#if 0
    if (efeat_valid) {
        uint64 cap_feat = read<uint64>(REG_EFEAT);
        if (cap_feat & (1ull << 46))
            Console::print("%p: MSI cap MMIO supported, enabling could be implemented XXX", this);
        else
            Console::print("%p: MSI cap not supported XXX", this);
    }
#endif

    if (use_pci)
        Pci::enable_msi(iommu_rid);

    uint64 ctrl = read<uint64>(REG_CTRL);

    if (!event_base)
        event_base = reinterpret_cast<mword>(Buddy::allocator.alloc (EVENT_ORDER, Pd::kern.quota, Buddy::FILL_0));

    if (event_base) {
        uint64 pevent = Buddy::ptr_to_phys (reinterpret_cast<void *>(event_base));
        uint64 const base = mask(pevent) | ((0b1000ull + EVENT_ORDER) << 56);

        write<uint64>(REG_EVENT, base);

        ctrl |= 4ULL /* enable */ | 8ULL /* irq on overflow */;
    }

    if (!cmd_base)
        cmd_base = reinterpret_cast<mword>(Buddy::allocator.alloc (CMD_ORDER, Pd::kern.quota, Buddy::FILL_0));

    if (cmd_base) {
        uint64 pcmd = Buddy::ptr_to_phys (reinterpret_cast<void *>(cmd_base));
        uint64 const base = mask(pcmd) | ((0b1000ull + CMD_ORDER) << 56);

        write<uint64>(REG_CMD, base);

        ctrl |= 1ULL << 12 /* enable */;
    }

    enable_dtb();

    ctrl |= 1ULL;
    write<uint64>(REG_CTRL, ctrl);
}

void Iommu::Amd::enable_dtb()
{
    if (!dtb)
        return;

    uint64 pbase = Buddy::ptr_to_phys (dtb);
    pbase  = mask(pbase);
    pbase |= (dtsize / 4096) - 1;

    write(REG_DTB, pbase);
}

void Iommu::Amd::alloc_dtb(unsigned short const /* order */,
                           unsigned short const rid_s,
                           unsigned short const rid_e,
                           uint8          const setting)
{
    /* use one large area 256bit * 65536 dev */
    unsigned short const order = 9;
    if (order > 9) return;

    if (!dtb) {
        dtb = static_cast<Dte *>(Buddy::allocator.alloc (order, Pd::kern.quota, Buddy::FILL_0));
        if (!dtb) return;

        dtsize = 1u << (order + 12);

        enable_dtb();
    }

    for (unsigned rid = rid_s; rid <= rid_e; rid ++) {
        Dte * dev = dte(uint16(rid));
        if (!dev) break;

        dev->enable(); /* checks on, DMA map empty */
        dev->iv();     /* checks on, IRQ map empty */

        /* DTE settings */
        if (setting & 0x1)
            dev->x[2] |= 1ULL << 56;
        if (setting & 0x2)
            dev->x[2] |= 1ULL << 57;
        if (setting & 0x4)
            dev->x[2] |= 1ULL << 58;
        if (setting & 0x10)
            dev->x[1] |= 1ULL << 40;
        if (setting & 0x20)
            dev->x[1] |= 1ULL << 41;
        if (setting & 0x40)
            dev->x[2] |= 1ULL << 62;
        if (setting & 0x80)
            dev->x[2] |= 1ULL << 63;
    }
}

Iommu::Dte * Iommu::Amd::dte(uint16 const rid) const
{
    if (!dtb) return nullptr;

    Dte * const entry = dtb + rid;
    if ((entry < dtb) || (entry + 1 > dtb + (dtsize / sizeof(*entry)))) return nullptr;

    return entry;
}

void Iommu::Amd::assign (uint16 const rid, Pd * const p)
{
    Dte * const entry = dte(rid);

    if (!entry) return;
    if (!p || p->dom_id == Space_mem::NO_DOMAIN_ID) return;

    Lock_guard <Spinlock> guard (lock);

    if (entry->hptrp() && !entry->dma_active()) {
        Console::print("deny assignment of alias rid");
        return;
    }

    uint64 const ctrl = read<uint64>(REG_CTRL);
    if (enable_events(ctrl))
        trace(TRACE_IOMMU, "IOMMU:%p - re-enabling fault reporting", this);

    if (entry->hptrp() != mask(p->ipt.root(p->quota))) {
        release(entry, rid);
        p->release_rid([](uint16){});

        if (entry->hptrp())
            return;

        p->assign_rid(rid);
    }

    entry->hptrp(p->ipt.root(p->quota));
    entry->ir();
    entry->iw();
    entry->paging_mode(p->ipt.max());
    entry->domain_id(p->dom_id);

    entry->dma_enable();
    entry->enable();

    flush_dte(rid, false);
    flush_pgt(*p);
}

static Iommu::Amd * lookup (uint16 const rid)
{
    auto * const obj = Pci::find_iommu (rid);
    Iommu::Amd * iommu = static_cast<Iommu::Amd *>(obj);

    if (!iommu) {
        /* Required if DTE_SPECIAL PCI device is not visible on PCI bus (Qemu) */
        Ioapic::for_each([&](Ioapic *ioapic) {
            if (ioapic->get_rid() != rid) return;
            iommu = static_cast<Iommu::Amd *>(ioapic->io_mmu());
        });
    }

    return iommu;
}

void Iommu::Amd::release (Pd * const p, uint16 const rid)
{
    if (!p) return;

    Dte * const entry = dte(rid);
    if (!entry) return;

    Lock_guard <Spinlock> guard (lock);

    if (entry->hptrp() != mask(p->ipt.root(p->quota))) return;

    release(entry, rid);
}

void Iommu::Amd::release (Dte * const entry, uint16 const rid)
{
    if (!entry) return;

    uint64 const phys = entry->itrp();

    entry->hptrp(0);
    entry->dma_disable();
    entry->domain_id(Space_mem::NO_DOMAIN_ID);
    entry->intctl_disable();

    if (phys) {
        mword const p = static_cast<mword>(phys);
        assert (p == phys);
        if (p == phys) {
            void * irt = phys ? Buddy::phys_to_ptr(p) : nullptr;
            if (irt)
                memset(irt, 0, 4096);
        }
    }

    flush_irt(rid, false);
    flush_dte(rid, true);
}

void Iommu::Amd::alias (uint16 const alias, uint16 const rid)
{
    Dte * const entry = dte(alias);
    if (!entry) return;

    Lock_guard <Spinlock> guard (lock);

    /* let translation of dma off, see assign check, store rid as hint */
    uint64 pbase = uint64(rid) << 12;
    entry->hptrp(pbase);
    entry->enable();
}

void Iommu::Amd::set_irt(unsigned, unsigned rid, unsigned cpu, unsigned vec, unsigned)
{
    /* paranoia */
    if (vec >= 256 || cpu >= 256) return;

    auto iommu = lookup(uint16(rid));
    if (!iommu) return;

    Dte * dev = iommu->dte(uint16(rid));
    if (!dev) return;

    Lock_guard <Spinlock> guard (iommu->lock);

    uint64 phys    = dev->itrp();
    mword  const p = static_cast<mword>(phys);
    assert (p == phys);
    if (p != phys) return;

    uint32 * irt = phys ? static_cast<uint32 *>(Buddy::phys_to_ptr(p)) : nullptr;

    if (!phys) {
        /* use during early boot quota of Pd::kern */
        Quota &quota = (Pd::kern.quota.limit() == 0) ? Pd::kern.quota : Pd::root.quota;
        irt  = static_cast<uint32 *>(Buddy::allocator.alloc (0, quota, Buddy::FILL_0));
        if (!irt) return;

        phys = Buddy::ptr_to_phys (irt);
    }

    /* rq_eoi required for IRQ via IOAPIC (APU 5800K), MSI/-X works also w/o */
    irt[vec] = ((vec & 0xffu) << 16) | ((cpu & 0xffu) << 8) | (1u << 5) | 1u;

    if (!dev->intctl()) {
        dev->inttablen256();
        dev->itrp(phys);

        iommu->flush_irt(rid, false);

        dev->intctl10();
    }

    iommu->flush_dte(rid, true);
}

void Iommu::Amd::flush(unsigned const rid, unsigned const type, bool const wait)
{
    if (!cmd_base) return;

    uint64 tail = ring_mask(read<uint64>(REG_CMD_TAIL));
    uint64 const ring_size = 1ull << (12 + EVENT_ORDER);

    uint64 *inv = reinterpret_cast<uint64 *>(cmd_base + tail);
    *inv = (uint64(type) << 60) | uint64(rid & 0xffffu);
    *(++inv) = 0ull;
    tail = (tail + 16) % ring_size;

    if (wait) {
        inv = reinterpret_cast<uint64 *>(cmd_base + tail);

        *inv = (1ull /* cmd wait */ << 60);
        *(++inv) = 0ull;
        tail = (tail + 16) % ring_size;
    }

    barrier();

    write<uint64>(REG_CMD_TAIL, tail);

    if (!Lapic::pause_loop_until(500, [&] {
      return (ring_mask(read<uint64>(REG_CMD_HEAD)) != tail);
    }))
      trace(TRACE_IOMMU, "timeout - iommu flush");
}

void Iommu::Amd::flush_pgt (Pd &p)
{
    if (!cmd_base) return;

    uint64 tail = ring_mask(read<uint64>(REG_CMD_TAIL));
    uint64 const ring_size = 1ull << (12 + EVENT_ORDER);

    uint64 *inv = reinterpret_cast<uint64 *>(cmd_base + tail);
    inv[0] = (3ull /* flush pgt */ << 60) | (uint64(p.dom_id) & 0xffffull) << 32;
    inv[1] = (0x7FFFFFFFFFFFFull << 12) | 2 | 1;
    tail = (tail + 16) % ring_size;

    barrier();

    write<uint64>(REG_CMD_TAIL, tail);

    if (!Lapic::pause_loop_until(500, [&] {
      return (ring_mask(read<uint64>(REG_CMD_HEAD)) != tail);
    }))
      trace(TRACE_IOMMU, "timeout - iommu flush pgt");
}

void Iommu::Amd::flush_pgt (uint16 const rid, Pd &p)
{
    auto iommu = lookup(rid);
    if (!iommu) return;

    Lock_guard <Spinlock> guard (iommu->lock);

    iommu->flush_pgt(p);
}
