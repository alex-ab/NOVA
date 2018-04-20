/*
 * Host Page Table (HPT)
 *
 * Copyright (C) 2009-2011 Udo Steinberg <udo@hypervisor.org>
 * Economic rights: Technische Universitaet Dresden (Germany)
 *
 * Copyright (C) 2012-2013 Udo Steinberg, Intel Corporation.
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

#include "assert.hpp"
#include "bits.hpp"
#include "hpt.hpp"

bool Hpt::sync_from (Quota &quota, Hpt src, mword v, mword o)
{
    mword l = (bit_scan_reverse (v ^ o) - PAGE_BITS) / bpl();

    Hpt *s = static_cast<Hpt *>(src.walk (quota, v, l, false));
    if (!s)
        return false;

    Hpt *d = static_cast<Hpt *>(walk (quota, v, l));
    assert (d);

    if (d->val == s->val)
        return false;

    d->val = s->val;

    return true;
}

void Hpt::sync_master_range (Quota & quota, mword s, mword e)
{
    for (mword l = (bit_scan_reverse (LINK_ADDR ^ CPU_LOCAL) - PAGE_BITS) / bpl(); s < e; s += 1UL << (l * bpl() + PAGE_BITS))
        sync_from (quota, Hptp (reinterpret_cast<mword>(&PDBR)), s, CPU_LOCAL);
}

Paddr Hpt::replace (Quota &quota, mword v, mword p)
{
    Hpt o, *e = walk (quota, v, 0); assert (e);

    do o = *e; while (o.val != p && !(o.attr() & HPT_W) && !e->set (o.val, p));

    flush(v);
    return e->addr();
}

void *Hpt::remap (Quota &quota, Paddr phys)
{
    Hptp hpt (current());

    size_t size = 1UL << (bpl() + PAGE_BITS);

    mword offset = phys & (size - 1);

    phys &= ~offset;

    Paddr old; mword attr;
    if (hpt.lookup (SPC_LOCAL_REMAP, old, attr)) {
        hpt.update (quota, SPC_LOCAL_REMAP,        bpl(), 0, 0, Hpt::TYPE_DN); flush (SPC_LOCAL_REMAP);
        hpt.update (quota, SPC_LOCAL_REMAP + size, bpl(), 0, 0, Hpt::TYPE_DN); flush (SPC_LOCAL_REMAP + size);
    }

    hpt.update (quota, SPC_LOCAL_REMAP,        bpl(), phys,        HPT_W | HPT_P);
    hpt.update (quota, SPC_LOCAL_REMAP + size, bpl(), phys + size, HPT_W | HPT_P);

    return reinterpret_cast<void *>(SPC_LOCAL_REMAP + offset);
}
