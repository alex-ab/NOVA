/*
 * System-Call Interface
 *
 * Copyright (C) 2009-2011 Udo Steinberg <udo@hypervisor.org>
 * Economic rights: Technische Universitaet Dresden (Germany)
 *
 * Copyright (C) 2012-2013 Udo Steinberg, Intel Corporation.
 * Copyright (C) 2014 Udo Steinberg, FireEye, Inc.
 * Copyright (C) 2013-2015 Alexander Boettcher, Genode Labs GmbH
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

#include "dmar.hpp"
#include "gsi.hpp"
#include "hip.hpp"
#include "hpet.hpp"
#include "lapic.hpp"
#include "pci.hpp"
#include "pt.hpp"
#include "sm.hpp"
#include "stdio.hpp"
#include "syscall.hpp"
#include "utcb.hpp"
#include "vectors.hpp"

template <Sys_regs::Status S, bool T>
void Ec::sys_finish()
{
    if (T)
        current->clr_timeout();

    current->regs.set_status (S);

    if (current->xcpu_sm)
        xcpu_return();

    if (Pd::current->quota.hit_limit() && S != Sys_regs::QUO_OOM)
        trace (TRACE_OOM, "warning: insufficient resources %lu/%lu", Pd::current->quota.usage(), Pd::current->quota.limit());

    ret_user_sysexit();
}

void Ec::activate()
{
    Ec *ec = this;

    // XXX: Make the loop preemptible
    for (Sc::ctr_link = 0; ec->partner; ec = ec->partner)
        Sc::ctr_link++;

    if (EXPECT_FALSE (ec->blocked()))
        ec->block_sc();

    ec->make_current();
}

template <bool C>
void Ec::delegate()
{
    Ec *ec = current->rcap;
    assert (ec);

    Ec *src = C ? ec : current;
    Ec *dst = C ? current : ec;

    bool user = C || ((dst->cont == ret_user_sysexit) || (dst->cont == xcpu_return));

    dst->pd->xfer_items (src->pd,
                         user ? dst->utcb->xlt : Crd (0),
                         user ? dst->utcb->del : Crd (Crd::MEM, (dst->cont == ret_user_iret ? dst->regs.cr2 : dst->regs.nst_fault) >> PAGE_BITS),
                         src->utcb->xfer(),
                         user ? dst->utcb->xfer() : nullptr,
                         src->utcb->ti());

    if (Cpu::hazard & HZD_OOM) {
        if (dst->pd->quota.hit_limit())
            trace (TRACE_OOM, "warning: insufficient resources %lx/%lx", dst->pd->quota.usage(), dst->pd->quota.limit());

        Cpu::hazard &= ~HZD_OOM;
        current->oom_delegate(dst, ec, src, user, C);
    }
}

template <void (*C)()>
void Ec::send_msg()
{
    Exc_regs *r = &current->regs;

    Kobject *obj = Space_obj::lookup (current->evt + r->dst_portal).obj();
    if (EXPECT_FALSE (obj->type() != Kobject::PT))
        die ("PT not found");

    Pt *pt = static_cast<Pt *>(obj);
    Ec *ec = pt->ec;

    if (EXPECT_FALSE (current->cpu != ec->xcpu))
        die ("PT wrong CPU");

    if (EXPECT_TRUE (!ec->cont)) {
        current->cont = C;
        current->set_partner (ec);
        current->regs.mtd = pt->mtd.val;
        ec->cont = recv_kern;
        ec->regs.set_pt (pt->id);
        ec->regs.set_ip (pt->ip);
        ec->make_current();
    }

    ec->help (send_msg<C>);

    die ("IPC Timeout");
}

void Ec::sys_call()
{
    Sys_call *s = static_cast<Sys_call *>(current->sys_regs());

    Kobject *obj = Space_obj::lookup (s->pt()).obj();
    if (EXPECT_FALSE (obj->type() != Kobject::PT)) {
//        trace (TRACE_ERROR, "%s: Bad PT CAP (%#lx)", __func__, s->pt());
        sys_finish<Sys_regs::BAD_CAP>();
    }

    Pt *pt = static_cast<Pt *>(obj);
    Ec *ec = pt->ec;

    if (Pd::current->quota.hit_limit()) {

        if (!current->pt_oom)
            sys_finish<Sys_regs::QUO_OOM>();

        if (current->xcpu_sm) {
            current->regs.set_status (Sys_regs::QUO_OOM, false);
            xcpu_return();
        }

        current->oom_call_cpu (current->pt_oom, current->pt_oom->id, sys_call, sys_call);
        sys_finish<Sys_regs::QUO_OOM>();
    }

    if (EXPECT_FALSE (current->cpu != ec->xcpu))
        Ec::sys_xcpu_call();

    if (EXPECT_TRUE (!ec->cont)) {
        current->cont = current->xcpu_sm ? xcpu_return : ret_user_sysexit;
        current->set_partner (ec);
        ec->cont = recv_user;
        ec->regs.set_pt (pt->id);
        ec->regs.set_ip (pt->ip);
        ec->make_current();
    }

    if (EXPECT_TRUE (!(s->flags() & Sys_call::DISABLE_BLOCKING)))
        ec->help (sys_call);

    sys_finish<Sys_regs::COM_TIM>();
}

void Ec::recv_kern()
{
    Ec *ec = current->rcap;

    bool fpu = false;

    if (ec->cont == ret_user_iret)
        fpu = current->utcb->load_exc (&ec->regs);
    else if (ec->cont == ret_user_vmresume)
        fpu = current->utcb->load_vmx (&ec->regs);
    else if (ec->cont == ret_user_vmrun)
        fpu = current->utcb->load_svm (&ec->regs);

    if (EXPECT_FALSE (fpu))
        ec->transfer_fpu (current);

    ret_user_sysexit();
}

void Ec::recv_user()
{
    Ec *ec = current->rcap;

    ec->utcb->save (current->utcb);

    if (EXPECT_FALSE (ec->utcb->tcnt()))
        delegate<true>();

    ret_user_sysexit();
}

void Ec::reply (void (*c)(), Sm * sm)
{
    current->cont = c;

    if (EXPECT_FALSE (current->glb))
        Sc::schedule (true);

    Ec *ec = current->rcap;

    if (EXPECT_FALSE (!ec))
        Sc::current->ec->activate();

    bool clr = ec->clr_partner();

    if (Sc::current->ec == ec && Sc::current->last_ref())
        Sc::schedule (true);

    if (sm)
        sm->dn (false, 0, ec, clr);

    if (!clr)
        Sc::current->ec->activate();

    ec->make_current();
}

void Ec::sys_reply()
{
    Ec *ec = current->rcap;
    Sm *sm = nullptr;

    if (EXPECT_TRUE (ec)) {

        enum { SYSCALL_REPLY = 1 };

        Sys_reply *r = static_cast<Sys_reply *>(current->sys_regs());

        if (EXPECT_FALSE (current->cont == sys_reply && current->regs.status() != SYSCALL_REPLY)) {
            sm = reinterpret_cast<Sm *>(r->sm_kern());
            current->regs.set_pt(SYSCALL_REPLY);
        } else {
            if (EXPECT_FALSE (r->sm())) {
                Capability cap = Space_obj::lookup (r->sm());
                if (EXPECT_TRUE (cap.obj()->type() == Kobject::SM && (cap.prm() & 2)))
                    sm = static_cast<Sm *>(cap.obj());
            }
        }

        if (EXPECT_FALSE (sm)) {
            if (ec->cont == ret_user_sysexit)
                ec->cont = sys_call;
            else if (ec->cont == xcpu_return)
                ec->regs.set_status (Sys_regs::BAD_HYP, false);
            else if (ec->cont == sys_reply) {
                assert (ec->regs.status() == SYSCALL_REPLY);
                ec->regs.set_pt(reinterpret_cast<mword>(sm));
                assert (ec->regs.status() != SYSCALL_REPLY);
                reply();
            }
        }

        Utcb *src = current->utcb;

        if (EXPECT_FALSE (src->tcnt()))
            delegate<false>();

        bool fpu = false;

        assert (current->cont != ret_xcpu_reply);

        if (EXPECT_TRUE ((ec->cont == ret_user_sysexit) || ec->cont == xcpu_return))
            src->save (ec->utcb);
        else if (ec->cont == ret_user_iret)
            fpu = src->save_exc (&ec->regs);
        else if (ec->cont == ret_user_vmresume)
            fpu = src->save_vmx (&ec->regs);
        else if (ec->cont == ret_user_vmrun)
            fpu = src->save_svm (&ec->regs);

        if (EXPECT_FALSE (fpu))
            current->transfer_fpu (ec);
    }

    reply(nullptr, sm);
}

template <void(*C)()>
void Ec::check(mword r, bool call)
{
    if (Pd::current->quota.hit_limit(r)) {
        trace(TRACE_OOM, "%s:%u - not enough resources %lu/%lu (%lu)", __func__, __LINE__, Pd::current->quota.usage(), Pd::current->quota.limit(), r);

        if (Ec::current->pt_oom && call)
            Ec::current->oom_call_cpu (Ec::current->pt_oom, Ec::current->pt_oom->id, C, C);

        sys_finish<Sys_regs::QUO_OOM>();
    }
}

void Ec::sys_create_pd()
{
    check<sys_create_pd>(0, false);

    Sys_create_pd *r = static_cast<Sys_create_pd *>(current->sys_regs());

    trace (TRACE_SYSCALL, "EC:%p SYS_CREATE PD:%#lx", current, r->sel());

    Capability cap = Space_obj::lookup (r->pd());
    if (EXPECT_FALSE (cap.obj()->type() != Kobject::PD) || !(cap.prm() & 1UL << Kobject::PD)) {
        trace (TRACE_ERROR, "%s: Non-PD CAP (%#lx)", __func__, r->pd());
        sys_finish<Sys_regs::BAD_CAP>();
    }
    Pd * pd_src = static_cast<Pd *>(cap.obj());

    if (r->limit_lower() > r->limit_upper())
        sys_finish<Sys_regs::BAD_PAR>();

    if (pd_src->quota.hit_limit(1)) {
        trace(TRACE_OOM, "%s:%u - not enough resources %lu/%lu", __func__, __LINE__, pd_src->quota.usage(), pd_src->quota.limit());
        sys_finish<Sys_regs::QUO_OOM>();
    }

    Pd *pd = new (Pd::current->quota) Pd (Pd::current, r->sel(), cap.prm());

    if (!pd->quota.set_limit(r->limit_lower(), r->limit_upper(), pd_src->quota)) {
        trace (0, "Insufficient kernel memory for creating new PD");
        delete pd;
        sys_finish<Sys_regs::BAD_PAR>();
    }

    if (!Space_obj::insert_root (pd->quota, pd)) {
        trace (TRACE_ERROR, "%s: Non-NULL CAP (%#lx)", __func__, r->sel());
        delete pd;
        sys_finish<Sys_regs::BAD_CAP>();
    }

    Crd crd = r->crd();
    pd->del_crd (Pd::current, Crd (Crd::OBJ), crd);

    sys_finish<Sys_regs::SUCCESS>();
}

void Ec::sys_create_ec()
{
    check<sys_create_ec>(0, false);

    Sys_create_ec *r = static_cast<Sys_create_ec *>(current->sys_regs());

    trace (TRACE_SYSCALL, "EC:%p SYS_CREATE EC:%#lx CPU:%#x UTCB:%#lx ESP:%#lx EVT:%#x", current, r->sel(), r->cpu(), r->utcb(), r->esp(), r->evt());

    if (EXPECT_FALSE (!Hip::cpu_online (r->cpu()))) {
        trace (TRACE_ERROR, "%s: Invalid CPU (%#x)", __func__, r->cpu());
        sys_finish<Sys_regs::BAD_CPU>();
    }

    if (EXPECT_FALSE (!r->utcb() && !(Hip::feature() & (Hip::FEAT_VMX | Hip::FEAT_SVM)))) {
        trace (TRACE_ERROR, "%s: VCPUs not supported", __func__);
        sys_finish<Sys_regs::BAD_FTR>();
    }

    Capability cap_pd = Space_obj::lookup (r->pd());
    if (EXPECT_FALSE (cap_pd.obj()->type() != Kobject::PD) || !(cap_pd.prm() & 1UL << Kobject::EC)) {
        trace (TRACE_ERROR, "%s: Non-PD CAP (%#lx)", __func__, r->pd());
        sys_finish<Sys_regs::BAD_CAP>();
    }
    Pd *pd = static_cast<Pd *>(cap_pd.obj());

    if (pd->quota.hit_limit(7)) {
        trace(TRACE_OOM, "%s:%u - not enough resources %lu/%lu", __func__, __LINE__, pd->quota.usage(), pd->quota.limit());
        sys_finish<Sys_regs::QUO_OOM>();
    }

    if (EXPECT_FALSE (r->utcb() >= USER_ADDR || r->utcb() & PAGE_MASK || !pd->insert_utcb (pd->quota, r->utcb()))) {
        trace (TRACE_ERROR, "%s: Invalid UTCB address (%#lx)", __func__, r->utcb());
        sys_finish<Sys_regs::BAD_PAR>();
    }

    Capability cap_pt = Space_obj::lookup (r->sel() + 1);
    Pt *pt = cap_pt.obj()->type() == Kobject::PT ? static_cast<Pt *>(cap_pt.obj()) : nullptr;

    Ec *ec = new (pd->quota) Ec (Pd::current, r->sel(), pd, r->flags() & 1 ? static_cast<void (*)()>(send_msg<ret_user_iret>) : nullptr, r->cpu(), r->evt(), r->utcb(), r->esp(), pt);

    if (!Space_obj::insert_root (pd->quota, ec)) {
        trace (TRACE_ERROR, "%s: Non-NULL CAP (%#lx)", __func__, r->sel());
        Ec::destroy (ec, ec->pd->quota);
        sys_finish<Sys_regs::BAD_CAP>();
    }

    sys_finish<Sys_regs::SUCCESS>();
}

void Ec::sys_create_sc()
{
    check<sys_create_sc>(0, false);

    Sys_create_sc *r = static_cast<Sys_create_sc *>(current->sys_regs());

    trace (TRACE_SYSCALL, "EC:%p SYS_CREATE SC:%#lx EC:%#lx P:%#x Q:%#x", current, r->sel(), r->ec(), r->qpd().prio(), r->qpd().quantum());

    Capability cap = Space_obj::lookup (r->pd());
    if (EXPECT_FALSE (cap.obj()->type() != Kobject::PD) || !(cap.prm() & 1UL << Kobject::SC)) {
        trace (TRACE_ERROR, "%s: Non-PD CAP (%#lx)", __func__, r->pd());
        sys_finish<Sys_regs::BAD_CAP>();
    }
    Pd *pd = static_cast<Pd *>(cap.obj());

    if (pd->quota.hit_limit(2)) {
        trace(TRACE_OOM, "%s:%u - not enough resources %lu/%lu", __func__, __LINE__, pd->quota.usage(), pd->quota.limit());
        sys_finish<Sys_regs::QUO_OOM>();
    }

    Capability cap_sc = Space_obj::lookup (r->ec());
    if (EXPECT_FALSE (cap_sc.obj()->type() != Kobject::EC) || !(cap_sc.prm() & 1UL << Kobject::SC)) {
        trace (TRACE_ERROR, "%s: Non-EC CAP (%#lx)", __func__, r->ec());
        sys_finish<Sys_regs::BAD_CAP>();
    }
    Ec *ec = static_cast<Ec *>(cap_sc.obj());

    if (EXPECT_FALSE (!ec->glb)) {
        trace (TRACE_ERROR, "%s: Cannot bind SC", __func__);
        sys_finish<Sys_regs::BAD_CAP>();
    }

    if (EXPECT_FALSE (!r->qpd().prio() || !r->qpd().quantum() | (r->qpd().prio() >= Sc::priorities))) {
        trace (TRACE_ERROR, "%s: Invalid QPD", __func__);
        sys_finish<Sys_regs::BAD_PAR>();
    }

    Sc *sc = new (pd->quota) Sc (Pd::current, r->sel(), ec, ec->cpu, r->qpd().prio(), r->qpd().quantum());
    if (!Space_obj::insert_root (pd->quota, sc)) {
        trace (TRACE_ERROR, "%s: Non-NULL CAP (%#lx)", __func__, r->sel());
        delete sc;
        sys_finish<Sys_regs::BAD_CAP>();
    }

    sc->remote_enqueue();

    sys_finish<Sys_regs::SUCCESS>();
}

void Ec::sys_create_pt()
{
    check<sys_create_pt>(0, false);

    Sys_create_pt *r = static_cast<Sys_create_pt *>(current->sys_regs());

    trace (TRACE_SYSCALL, "EC:%p SYS_CREATE PT:%#lx EC:%#lx EIP:%#lx", current, r->sel(), r->ec(), r->eip());

    if (EXPECT_FALSE (r->eip() >= USER_ADDR)) {
        trace (TRACE_ERROR, "%s: Invalid instruction pointer (%#lx)", __func__, r->eip());
        sys_finish<Sys_regs::BAD_PAR>();
    }

    Capability cap = Space_obj::lookup (r->pd());
    if (EXPECT_FALSE (cap.obj()->type() != Kobject::PD) || !(cap.prm() & 1UL << Kobject::PT)) {
        trace (TRACE_ERROR, "%s: Non-PD CAP (%#lx)", __func__, r->pd());
        sys_finish<Sys_regs::BAD_CAP>();
    }
    Pd *pd = static_cast<Pd *>(cap.obj());

    if (pd->quota.hit_limit(2)) {
        trace(TRACE_OOM, "%s:%u - not enough resources %lu/%lu", __func__, __LINE__, pd->quota.usage(), pd->quota.limit());
        sys_finish<Sys_regs::QUO_OOM>();
    }

    Capability cap_ec = Space_obj::lookup (r->ec());
    if (EXPECT_FALSE (cap_ec.obj()->type() != Kobject::EC) || !(cap_ec.prm() & 1UL << Kobject::PT)) {
        trace (TRACE_ERROR, "%s: Non-EC CAP (%#lx)", __func__, r->ec());
        sys_finish<Sys_regs::BAD_CAP>();
    }
    Ec *ec = static_cast<Ec *>(cap_ec.obj());

    if (EXPECT_FALSE (ec->glb)) {
        trace (TRACE_ERROR, "%s: Cannot bind PT", __func__);
        sys_finish<Sys_regs::BAD_CAP>();
    }

    Pt *pt = new (pd->quota) Pt (Pd::current, r->sel(), ec, r->mtd(), r->eip());
    if (!Space_obj::insert_root (pd->quota, pt)) {
        trace (TRACE_ERROR, "%s: Non-NULL CAP (%#lx)", __func__, r->sel());
        Pt::destroy (pt, pd->quota);
        sys_finish<Sys_regs::BAD_CAP>();
    }

    sys_finish<Sys_regs::SUCCESS>();
}

void Ec::sys_create_sm()
{
    check<sys_create_sm>(0, false);

    Sys_create_sm *r = static_cast<Sys_create_sm *>(current->sys_regs());

    trace (TRACE_SYSCALL, "EC:%p SYS_CREATE SM:%#lx CNT:%lu", current, r->sel(), r->cnt());

    Capability cap = Space_obj::lookup (r->pd());
    if (EXPECT_FALSE (cap.obj()->type() != Kobject::PD) || !(cap.prm() & 1UL << Kobject::SM)) {
        trace (TRACE_ERROR, "%s: Non-PD CAP (%#lx)", __func__, r->pd());
        sys_finish<Sys_regs::BAD_CAP>();
    }
    Pd *pd = static_cast<Pd *>(cap.obj());

    if (pd->quota.hit_limit(1)) {
        trace(TRACE_OOM, "%s:%u - not enough resources %lu/%lu", __func__, __LINE__, pd->quota.usage(), pd->quota.limit());
        sys_finish<Sys_regs::QUO_OOM>();
    }

    Sm * sm;

    if (r->sm()) {
        /* check for valid SM to be chained with */
        Capability cap_si = Space_obj::lookup (r->sm());
        if (EXPECT_FALSE (cap_si.obj()->type() != Kobject::SM)) {
            trace (TRACE_ERROR, "%s: Non-SM CAP (%#lx)", __func__, r->sm());
            sys_finish<Sys_regs::BAD_CAP>();
        }

        Sm * si = static_cast<Sm *>(cap_si.obj());
        if (si->is_signal()) {
            /* limit chaining to solely one level */
            trace (TRACE_ERROR, "%s: SM CAP (%#lx) is signal", __func__, r->sm());
            sys_finish<Sys_regs::BAD_CAP>();
        }

        sm = new (pd->quota) Sm (Pd::current, r->sel(), 0, si, r->cnt());
    } else
        sm = new (pd->quota) Sm (Pd::current, r->sel(), r->cnt());

    if (!Space_obj::insert_root (pd->quota, sm)) {
        trace (TRACE_ERROR, "%s: Non-NULL CAP (%#lx)", __func__, r->sel());
        Sm::destroy(sm, pd->quota);
        sys_finish<Sys_regs::BAD_CAP>();
    }

    sys_finish<Sys_regs::SUCCESS>();
}

