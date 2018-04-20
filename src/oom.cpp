/*
 * Out-of-memory handling of a PD
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


#include "ec.hpp"
#include "pt.hpp"
#include "utcb.hpp"
#include "sm.hpp"

void Ec::oom_delegate(Ec * dst_ec, Ec * rep_ec, Ec * src_ec, bool user, bool C)
{
    Pt * const dst_pt = dst_ec->pt_oom;
    Pt * const src_pt = src_ec->pt_oom;

    if (!dst_pt) {
        dst_ec->pd->quota.dump(dst_ec->pd);
        die ("PT not found - OOM");
    }

    if (user) {
        if (C) {
             assert (rep_ec->cont == ret_user_sysexit || rep_ec->cont == xcpu_return);
             assert (dst_ec->cont == recv_user);
        } else 
             assert (dst_ec->cont == ret_user_sysexit || dst_ec->cont == xcpu_return);

        assert(dst_ec->utcb);

        dst_ec->pd->rev_crd (dst_ec->utcb->del, true, false, false);
    } else {
        assert(!C);
        assert(src_ec->utcb);

        Xfer *s = src_ec->utcb->xfer();
        for (unsigned long ti = src_ec->utcb->ti(); ti--; s--) {
            if ((s->flags() >> 8) & 1)
                continue;
            src_ec->pd->rev_crd (*s, false, false, false);
        }
    }

    mword src_pd_id = !src_pt ? ~0UL : 0;
    if (src_pt && src_pt->ec->pd == dst_pt->ec->pd)
        src_pd_id = src_pt->id;

    Ec * const ec = dst_pt->ec;

    enum { OOM_SEND = 1, OOM_REPLY = 2, OOM_SELF = 4 };

    mword oom_state = C ? OOM_SEND : OOM_REPLY;
    if (EXPECT_FALSE (current == ec))
        oom_state += OOM_SELF;

    if (!C && current->cpu != ec->xcpu) {
        assert (!current->partner);
        current->oom_xcpu<Ec::sys_reply>(dst_pt, src_pd_id, oom_state);
    }

    if (EXPECT_FALSE (current->cpu != ec->xcpu) || (current->cont == xcpu_return) || (current->cont == ret_xcpu_reply))
        die ("PT wrong CPU - OOM");

    if (C && current == ec) {
        assert (rep_ec->partner == current);
        rep_ec->cont = sys_call;
        current->oom_call(dst_pt, src_pd_id, oom_state, nullptr /* unused */, nullptr /* unused */);
    }

    if (C && current != ec) {
        bool clr = rep_ec->clr_partner();
        assert(clr);

        /* current thread has no IPC relation to dst anymore */
        current->cont = nullptr;
    }

    Ec * chg = C ? rep_ec : current;
    void (*c)() = C ? sys_call : sys_reply;
    void (*h)() = C ? current->cont : sys_reply;

    chg->oom_call(dst_pt, src_pd_id, oom_state, c, h);
}

void Ec::oom_call(Pt * pt, mword src, mword state, void (*CC)(), void (*HELP)())
{
    Ec *ec = pt->ec;

    assert (this->cpu == ec->xcpu);

    if (this != ec) {
        assert (!this->partner);
        this->cont = CC;

        if (ec->cont)
            ec->help (HELP);

        this->set_partner (ec);
    }

    ec->cont = ret_user_sysexit;
    ec->regs.set_pt (pt->id, src, state);
    ec->regs.set_ip (pt->ip);
    ec->make_current();
}

template <void (*C)()>
void Ec::oom_xcpu(Pt * pt, mword src_pd_id, mword oom_state)
{
    assert(current == this);
    assert(!this->xcpu_sm);

    enum { UNUSED = 0, CNT = 0 };

    this->xcpu_sm = new (Pd::current->quota) Sm (Pd::current, UNUSED, CNT);

    Ec *xcpu_ec = new (Pd::current->quota) Ec (Pd::current, Pd::current, sys_xcpu_call_oom<C>, pt->ec->cpu, this);
    xcpu_ec->regs.set_pt (reinterpret_cast<mword>(pt), src_pd_id, oom_state);

    Sc *xcpu_sc = new (Pd::current->quota) Sc (Pd::current, xcpu_ec, xcpu_ec->cpu, Sc::current);

    xcpu_sc->remote_enqueue();
    this->xcpu_sm->dn (false, 0);

    die ("XCPU OOM error");
}

template <void (*C)()>
void Ec::oom_xcpu_return()
{
    assert (current->xcpu_sm);
    assert (current->rcap);
    assert (current->utcb);
    assert (Sc::current->ec == current);

    current->xcpu_sm->up (C);

    current->rcap    = nullptr;
    current->utcb    = nullptr;
    current->fpu     = nullptr;
    current->xcpu_sm = nullptr;

    Rcu::call(current);
    Rcu::call(Sc::current);

    Sc::schedule(true);
}

template <void (*C)()>
void Ec::ret_xcpu_reply_oom()
{
    assert (current->xcpu_sm);

    Sm::destroy(current->xcpu_sm, Pd::current->quota);
    current->xcpu_sm = nullptr;

    current->cont = C;
    current->make_current();
}

template <void (*C)()>
void Ec::sys_xcpu_call_oom()
{
    assert (current->xcpu_sm);

    Sys_regs *s  = current->sys_regs();
    Pt       *pt = reinterpret_cast<Pt *>(s->ARG_1);
    current->oom_call(pt, s->ARG_2, s->ARG_3, oom_xcpu_return<ret_xcpu_reply_oom<C>>, sys_xcpu_call_oom<C>);
}

void Ec::oom_call_cpu(Pt * pt, mword src, void (*CC)(), void (*HELP)())
{
    enum { OOM_SEND = 1, OOM_REPLY = 2, OOM_SELF = 4 };
    mword s = OOM_SEND | (this == pt->ec) ? OOM_SELF : 0;

    if (this->cpu != pt->ec->xcpu) {
        if (CC == sys_call) this->oom_xcpu<sys_call>(pt, src, s); else
        if (CC == sys_lookup) this->oom_xcpu<sys_lookup>(pt, src, s); else
        if (CC == sys_sm_ctrl) this->oom_xcpu<sys_sm_ctrl>(pt, src, s); else
        if (CC == sys_ec_ctrl) this->oom_xcpu<sys_ec_ctrl>(pt, src, s); else
        if (CC == sys_sc_ctrl) this->oom_xcpu<sys_sc_ctrl>(pt, src, s); else
        if (CC == sys_pt_ctrl) this->oom_xcpu<sys_pt_ctrl>(pt, src, s); else
        if (CC == sys_pd_ctrl) this->oom_xcpu<sys_pd_ctrl>(pt, src, s); else
        if (CC == sys_assign_gsi) this->oom_xcpu<sys_assign_gsi>(pt, src, s); else
        if (CC == sys_assign_pci) this->oom_xcpu<sys_assign_pci>(pt, src, s); else
        die ("Unknown oom call");
    }

    oom_call(pt, src, s, CC, HELP);
}
