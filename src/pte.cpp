/*
 * Page Table Entry (PTE)
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

#include "dpt.hpp"
#include "ept.hpp"
#include "hpt.hpp"
#include "ipt.hpp"
#include "pte.hpp"
#include "stdio.hpp"

mword Dpt::ord = ~0UL;
mword Ept::ord = ~0UL;
mword Hpt::ord = ~0UL;
mword Ipt::ord = ~0UL;

bool  Dpt::force_flush = false;

template <typename P, typename E, unsigned L, unsigned B, bool F, bool V>
P *Pte<P,E,L,B,F,V>::walk (Quota &quota, E v, unsigned long n, bool a)
{
    unsigned long l = L;

    for (P *p, *e = static_cast<P *>(this);; e = static_cast<P *>(Buddy::phys_to_ptr (e->addr())) + (v >> (--l * B + PAGE_BITS) & ((1UL << B) - 1))) {

        if (l == n)
            return e;

        if (!e->val) {

            if (!a)
                return nullptr;

            if (!e->set (0, Buddy::ptr_to_phys (p = new (quota) P) | (l == L ? 0 : E(P::PTE_N)) | (V ? E(l) << 9 : 0)))
                Pte::destroy(p, quota);
        }
    }
}

template <typename P, typename E, unsigned L, unsigned B, bool F, bool V>
size_t Pte<P,E,L,B,F,V>::lookup (E v, Paddr &p, mword &a)
{
    unsigned long l = L;

    for (P *e = static_cast<P *>(this);; e = static_cast<P *>(Buddy::phys_to_ptr (e->addr())) + (v >> (--l * B + PAGE_BITS) & ((1UL << B) - 1))) {

        if (EXPECT_FALSE (!e->val))
            return 0;

        if (EXPECT_FALSE (l && !e->super(l)))
            continue;

        size_t s = 1UL << (l * B + e->order());

        p = static_cast<Paddr>(e->addr() | (v & (s - 1)));

        a = e->attr();

        return s;
    }
}

template <typename P, typename E, unsigned L, unsigned B, bool F, bool V>
bool Pte<P,E,L,B,F,V>::update (Quota &quota, E v, mword o, E p, E a, Type t)
{
    unsigned long l = o / B, n = 1UL << o % B, s;

    P *e = walk (quota, v, l, t == TYPE_UP);

    if (!e)
        return false;

    if (a) {
        p |= P::order (o % B) | P::pte_s(l) | a;
        s = 1UL << (l * B + PAGE_BITS);
    } else
        p = s = 0;

    bool flush_tlb = false;

    for (unsigned long i = 0; i < n; e[i].val = p, i++, p += s) {

        if (!e[i].val)
            continue;

        /* XXX also !l for flushing ? */
        if (l && e[i].val != p)
            flush_tlb = true;

        if (t == TYPE_DF)
            continue;

        if (l && !e[i].super(l)) {
            if (l > 1)
                trace (0, "XXX leaking memory");

            Pte::destroy(static_cast<P *>(Buddy::phys_to_ptr (e[i].addr())), quota);
            flush_tlb = true;
        }
    }

    if (F)
        flush (e, n * sizeof (E));

    if (!a) {
        release_empty(quota, v, l, false);
        flush_tlb = true; /* XXX only if something changed by release_empty */
    }

    return flush_tlb;
}


template <typename P, typename E, unsigned L, unsigned B, bool F, bool V>
void Pte<P,E,L,B,F,V>::release_empty (Quota &quota, E v, mword l, bool verbose)
{
    if (l + 1 >= L)
        return;

    auto const v_ls = v & ~((1ull << ((l + 1) * B + PAGE_BITS)) - 1);

//    if (verbose)
//        trace(0, "l=%lu v=%llx->%llx", l, uint64_t(v), uint64_t(v_ls));

/* XXX - wrong - must per pt type done */
    if (v_ls >= USER_ADDR)
        return;

    P * e_l = walk (quota, v_ls, l, false);

//    if (verbose)
//        trace(0, "l=%lu v=%llx->%llx e_l=%p", l, uint64_t(v), uint64_t(v_ls), e_l);

    if (!e_l)
        return;

    bool unused = true;
    unsigned use = 0;

    for (unsigned long i = 0; i < (1 << B); i++) {

        if (!e_l[i].val)
            continue;

        use ++;
        unused = false;
//        break;
    }

    if (!unused)
        return;

    if (verbose)
        trace(0, "l=%lu, v=%llx->%llx used=%u", l, uint64_t(v), uint64_t(v_ls), use);

    if (l + 1 > 2)
        return;

    auto const v_ln = v_ls & ~((1ull << ((l + 1 + 1) * B + PAGE_BITS)) - 1);
    auto const v_no = v_ls - v_ln;
    auto const v_ns = 1UL << ((l + 1) * B + PAGE_BITS);

    P *e_n = walk (quota, v_ln, l + 1, false);

    assert(e_n);

    if (!e_n)
        return;

    if (verbose)
        trace(0, "l=%lu, v=%llx->%llx v_no=%llx v_ns=%llx e_pos=%llu",
              l + 1,
              uint64_t(v_ls), uint64_t(v_ln), uint64_t(v_no), uint64_t(v_ns), v_no / v_ns);

    auto const pos = v_no / v_ns;

    if (verbose)
        trace(0, "e_l %p -> e_n %p e_n[e_pos].addr()=%llx p=%p",
              e_l, e_n, uint64_t(e_n ? e_n[pos].addr() : 0),
              e_n ? static_cast<P *>(Buddy::phys_to_ptr (e_n[pos].addr())) : nullptr);

    if (pos >= (1 << B))
        return;

    assert (e_n[pos].val);
    assert (Buddy::phys_to_ptr(e_n[pos].addr()) == e_l);

    if (!e_n[pos].val)
        return;

    if (e_n[pos].super(l + 1))
        trace(0, " free up level=%lu super=%d", l + 1, e_n[pos].super(l + 1));

    if (e_n[pos].super(l + 1))
        return;

     P *pl = static_cast<P *>(Buddy::phys_to_ptr (e_n[pos].addr()));

     e_n[pos].val = 0;

     release_empty(quota, v_ln, l + 1, true);

     Pte::destroy(pl, quota);
}