void Ec::sys_revoke()
{
    Sys_revoke *r = static_cast<Sys_revoke *>(current->sys_regs());

    trace (TRACE_SYSCALL, "EC:%p SYS_REVOKE", current);

    Pd * pd = Pd::current;

    if (current->cont != sys_revoke) {
        if (r->remote()) {
            Capability cap = Space_obj::lookup (r->pd());
            if (EXPECT_FALSE (cap.obj()->type() != Kobject::PD)) {
                trace (TRACE_ERROR, "%s: Bad PD CAP (%#lx)", __func__, r->pd());
                sys_finish<Sys_regs::BAD_CAP>();
            }
            pd = static_cast<Pd *>(cap.obj());
            if (!pd->add_ref())
                sys_finish<Sys_regs::BAD_CAP>();
        }
        current->cont = sys_revoke;

        r->rem(pd);
    } else
        pd = reinterpret_cast<Pd *>(r->pd());

    pd->rev_crd (r->crd(), r->self(), true, r->keep());

    current->cont = sys_finish<Sys_regs::SUCCESS>;
    r->rem(nullptr);

    if (r->remote() && pd->del_rcu())
        Rcu::call(pd);

    if (EXPECT_FALSE (r->sm())) {
        Capability cap_sm = Space_obj::lookup (r->sm());
        if (EXPECT_FALSE (cap_sm.obj()->type() == Kobject::SM && (cap_sm.prm() & 1))) {
            Sm *sm = static_cast<Sm *>(cap_sm.obj());
            sm->add_to_rcu();
        }
    }

    sys_finish<Sys_regs::SUCCESS>();
}

