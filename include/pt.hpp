/*
 * Portal
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

#pragma once

#include "kobject.hpp"
#include "mtd.hpp"

class Ec;

class Pt : public Kobject, public Refcount
{
    private:
        static Slab_cache cache;

        static void free (Rcu_elem * p);

    public:
        Refptr<Ec> const ec;
        Mtd        const mtd;
        mword      const ip;
        mword      id;

        Pt (Pd *, mword, Ec *, Mtd, mword);

        ALWAYS_INLINE
        inline void set_id (mword i) { id = i; }

        ALWAYS_INLINE
        static inline void *operator new (size_t, Quota &quota) { return cache.alloc(quota); }

        ALWAYS_INLINE
        static inline void destroy(Pt *obj, Quota &quota) { obj->~Pt(); cache.free (obj, quota); }
};