template <typename P, typename E, unsigned L, unsigned B, bool F, bool V>
void Pte<P,E,L,B,F,V>::clear (Quota &from, Quota &to, bool (*d) (Paddr, mword, unsigned), bool (*il) (unsigned, mword))
{
    if (!val)
        return;

    P * e = static_cast<P *>(Buddy::phys_to_ptr (this->addr()));

    e->free_up(from, to, L - 1, e, 0, d, il);

    Pte::destroy (e, from, &to);
}

template <typename P, typename E, unsigned L, unsigned B, bool F, bool V>
void Pte<P,E,L,B,F,V>::free_up (Quota &from, Quota &to, unsigned l, P * e, mword v, bool (*d)(Paddr, mword, unsigned), bool (*il) (unsigned, mword))
{
    if (!e)
        return;

    for (unsigned long i = 0; i < (1 << B); i++) {
        if (!e[i].val || e[i].super(l))
            continue;

        P *p = static_cast<P *>(Buddy::phys_to_ptr (e[i].addr()));
        mword virt = v + (i << (l * B + PAGE_BITS));

        if (il ? il(l, virt) : l > 1)
            p->free_up(from, to, l - 1, p, virt, d, il);

        if (!d || d(e[i].addr(), virt, l))
            Pte::destroy(p, from, &to);
    }
}

template <typename P, typename E, unsigned L, unsigned B, bool F, bool V>
void Pte<P,E,L,B,F,V>::debug_walk (Quota &q, bool const verbose)
{
    if (!val)
        return;

    P * e = static_cast<P *>(Buddy::phys_to_ptr (this->addr()));

    if (verbose)
        trace (0, "l=%u: %llx: ", L - 1, 0ull);

    e->debug_walk_level(q, L - 1, e, 0, verbose);
}

template <typename P, typename E, unsigned L, unsigned B, bool F, bool V>
void Pte<P,E,L,B,F,V>::debug_walk_level (Quota &q, unsigned l, P * e, mword v, bool const verbose)
{
    if (!e)
        return;

    q.alloc(1);

    unsigned cnt_entries = 0;

    for (unsigned long i = 0; i < (1 << B); i++) {
        if (!e[i].val)
            continue;

        P *p = static_cast<P *>(Buddy::phys_to_ptr (e[i].addr()));
        mword virt = v + (i << (l * B + PAGE_BITS));

        if (verbose)
            trace (0, "%sl=%u:s=%u: %lx+%lx",
                   l > 3 ? ""   :
                   l > 2 ? " "  :
                   l > 1 ? "  "  : "   ",
                   l, e[i].super(l), virt, 1ul << (l * B + PAGE_BITS));

        cnt_entries ++;

        if (l >= 1 && !e[i].super(l))
            p->debug_walk_level(q, l - 1, p, virt, verbose);
    }

    if (l >= 2)
        trace (0, "%sl=%u %lx+%lx entries=%u",
               l > 3 ? ""   :
               l > 2 ? " "  :
               l > 1 ? "  "  : "   ",
               l, v, 1ul << (l * B + PAGE_BITS),
               cnt_entries);
}

template class Pte<Dpt, uint64, 4, 9, true, false>;
template class Pte<Ipt, uint64, 4, 9, true, true>;
template class Pte<Ept, uint64, 4, 9, false, false>;
template class Pte<Hpt, mword, PTE_LEV, PTE_BPL, false, false>;