void Ec::sys_lookup()
{
    check<sys_lookup>(2);

    Sys_lookup *s = static_cast<Sys_lookup *>(current->sys_regs());

    if (s->flags()) {
        trace (TRACE_SYSCALL, "EC:%p SYS_DELEGATE PD:%lx->%lx T:%d B:%#lx", current, s->pd_snd(), s->pd_dst(), s->crd().type(), s->crd().base());

        Kobject *obj_dst = Space_obj::lookup (s->pd_dst()).obj();
        if (EXPECT_FALSE (obj_dst->type() != Kobject::PD)) {
            trace (TRACE_ERROR, "%s: Non-PD CAP (%#lx)", __func__, s->pd_dst());
            sys_finish<Sys_regs::BAD_CAP>();
        }
        Kobject *obj_snd = Space_obj::lookup (s->pd_snd()).obj();
        if (EXPECT_FALSE (obj_snd->type() != Kobject::PD)) {
            trace (TRACE_ERROR, "%s: Non-PD CAP (%#lx)", __func__, s->pd_dst());
            sys_finish<Sys_regs::BAD_CAP>();
        }

        Pd * pd_dst = static_cast<Pd *>(obj_dst);
        Pd * pd_snd = static_cast<Pd *>(obj_snd);

        pd_dst->xfer_items (pd_snd,
                            Crd (0),
                            s->crd(),
                            current->utcb->xfer(),
                            nullptr,
                            current->utcb->ti());

        if (Cpu::hazard & HZD_OOM) {
           Cpu::hazard &= ~HZD_OOM;
           sys_finish<Sys_regs::QUO_OOM>();
        }

        sys_finish<Sys_regs::SUCCESS>();
    }

    trace (TRACE_SYSCALL, "EC:%p SYS_LOOKUP T:%d B:%#lx", current, s->crd().type(), s->crd().base());

    Space *space; Mdb *mdb;
    if ((space = Pd::current->subspace (s->crd().type())) && (mdb = space->tree_lookup (s->crd().base())))
        s->crd() = Crd (s->crd().type(), mdb->node_base, mdb->node_order, mdb->node_attr);
    else
        s->crd() = Crd (0);

    sys_finish<Sys_regs::SUCCESS>();
}

