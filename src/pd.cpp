/*
 * Protection Domain
 *
 * Copyright (C) 2009-2011 Udo Steinberg <udo@hypervisor.org>
 * Economic rights: Technische Universitaet Dresden (Germany)
 *
 * Copyright (C) 2012 Udo Steinberg, Intel Corporation.
 * Copyright (C) 2015 Alexander Boettcher, Genode Labs GmbH
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

#include "mtrr.hpp"
#include "pd.hpp"
#include "stdio.hpp"
#include "hip.hpp"
#include "ec.hpp"

INIT_PRIORITY (PRIO_SLAB)
Slab_cache Pd::cache (sizeof (Pd), 32);

Pd *Pd::current;

INIT_PRIORITY (PRIO_BUDDY)
ALIGNED(32) Pd Pd::kern (&Pd::kern);
ALIGNED(32) Pd Pd::root (&Pd::root, NUM_EXC, 0x1f);

Pd::Pd (Pd *own) : Kobject (PD, static_cast<Space_obj *>(own))
{
    hpt = Hptp (reinterpret_cast<mword>(&PDBR));

    Mtrr::init();

    Space_mem::insert_root (own->quota, 0, reinterpret_cast<mword>(&LINK_P));
    Space_mem::insert_root (own->quota, reinterpret_cast<mword>(&LINK_E), 1ULL << 52);

    // HIP
    Space_mem::insert_root (own->quota, reinterpret_cast<mword>(&FRAME_H), reinterpret_cast<mword>(&FRAME_H) + PAGE_SIZE, 1);

    // I/O Ports
    Space_pio::addreg (own->quota, 0, 1UL << 16, 7);
}

Pd::Pd (Pd *own, mword sel, mword a) : Kobject (PD, static_cast<Space_obj *>(own), sel, a, free, pre_free)
{
    if (this == &Pd::root) {
        bool res = Quota::init.transfer_to(quota, Quota::init.limit());
        assert(res);
    }
}

template <typename S>
static void free_mdb(Rcu_elem * e)
{
    Mdb *mdb = static_cast<Mdb *>(e);
    Pd  *pd  = &Pd::root;

    if (!mdb->invalid()) {
        S *space = static_cast<S *>(mdb->space);
        pd = static_cast<Pd *>(space);
    }

    Mdb::destroy (mdb, pd->quota);
}

template <typename S>
bool Pd::delegate (Pd *snd, mword const snd_base, mword const rcv_base, mword const ord, mword const attr, mword const sub, char const * deltype)
{
    bool s = false;

    Quota_guard qg(this->quota);

    Mdb *mdb;
    for (mword addr = snd_base; (mdb = snd->S::tree_lookup (addr, true)); addr = mdb->node_base + (1UL << mdb->node_order)) {

        mword o, b = snd_base;
        if ((o = clamp (mdb->node_base, b, mdb->node_order, ord)) == ~0UL)
            break;

        if (quota.hit_limit(1)) {
            Cpu::hazard |= HZD_OOM;
            return s;
        }

        Mdb *node = new (qg) Mdb (static_cast<S *>(this), free_mdb<S>, b - mdb->node_base + mdb->node_phys, b - snd_base + rcv_base, o, 0, mdb->node_type, S::sticky_sub(mdb->node_sub) | sub, static_cast<uint16>(mdb->dpth + 1));

        if (!S::tree_insert (node)) {
            Mdb::destroy (node, qg);

            Mdb * x = S::tree_lookup(b - snd_base + rcv_base);
            if (!x || x->prnt != mdb || x->node_attr != attr)
                trace (0, "overmap attempt %s - tree - PD:%p->%p SB:%#010lx RB:%#010lx O:%#04lx A:%#lx SUB:%lx", deltype, snd, this, snd_base, rcv_base, ord, attr, sub);

            continue;
        }

        if (!node->insert_node (mdb, attr)) {
            assert (node->prev == node);
            assert (node->next == node);

            if (S::tree_remove (node))
                Rcu::call (node);

            trace (0, "overmap attempt %s - node - PD:%p->%p SB:%#010lx RB:%#010lx O:%#04lx A:%#lx SUB:%lx", deltype, snd, this, snd_base, rcv_base, ord, attr, sub);
            continue;
        }

        s |= S::update (qg, node);

        if (Cpu::hazard & HZD_OOM) {
            s |= S::update (qg, node, attr);
            node->demote_node (attr);
            if (node->remove_node() && S::tree_remove (node))
                Rcu::call (node);
            return s;
        }
    }

    if (!qg.check(0))
        Cpu::hazard |= HZD_OOM;

    return s;
}

template <typename S>
void Pd::revoke (mword const base, mword const ord, mword const attr, bool self, bool kim)
{
    Mdb *mdb;
    for (mword addr = base; (mdb = S::tree_lookup (addr, true)); addr = mdb->node_base + (1UL << mdb->node_order)) {

        mword o, p, b = base;
        if ((o = clamp (mdb->node_base, b, mdb->node_order, ord)) == ~0UL)
            break;

        /* keep in mapping database if requested and at least one child node exists */
        if (kim && (ACCESS_ONCE(mdb->next)->dpth > mdb->dpth)) {
            Quota_guard qg(this->quota);
            if (mdb->node_attr & 0x1f) {
                static_cast<S *>(mdb->space)->update (qg, mdb, 0x1f);
                mdb->demote_node (0x1f);
            }
            static_cast<S *>(mdb->space)->tree_remove (mdb, Avl::State::KIM);
            continue;
        }

        Mdb *node = mdb;

        unsigned d = node->dpth; bool demote = false;

        for (Mdb *ptr;; node = ptr) {

            if (node->dpth == d + !self)
                demote = clamp (node->node_phys, p = b - mdb->node_base + mdb->node_phys, node->node_order, o) != ~0UL;

            if (demote && node->node_attr & attr) {
                Quota_guard qg(this->quota);
                static_cast<S *>(node->space)->update (qg, node, attr);
                node->demote_node (attr);
            }

            ptr = ACCESS_ONCE (node->next);

            if (ptr->dpth <= d)
                break;
        }

        Mdb *x = ACCESS_ONCE (node->next);

        assert ((x->dpth <= d) ||
                (self && !(x->node_attr & attr)) ||
                (!self && ((mdb == node) || (d + 1 == x->dpth) || !(x->node_attr & attr))));
        assert (x->dpth > node->dpth ? (x->dpth == node->dpth + 1) : true);

        bool preempt = Cpu::preemption;

        for (Mdb *ptr;; node = ptr) {

            if (preempt)
                Cpu::preempt_disable();

            if (node->remove_node() && static_cast<S *>(node->space)->tree_remove (node))
                Rcu::call (node);

            if (preempt)
                Cpu::preempt_enable();

            ptr = ACCESS_ONCE (node->prev);

            if (node->dpth <= d)
                break;
        }

        assert (node == mdb);
    }
}

