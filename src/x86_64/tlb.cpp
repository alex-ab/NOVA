/*
 * Translation Lookaside Buffer (TLB)
 *
 * Copyright (C) 2009-2011 Udo Steinberg <udo@hypervisor.org>
 * Economic rights: Technische Universitaet Dresden (Germany)
 *
 * Copyright (C) 2012-2013 Udo Steinberg, Intel Corporation.
 * Copyright (C) 2019-2024 Udo Steinberg, BlueRock Security, Inc.
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

#include "counter.hpp"
#include "ec.hpp"
#include "interrupt.hpp"
#include "space_gst.hpp"
#include "space_hst.hpp"
#include "stc.hpp"
#include "stdio.hpp"
#include "tlb.hpp"
#include "wait.hpp"

void Tlb::shootdown (Space *s)
{
    Cpu::preemption_enable();

    for (cpu_t cpu { 0 }; cpu < Cpu::count; cpu++) {

        auto const ec { Ec::remote_current (cpu) };

        if (ec->regs.get_hst() != s && ec->regs.get_gst() != s)
            continue;

        if (Cpu::id == cpu) {
            Cpu::hazard |= Hazard::SCHED;
            continue;
        }

        auto const c { Counter::req[Interrupt::Request::RKE].get (cpu) };

        Interrupt::send_cpu (Interrupt::Request::RKE, cpu);

        Wait::until (1, [&] { return Counter::req[Interrupt::Request::RKE].get (cpu) != c; });
    }

    Cpu::preemption_disable();
}