void Ec::sys_ec_ctrl()
{
    check<sys_ec_ctrl>(1);

    Sys_ec_ctrl *r = static_cast<Sys_ec_ctrl *>(current->sys_regs());

    switch (r->op()) {
        case 0:
        {
            Capability cap = Space_obj::lookup (r->ec());
            if (EXPECT_FALSE (cap.obj()->type() != Kobject::EC || !(cap.prm() & 1UL << 0))) {
               trace (TRACE_ERROR, "%s: Bad EC CAP (%#lx)", __func__, r->ec());
               sys_finish<Sys_regs::BAD_CAP>();
            }

            Ec *ec = static_cast<Ec *>(cap.obj());

            if (!(ec->regs.hazard() & HZD_RECALL)) {

                ec->regs.set_hazard (HZD_RECALL);

                if (Cpu::id != ec->cpu && Ec::remote (ec->cpu) == ec) {
                    Lapic::send_ipi (ec->cpu, VEC_IPI_RKE);
                    if (r->state())
                        sys_finish<Sys_regs::COM_TIM>();
                }
            }

            if (!(r->state() && current->utcb))
                break;

            Cpu_regs regs(ec->regs);

            regs.mtd = Mtd::GPR_ACDB |
                       Mtd::GPR_BSD |
#ifdef __x86_64__
                       Mtd::GPR_R8_R15 |
#endif
                       Mtd::RSP |
                       Mtd::RIP_LEN |
                       Mtd::RFLAGS |
                       Mtd::QUAL;

            if (((ec->cont != ret_user_iret) && (ec->cont != recv_kern))) {
                /* in syscall */
                regs.REG(ip) = ec->regs.ARG_IP;
                regs.REG(sp) = ec->regs.ARG_SP;
            }

            /*
             * Find out if the EC is in exception handling state, which is the
             * case if it has called an exception handler portal. The exception
             * numbers in the comparison are the ones handled as exception in
             * 'entry.S'. Page fault exceptions are not of interest for GDB,
             * which is currently the only user of this status information.
             */
            if ((ec->cont == ret_user_iret) &&
                (ec->partner != nullptr) && (ec->partner->cont == recv_kern) &&
                ((regs.dst_portal <= 0x01) ||
                 ((regs.dst_portal >= 0x03) && (regs.dst_portal <= 0x07)) ||
                 ((regs.dst_portal >= 0x0a) && (regs.dst_portal <= 0x0d)) ||
                 ((regs.dst_portal >= 0x10) && (regs.dst_portal <= 0x13)))) {
                /* 'regs.err' will be transferred into utcb->qual[0] */
                regs.err = 1;
            } else
                regs.err = 0;

            bool fpu = current->utcb->load_exc (&regs);
            /* we don't really reload state of different threads - ignore */
            (void)fpu;
            break;
        }

        case 1: /* yield */
            current->cont = sys_finish<Sys_regs::SUCCESS>;
            Sc::schedule (false, false);
            break;

        case 2: /* helping */
        {
            Kobject *obj = Space_obj::lookup (r->ec()).obj();

            if (EXPECT_FALSE (obj->type() != Kobject::EC))
                sys_finish<Sys_regs::BAD_CAP>();

            Ec *ec = static_cast<Ec *>(obj);

            if (EXPECT_FALSE(ec->cpu != current->cpu))
                sys_finish<Sys_regs::BAD_CPU>();

            if (EXPECT_FALSE(!ec->utcb || ec->blocked() || ec->partner || ec->pd != Ec::current->pd || (r->cnt() != ec->utcb->tls)))
                sys_finish<Sys_regs::BAD_PAR>();

            current->cont = sys_finish<Sys_regs::SUCCESS>;
            ec->make_current();

            break;
        }

        case 3: /* re-schedule */
            current->cont = sys_finish<Sys_regs::SUCCESS>;
            Sc::schedule (false, true);
            break;

        default:
            sys_finish<Sys_regs::BAD_PAR>();
    }

    sys_finish<Sys_regs::SUCCESS>();
}

