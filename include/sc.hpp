/*
 * Scheduling Context
 *
 * Copyright (C) 2009-2011 Udo Steinberg <udo@hypervisor.org>
 * Economic rights: Technische Universitaet Dresden (Germany)
 *
 * Copyright (C) 2012-2013 Udo Steinberg, Intel Corporation.
 * Copyright (C) 2014 Udo Steinberg, FireEye, Inc.
 * Copyright (C) 2013-2020 Alexander Boettcher, Genode Labs GmbH
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

class Ec;

class Sc : public Kobject, public Refcount
{
    friend class Queue<Sc>;

    public:
        Refptr<Ec> const ec;
        unsigned       cpu;
        uint16         prio;
        uint16         disable { 0 };
        uint64         budget;
        uint64         time    { 0 };
        uint64         time_m  { 0 };

        static unsigned const priorities = 128;

    private:
        uint64 left;
        Sc *prev { nullptr }, *next { nullptr };
        uint64 tsc { 0 };

        static struct Rq {
            Spinlock    lock { };
            Sc *        queue { nullptr };
        } rq CPULOCAL;

        static Sc *     list[priorities] CPULOCAL;
        static unsigned prio_top         CPULOCAL;

        void ready_enqueue (uint64, bool, bool = true);
        void ready_dequeue (uint64);

        static void free      (Rcu_elem *);
        static void free_xcpu (Rcu_elem *);
        static void pre_free  (Rcu_elem *);
        static bool remove    (Sc *);

        Sc              (Sc const &);
        Sc & operator = (Sc const &);

    public:

        static Sc *     current     CPULOCAL_HOT;
        static unsigned ctr_link    CPULOCAL;
        static unsigned ctr_loop    CPULOCAL;
        static uint64   long_loop   CPULOCAL;
        static uint64   cross_time[NUM_CPU];
        static uint64   killed_time[NUM_CPU];

        static unsigned const default_prio = 1;
        static unsigned const default_quantum = 10000;

        Sc (Pd *, mword, Ec *);
        Sc (Pd *, mword, Ec *, unsigned, unsigned, unsigned);
        Sc (Pd *, Ec *, unsigned, Sc *);
        Sc (Pd *, Ec *, Sc &);

        ALWAYS_INLINE
        static inline Rq *remote (unsigned long c)
        {
            return reinterpret_cast<typeof rq *>(reinterpret_cast<mword>(&rq) - CPU_LOCAL_DATA + HV_GLOBAL_CPUS + c * PAGE_SIZE);
        }

        void remote_enqueue(bool = true);

        static void rrq_handler();
        static void rke_handler();

        NORETURN
        static void schedule (bool = false, bool = true);

        ALWAYS_INLINE
        static inline void *operator new (size_t, Pd &pd) { return pd.sc_cache.alloc(pd.quota); }

        static void operator delete (void *ptr);

        ALWAYS_INLINE
        void inline measured() { time_m = time; }

        void xcpu_clone(Sc const & sc, uint16 const tcpu)
        {
            prio    = sc.prio;
            budget  = sc.budget;
            left    = sc.left;
            disable = false;
            cpu     = tcpu;
            time    = 0;
            time_m  = 0;
            tsc     = 0;
        }
};