mword Pd::clamp (mword snd_base, mword &rcv_base, mword snd_ord, mword rcv_ord)
{
    if ((snd_base ^ rcv_base) >> max (snd_ord, rcv_ord))
        return ~0UL;

    rcv_base |= snd_base;

    return min (snd_ord, rcv_ord);
}

mword Pd::clamp (mword &snd_base, mword &rcv_base, mword snd_ord, mword rcv_ord, mword h)
{
    assert (snd_ord < sizeof (mword) * 8);
    assert (rcv_ord < sizeof (mword) * 8);

    mword s = (1ul << snd_ord) - 1;
    mword r = (1ul << rcv_ord) - 1;

    snd_base &= ~s;
    rcv_base &= ~r;

    if (EXPECT_TRUE (s < r)) {
        rcv_base |= h & r & ~s;
        return snd_ord;
    } else {
        snd_base |= h & s & ~r;
        return rcv_ord;
    }
}

void Pd::xlt_crd (Pd *pd, Crd xlt, Crd &crd)
{
    Crd::Type t = xlt.type();

    if (t && t == crd.type()) {

        Space *snd = pd->subspace (t), *rcv = subspace (t);
        mword sb = crd.base(), so = crd.order(), rb = xlt.base(), ro = xlt.order();
        Mdb *mdb, *node;

        for (node = mdb = snd->tree_lookup (sb); node; node = node->prnt)
            if (node->space == rcv && node != mdb && node->accessible())
                if ((ro = clamp (node->node_base, rb, node->node_order, ro)) != ~0UL)
                    break;

        if (!node) {
            /* Special handling on Genode:
             * If a translate of an item inside the same PD (receiver/sender in same PD)
             * are of no success, then return the very same item.
             */
            Mdb *first = snd->tree_lookup (crd.base());
            if (first && first->space == rcv && first == mdb) {
                rb = xlt.base();
                ro = xlt.order();
                if ((ro = clamp (first->node_base, rb, first->node_order, ro)) != ~0UL)
                    node = first;
           }
        }

        if (node) {

            so = clamp (mdb->node_base, sb, mdb->node_order, so);
            sb = (sb - mdb->node_base) + (mdb->node_phys - node->node_phys) + node->node_base;

            if ((ro = clamp (sb, rb, so, ro)) != ~0UL) {
                trace (TRACE_DEL, "XLT OBJ PD:%p->%p SB:%#010lx RB:%#010lx O:%#04lx", pd, this, crd.base(), rb, so);
                crd = Crd (crd.type(), rb, ro, mdb->node_attr);
                return;
            }
        }
    }

    crd = Crd (0);
}