void Ec::sys_sc_ctrl()
{
    check<sys_sc_ctrl>(1);

    Sys_sc_ctrl *r = static_cast<Sys_sc_ctrl *>(current->sys_regs());

    Capability cap = Space_obj::lookup (r->sc());
    if (EXPECT_FALSE (cap.obj()->type() != Kobject::SC || !(cap.prm() & 1UL << 0))) {
        trace (TRACE_ERROR, "%s: Bad SC CAP (%#lx)", __func__, r->sc());
        sys_finish<Sys_regs::BAD_CAP>();
    }

    uint32 dummy;
    r->set_time (div64 (static_cast<Sc *>(cap.obj())->time * 1000, Lapic::freq_tsc, &dummy));

    sys_finish<Sys_regs::SUCCESS>();
}

void Ec::sys_pt_ctrl()
{
    check<sys_pt_ctrl>(1);

    Sys_pt_ctrl *r = static_cast<Sys_pt_ctrl *>(current->sys_regs());

    Capability cap = Space_obj::lookup (r->pt());
    if (EXPECT_FALSE (cap.obj()->type() != Kobject::PT || !(cap.prm() & 1UL << 0))) {
        trace (TRACE_ERROR, "%s: Bad PT CAP (%#lx)", __func__, r->pt());
        sys_finish<Sys_regs::BAD_CAP>();
    }

    Pt *pt = static_cast<Pt *>(cap.obj());

    pt->set_id (r->id());

    sys_finish<Sys_regs::SUCCESS>();
}

