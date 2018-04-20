/*
 * Quota tracking of buddy allocator
 *
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

#include "lock_guard.hpp"
#include "util.hpp"

class Buddy;

class Quota
{
    friend class Buddy;

    private:
        Spinlock lock;

        mword used;
        mword over;

        mword upli;
        mword notr;

    public:

        static Quota init;

        Quota () : used(0), over(0), upli(0), notr(0) { }

        void alloc(mword p)
        {
            Lock_guard <Spinlock> guard (lock);
            used += p;
        }

        void free(mword p)
        {
            Lock_guard <Spinlock> guard (lock);

            if (p <= used) {
                used -= p;
                return;
            }

            over += p - used;
            upli += p - used;
            used = 0;
        }

        mword usage() { return used; }

        static void boot(Quota &kern, Quota &root)
        {
            kern.upli   = kern.used;
            root.upli  -= kern.used;
        }

        void free_up(Quota &to)
        {
            mword l, u, o;
            {
                Lock_guard <Spinlock> guard (lock);
                l = upli;
                u = used;
                o = over;
                upli = over = used = 0;
            }

            Lock_guard <Spinlock> guard (to.lock);
            to.used += u;
            to.upli += l;
            to.over += o;

            if (to.over && to.used) {
                mword s = min (to.used, to.over);
                to.used -= s;
                to.over -= s;
                to.upli -= s;
            }
        }

        bool hit_limit(mword free_space = 0)
        {
             if (free_space > upli)
                 return true;

             return usage() > upli - free_space;
        }

        bool transfer_to(Quota &to, mword transfer, bool check_notr = true)
        {
             mword o = 0;

             {
                 Lock_guard <Spinlock> guard (lock);

                 if (hit_limit()) return false;

                 mword not_for_transfer = check_notr ? notr : 0;

                 if (usage() + transfer > upli - not_for_transfer) return false;

                 upli -= transfer;

                 o = min (over, transfer);
                 if (o)
                     over -= o;
             }

             Lock_guard <Spinlock> guard (to.lock);
             to.upli += transfer;

             if (to.used && o) {
                mword u = min (to.used, o);
                to.used -= u;
                to.upli -= u;
                to.over += o - u;
             }

             return true;
        }

        bool set_limit(mword l, mword h, Quota &from)
        {
            if (!from.transfer_to(*this, h))
                return false;

            notr = l;
            return true;
        }

        mword limit() { return upli; }

        void dump(void *, bool = true);
};

class Quota_guard
{
    private:

        Quota q;
        Quota &r;

    public:

        Quota_guard(Quota &ref) : q(), r(ref) { }

        bool check(mword req)
        {
            if (!q.hit_limit(req))
                return true;

            if (q.limit() <= q.usage())
                req += q.usage() - q.limit();
            else
                req -= q.limit() - q.usage();

            return r.transfer_to(q, req, false);
        }

        operator Quota&() { return q; }

        ~Quota_guard() { q.free_up(r); }
};