void Pd::del_crd (Pd *pd, Crd del, Crd &crd, mword sub, mword hot)
{
    Crd::Type st = crd.type(), rt = del.type();
    bool s = false;

    mword a = crd.attr() & del.attr(), sb = crd.base(), so = crd.order(), rb = del.base(), ro = del.order(), o = 0;

    if (EXPECT_FALSE (st != rt || !a)) {
        crd = Crd (0);
        return;
    }

    switch (rt) {

        case Crd::MEM:
            o = clamp (sb, rb, so, ro, hot);
            trace (TRACE_DEL, "DEL MEM PD:%p->%p SB:%#010lx RB:%#010lx O:%#04lx A:%#lx", pd, this, sb, rb, o, a);
            s = delegate<Space_mem>(pd, sb, rb, o, a, sub, "MEM");
            break;

        case Crd::PIO:
            o = clamp (sb, rb, so, ro);
            trace (TRACE_DEL, "DEL I/O PD:%p->%p SB:%#010lx RB:%#010lx O:%#04lx A:%#lx", pd, this, rb, rb, o, a);
            delegate<Space_pio>(pd, rb, rb, o, a, sub, "PIO");
            break;

        case Crd::OBJ:
            o = clamp (sb, rb, so, ro, hot);
            trace (TRACE_DEL, "DEL OBJ PD:%p->%p SB:%#010lx RB:%#010lx O:%#04lx A:%#lx", pd, this, sb, rb, o, a);
            s = delegate<Space_obj>(pd, sb, rb, o, a, 0, "OBJ");
            break;
    }

    crd = Crd (rt, rb, o, a);

    if (s && rt == Crd::OBJ)
        /* if FRAME_0 got replaced by real pages we have to tell all cpus, done below by shootdown */
        this->htlb.merge (cpus);

    if (s)
        shootdown(this);
}

void Pd::rev_crd (Crd crd, bool self, bool preempt, bool kim)
{
    if (preempt)
        Cpu::preempt_enable();

    switch (crd.type()) {

        case Crd::MEM:
            trace (TRACE_REV, "REV MEM PD:%p B:%#010lx O:%#04x A:%#04x %s", this, crd.base(), crd.order(), crd.attr(), self ? "+" : "-");
            revoke<Space_mem>(crd.base(), crd.order(), crd.attr(), self, kim);
            break;

        case Crd::PIO:
            trace (TRACE_REV, "REV I/O PD:%p B:%#010lx O:%#04x A:%#04x %s", this, crd.base(), crd.order(), crd.attr(), self ? "+" : "-");
            revoke<Space_pio>(crd.base(), crd.order(), crd.attr(), self, kim);
            break;

        case Crd::OBJ:
            trace (TRACE_REV, "REV OBJ PD:%p B:%#010lx O:%#04x A:%#04x %s", this, crd.base(), crd.order(), crd.attr(), self ? "+" : "-");
            revoke<Space_obj>(crd.base(), crd.order(), crd.attr(), self, kim);
            break;
    }

    if (preempt)
        Cpu::preempt_disable();

    if (crd.type() == Crd::MEM)
        shootdown(this);
}

void Pd::xfer_items (Pd *src, Crd xlt, Crd del, Xfer *s, Xfer *d, unsigned long ti)
{
    mword set_as_del;

    for (Crd crd; ti--; s--) {

        crd = *s;
        set_as_del = 0;

        switch (s->flags() & 3) {

            case 0:
                xlt_crd (src, xlt, crd);
                break;

            case 2:
                xlt_crd (src, xlt, crd);
                if (crd.type()) break;

                crd = *s;
                set_as_del = 1;

            case 1: {
                bool r = src == &root && s->flags() & 0x800;
                del_crd (r? &kern : src, del, crd, (s->flags() >> 8) & (r ? 7 : 3), s->hotspot());
                if (Cpu::hazard & HZD_OOM)
                    return;
                break;
            }
            default:
                crd = Crd(0);

        };

        if (d)
            *d-- = Xfer (crd, s->flags() | set_as_del);
    }
}

Pd::~Pd()
{
    pre_free(this);

    Space_mem::hpt.clear(quota, Space_mem::hpt.dest_hpt, Space_mem::hpt.iter_hpt_lev);
    Space_mem::dpt.clear(quota);
    Space_mem::npt.clear(quota);
    for (unsigned cpu = 0; cpu < NUM_CPU; cpu++)
        if (Hip::cpu_online (cpu))
            Space_mem::loc[cpu].clear(quota, Space_mem::hpt.dest_loc, Space_mem::hpt.iter_loc_lev);
}

extern "C" int __cxa_atexit(void (*)(void *), void *, void *) { return 0; }
void * __dso_handle = nullptr;