void Ec::sys_sm_ctrl()
{
    check<sys_sm_ctrl>(1);

    Sys_sm_ctrl *r = static_cast<Sys_sm_ctrl *>(current->sys_regs());
    Capability cap = Space_obj::lookup (r->sm());

    if (EXPECT_FALSE (cap.obj()->type() != Kobject::SM || !(cap.prm() & 1UL << r->op()))) {
//        trace (TRACE_ERROR, "%s: Bad SM CAP (%#lx)", __func__, r->sm());
        sys_finish<Sys_regs::BAD_CAP>();
    }

    Sm *sm = static_cast<Sm *>(cap.obj());

    switch (r->op()) {

        case 0:
            sm->submit();
            break;

        case 1:
            if (sm->space == static_cast<Space_obj *>(&Pd::kern)) {
                Gsi::unmask (static_cast<unsigned>(sm->node_base - NUM_CPU));
                if (sm->is_signal())
                    break;
            }

            if (sm->is_signal())
                sys_finish<Sys_regs::BAD_CAP>();

            current->cont = Ec::sys_finish<Sys_regs::SUCCESS, true>;
            sm->dn (r->zc(), r->time());
            break;
    }

    sys_finish<Sys_regs::SUCCESS>();
}

void Ec::sys_pd_ctrl()
{
    check<sys_pd_ctrl>(1);

    Sys_pd_ctrl *r = static_cast<Sys_pd_ctrl *>(current->sys_regs());

    Capability cap = Space_obj::lookup (r->src());
    if (EXPECT_FALSE (cap.obj()->type() != Kobject::PD)) {
        trace (TRACE_ERROR, "%s: Bad src PD CAP (%#lx)", __func__, r->src());
        sys_finish<Sys_regs::BAD_CAP>();
    }
    Pd *src = static_cast<Pd *>(cap.obj());

    if (r->dbg()) {
        r->dump(src->quota.limit(), src->quota.usage());
        sys_finish<Sys_regs::SUCCESS>();
    }

    Capability cap_pd = Space_obj::lookup (r->dst());
    if (EXPECT_FALSE (cap_pd.obj()->type() != Kobject::PD)) {
        trace (TRACE_ERROR, "%s: Bad dst PD CAP (%#lx)", __func__, r->dst());
        sys_finish<Sys_regs::BAD_CAP>();
    }
    Pd *dst = static_cast<Pd *>(cap_pd.obj());

    if (!src->quota.transfer_to(dst->quota, r->tra())) {
        trace (TRACE_ERROR, "%s: PD %p has insufficient kernel memory quota", __func__, src);
        sys_finish<Sys_regs::BAD_PAR>();
    }

    sys_finish<Sys_regs::SUCCESS>();
}

void Ec::sys_assign_pci()
{
    check<sys_assign_pci>(4);

    Sys_assign_pci *r = static_cast<Sys_assign_pci *>(current->sys_regs());

    Kobject *obj = Space_obj::lookup (r->pd()).obj();
    if (EXPECT_FALSE (obj->type() != Kobject::PD)) {
        trace (TRACE_ERROR, "%s: Non-PD CAP (%#lx)", __func__, r->pd());
        sys_finish<Sys_regs::BAD_CAP>();
    }

    Pd * pd = static_cast<Pd *>(obj);

    if (pd->quota.hit_limit(4)) {
        trace(TRACE_OOM, "%s:%u - not enough resources %lu/%lu", __func__, __LINE__, pd->quota.usage(), pd->quota.limit());
        sys_finish<Sys_regs::QUO_OOM>();
    }

    Paddr phys; unsigned rid;
    if (EXPECT_FALSE (!pd->Space_mem::lookup (r->dev(), phys) || (rid = Pci::phys_to_rid (phys)) == ~0U)) {
        trace (TRACE_ERROR, "%s: Non-DEV CAP (%#lx)", __func__, r->dev());
        sys_finish<Sys_regs::BAD_DEV>();
    }

    Dmar *dmar = Pci::find_dmar (r->hnt());
    if (EXPECT_FALSE (!dmar)) {
        trace (TRACE_ERROR, "%s: Invalid Hint (%#lx)", __func__, r->hnt());
        sys_finish<Sys_regs::BAD_DEV>();
    }

    dmar->assign (rid, static_cast<Pd *>(obj));

    sys_finish<Sys_regs::SUCCESS>();
}

void Ec::sys_assign_gsi()
{
    check<sys_assign_gsi>(2);

    Sys_assign_gsi *r = static_cast<Sys_assign_gsi *>(current->sys_regs());

    if (EXPECT_FALSE (!Hip::cpu_online (r->cpu()))) {
        trace (TRACE_ERROR, "%s: Invalid CPU (%#x)", __func__, r->cpu());
        sys_finish<Sys_regs::BAD_CPU>();
    }

    Kobject *obj = Space_obj::lookup (r->sm()).obj();
    if (EXPECT_FALSE (obj->type() != Kobject::SM)) {
        trace (TRACE_ERROR, "%s: Non-SM CAP (%#lx)", __func__, r->sm());
        sys_finish<Sys_regs::BAD_CAP>();
    }

    Sm *sm = static_cast<Sm *>(obj);

    if (EXPECT_FALSE (sm->space != static_cast<Space_obj *>(&Pd::kern))) {
        trace (TRACE_ERROR, "%s: Non-GSI SM (%#lx)", __func__, r->sm());
        sys_finish<Sys_regs::BAD_CAP>();
    }

    if (r->si() != ~0UL) {
        Kobject *obj_si = Space_obj::lookup (r->si()).obj();
        if (EXPECT_FALSE (obj_si->type() != Kobject::SM)) {
            trace (TRACE_ERROR, "%s: Non-SI CAP (%#lx)", __func__, r->si());
            sys_finish<Sys_regs::BAD_CAP>();
        }

        Sm *si = static_cast<Sm *>(obj_si);

        if (si == sm) {
            sm->chain(nullptr);
            sys_finish<Sys_regs::SUCCESS>();
        }

        if (EXPECT_FALSE (si->space == static_cast<Space_obj *>(&Pd::kern))) {
            trace (TRACE_ERROR, "%s: Invalid-SM CAP (%#lx)", __func__, r->si());
            sys_finish<Sys_regs::BAD_CAP>();
        }

        sm->chain(si);
    }

    Paddr phys; unsigned rid = 0, gsi = static_cast<unsigned>(sm->node_base - NUM_CPU);
    if (EXPECT_FALSE (!Gsi::gsi_table[gsi].ioapic && (!Pd::current->Space_mem::lookup (r->dev(), phys) || ((rid = Pci::phys_to_rid (phys)) == ~0U && (rid = Hpet::phys_to_rid (phys)) == ~0U)))) {
        trace (TRACE_ERROR, "%s: Non-DEV CAP (%#lx)", __func__, r->dev());
        sys_finish<Sys_regs::BAD_DEV>();
    }

    r->set_msi (Gsi::set (gsi, r->cpu(), rid));

    sys_finish<Sys_regs::SUCCESS>();
}

void Ec::sys_xcpu_call()
{
    Sys_call *s = static_cast<Sys_call *>(current->sys_regs());

    Kobject *obj = Space_obj::lookup (s->pt()).obj();
    if (EXPECT_FALSE (obj->type() != Kobject::PT)) {
        trace (TRACE_ERROR, "%s: Bad PT CAP (%#lx)", __func__, s->pt());
        sys_finish<Sys_regs::BAD_CAP>();
    }

    Pt *pt = static_cast<Pt *>(obj);
    Ec *ec = pt->ec;

    if (EXPECT_FALSE (current->cpu == ec->cpu)) {
        trace (TRACE_ERROR, "%s: Bad CPU", __func__);
        sys_finish<Sys_regs::BAD_CPU>();
    }

    enum { UNUSED = 0, CNT = 0 };

    current->xcpu_sm = new (Pd::current->quota) Sm (Pd::current, UNUSED, CNT);

    Ec *xcpu_ec = new (Pd::current->quota) Ec (Pd::current, Pd::current, Ec::sys_call, ec->cpu, current);
    Sc *xcpu_sc = new (Pd::current->quota) Sc (Pd::current, xcpu_ec, xcpu_ec->cpu, Sc::current);

    xcpu_sc->remote_enqueue();
    current->xcpu_sm->dn (false, 0);

    ret_xcpu_reply();
}

void Ec::ret_xcpu_reply()
{
    assert (current->xcpu_sm);

    Sm::destroy(current->xcpu_sm, Pd::current->quota);
    current->xcpu_sm = nullptr;

    if (current->regs.status() != Sys_regs::SUCCESS) {
        current->cont = sys_call;
        current->regs.set_status (Sys_regs::SUCCESS, false);
    } else
        current->cont = ret_user_sysexit;

    current->make_current();
}

extern "C"
void (*const syscall[])() =
{
    &Ec::sys_call,
    &Ec::sys_reply,
    &Ec::sys_create_pd,
    &Ec::sys_create_ec,
    &Ec::sys_create_sc,
    &Ec::sys_create_pt,
    &Ec::sys_create_sm,
    &Ec::sys_revoke,
    &Ec::sys_lookup,
    &Ec::sys_ec_ctrl,
    &Ec::sys_sc_ctrl,
    &Ec::sys_pt_ctrl,
    &Ec::sys_sm_ctrl,
    &Ec::sys_assign_pci,
    &Ec::sys_assign_gsi,
    &Ec::sys_pd_ctrl,
};

template void Ec::sys_finish<Sys_regs::COM_ABT>();
template void Ec::send_msg<Ec::ret_user_vmresume>();
template void Ec::send_msg<Ec::ret_user_vmrun>();
template void Ec::send_msg<Ec::ret_user_iret>();
template void Ec::send_msg<Ec::ret_user_sysexit>();
