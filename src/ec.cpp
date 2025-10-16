/*
 * Execution Context
 *
 * Copyright (C) 2009-2011 Udo Steinberg <udo@hypervisor.org>
 * Economic rights: Technische Universitaet Dresden (Germany)
 *
 * Copyright (C) 2012-2013 Udo Steinberg, Intel Corporation.
 * Copyright (C) 2014 Udo Steinberg, FireEye, Inc.
 * Copyright (C) 2012-2023 Alexander Boettcher, Genode Labs GmbH.
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

#include "bits.hpp"
#include "ec.hpp"
#include "elf.hpp"
#include "hip.hpp"
#include "rcu.hpp"
#include "stdio.hpp"
#include "svm.hpp"
#include "vmx.hpp"
#include "vtlb.hpp"
#include "sm.hpp"
#include "pt.hpp"
#include "cpu.hpp"

Ec *Ec::current, *Ec::fpowner, *Ec::ec_idle;
Sm *Ec::auth_suspend;

uint64 Ec::killed_time[NUM_CPU];

// Constructors
Ec::Ec (Pd *own, void (*f)(), unsigned c) : Kobject (EC, static_cast<Space_obj *>(own)), cont (f), pd (own), cpu (static_cast<uint16>(c)), glb (true), evt (0), timeout (this)
{
    trace (TRACE_SYSCALL, "EC:%p created (PD:%p Kernel)", this, own);

    regs.vtlb = nullptr;
    regs.vmcs_state = nullptr;
    regs.vmcb_state = nullptr;
}

Ec::Ec (Pd *own, mword sel, Pd *p, void (*f)(), unsigned c, unsigned e, mword u, mword s, Pt *oom) : Kobject (EC, static_cast<Space_obj *>(own), sel, 0xd, free, pre_free), cont (f), pd (p), cpu (static_cast<uint16>(c)), glb (!!f), evt (e), timeout (this), user_utcb (u), xcpu_sm (nullptr), pt_oom (oom)
{
    // Make sure we have a PTAB for this CPU in the PD
    pd->Space_mem::init (pd->quota, c);

    regs.vtlb = nullptr;
    regs.vmcs_state = nullptr;
    regs.vmcb_state = nullptr;

    if (pt_oom && !pt_oom->add_ref())
        pt_oom = nullptr;

    if (u) {

        regs.cs  = SEL_USER_CODE;
        regs.ds  = SEL_USER_DATA;
        regs.es  = SEL_USER_DATA;
        regs.ss  = SEL_USER_DATA;
        regs.REG(fl) = Cpu::EFL_IF;

        if (glb)
            regs.REG(sp) = s;
        else
            regs.set_sp (s);

        utcb = new (pd->quota) Utcb;

        pd->Space_mem::insert (pd->quota, u, 0, Hpt::HPT_U | Hpt::HPT_W | Hpt::HPT_P, Buddy::ptr_to_phys (utcb));

        regs.dst_portal = PT_STARTUP;

        trace (TRACE_SYSCALL, "EC:%p created (PD:%p CPU:%#x UTCB:%#lx ESP:%lx EVT:%#x)", this, p, c, u, s, e);

        if (pd == &Pd::root)
            pd->insert_utcb (pd->quota, pd->mdb_cache, u, Buddy::ptr_to_phys(utcb) >> 12);

    } else {

        regs.dst_portal = VM_EXIT_STARTUP;
        regs.vtlb = new (pd->quota) Vtlb;
        regs.fpu_on = !Cmdline::fpu_lazy;

        if (Hip::feature() & Hip::FEAT_VMX) {
            mword host_cr3 = pd->loc[c].root(pd->quota) | (Cpu::feature (Cpu::PCID) ? pd->did : 0);

            auto vmcs = new (pd->quota) Vmcs (pd->quota,
                                              reinterpret_cast<mword>(sys_regs() + 1),
                                              pd->Space_pio::walk(pd->quota),
                                              host_cr3,
                                              pd->ept.root(pd->quota));

            regs.vmcs_state = new (pd->quota) Vmcs_state(*vmcs, cpu);

            regs.vmcs_state->make_current();

            regs.nst_ctrl<Vmcs>();

            regs.vmcs_state->clear();

            cont = send_msg<ret_user_vmresume>;
            trace (TRACE_SYSCALL, "EC:%p created (PD:%p VMCS:%p VTLB:%p)", this, p, regs.vmcs_state, regs.vtlb);

        } else if (Hip::feature() & Hip::FEAT_SVM) {
            if (pd->asid == Space_mem::NO_ASID_ID)
                pd->asid = Space_mem::asid_alloc.alloc();

            auto vmcb = new (pd->quota) Vmcb (pd->quota, pd->Space_pio::walk(pd->quota),
                                              pd->npt.root(pd->quota), unsigned(pd->asid));

            regs.vmcb_state = new (pd->quota) Vmcb_state(*vmcb, cpu);

            regs.vmcb_state->make_current();

            regs.REG(ax) = Buddy::ptr_to_phys (vmcb);
            regs.nst_ctrl<Vmcb>();

            regs.vmcb_state->clear();

            cont = send_msg<ret_user_vmrun>;
            trace (TRACE_SYSCALL, "EC:%p created (PD:%p VMCB:%p VTLB:%p)", this, p, regs.vmcb_state, regs.vtlb);
        }
    }
}

Ec::Ec (Pd *own, Pd *p, void (*f)(), unsigned c, Ec *clone) : Kobject (EC, static_cast<Space_obj *>(own), 0, 0xd, free, pre_free), cont (f), regs (clone->regs), rcap (clone), utcb (clone->utcb), pd (p), fpu (clone->fpu), cpu (static_cast<uint16>(c)), glb (!!f), evt (clone->evt), timeout (this), user_utcb (0), xcpu_sm (clone->xcpu_sm), pt_oom(clone->pt_oom)
{
    // Make sure we have a PTAB for this CPU in the PD
    pd->Space_mem::init (pd->quota, c);

    regs.vtlb = nullptr;
    regs.vmcs_state = nullptr;
    regs.vmcb_state = nullptr;

    if (rcap && !rcap->add_ref())
        rcap = nullptr;

    if (pt_oom && !pt_oom->add_ref())
        pt_oom = nullptr;
}

Ec::Ec (Pd *own, Pd *p, void (*f)(), unsigned c, Ec *clone, Pt *pt) : Kobject (EC, static_cast<Space_obj *>(own), clone->node_base, 0xd, free, pre_free), cont (f), regs (clone->regs), utcb (clone->utcb), pd (p), fpu (clone->fpu), cpu (static_cast<uint16>(c)), glb (!!f), evt (clone->evt), timeout (this), user_utcb (clone->user_utcb), xcpu_sm (clone->xcpu_sm), pt_oom(pt)
{
    if (EXPECT_FALSE((fpowner == clone) && clone->fpu && Cmdline::fpu_lazy)) {
        Fpu::enable();
        save_fpu();
        Fpu::disable();
    }

    clone->fpu = nullptr;
    clone->utcb = nullptr;
    clone->user_utcb = 0;

    // Make sure we have a PTAB for this CPU in the PD
    pd->Space_mem::init (pd->quota, c);

    regs.vtlb = nullptr;
    regs.vmcs_state = nullptr;
    regs.vmcb_state = nullptr;

    if (pt_oom && !pt_oom->add_ref())
        pt_oom = nullptr;
}

Ec::~Ec()
{
    if (xcpu_sm) {
        /* should never happen, Ec have to pass xcpu_return */
        trace (0, "invalid state, still have xcpu_sm cont=%p", cont);

        auto sm = xcpu_revert();

        if (sm)
            sm->up();
    }

    pre_free(this);

    if (partner)
        trace (0, "invalid state, still have partner");

    if (rcap && rcap->del_rcu())
        Rcu::call(rcap);

    if (pt_oom && pt_oom->del_ref())
        Pt::destroy(pt_oom);

    if (fpu)
        Fpu::destroy(fpu, *pd);

    if (this->time > this->time_m)
        Atomic::add(Ec::killed_time[this->cpu], this->time - this->time_m);

    if (utcb) {
        Utcb::destroy(utcb, pd->quota);
        return;
    }

    /* skip xCPU EC */
    if (!vcpu())
        return;

    /* vCPU cleanup */
    Vtlb::destroy(regs.vtlb, pd->quota);

    if ((Hip::feature() & Hip::FEAT_VMX) && regs.vmcs_state) {
        regs.vmcs_state->clear();
        Vmcs_state::destroy(regs.vmcs_state, pd->quota);
    }
    else
    if ((Hip::feature() & Hip::FEAT_SVM) && regs.vmcb_state) {
        regs.vmcb_state->clear();
        Vmcb_state::destroy(regs.vmcb_state, pd->quota);
    }
}

void Ec::handle_hazard (mword hzd, void (*func)())
{
    if (hzd & HZD_RCU)
        Rcu::quiet();

    if (hzd & HZD_SCHED) {
        current->cont = func;
        Sc::schedule();
    }

    if (hzd & HZD_RECALL) {
        current->regs.clr_hazard (HZD_RECALL);

        if (func == ret_user_vmresume) {
            current->regs.dst_portal = VM_EXIT_RECALL;
            send_msg<ret_user_vmresume>();
        }

        if (func == ret_user_vmrun) {
            current->regs.dst_portal = VM_EXIT_RECALL;
            send_msg<ret_user_vmrun>();
        }

        if (func == ret_user_sysexit)
            current->redirect_to_iret();

        current->regs.dst_portal = PT_RECALL;
        send_msg<ret_user_iret>();
    }

    if (hzd & HZD_STEP) {
        current->regs.clr_hazard (HZD_STEP);

        if (func == ret_user_sysexit)
            current->redirect_to_iret();

        current->regs.dst_portal = Cpu::EXC_DB;
        send_msg<ret_user_iret>();
    }

    if (hzd & HZD_TSC) {
        current->regs.clr_hazard (HZD_TSC);

        if (func == ret_user_vmresume) {
            current->regs.vmcs_state->make_current();
            Vmcs::write (Vmcs::TSC_OFFSET,    static_cast<mword>(current->regs.tsc_offset));
            Vmcs::write (Vmcs::TSC_OFFSET_HI, static_cast<mword>(current->regs.tsc_offset >> 32));
        } else
        if (func == ret_user_vmrun) {
            current->regs.vmcb_state->make_current();
            current->regs.vmcb_state->vmcb.tsc_offset = current->regs.tsc_offset;
        }
    }

    if (hzd & HZD_TSC_AUX) {
        current->regs.clr_hazard (HZD_TSC_AUX);

        if ((func == ret_user_vmresume) || (func == ret_user_vmrun))
            Msr::write (Msr::IA32_TSC_AUX, current->regs.tsc_aux);
        else
            Msr::write (Msr::IA32_TSC_AUX, Cpu::id);
    }

    if (hzd & HZD_DS_ES) {
        Cpu::hazard &= ~HZD_DS_ES;
        asm volatile ("mov %0, %%ds; mov %0, %%es" : : "r" (SEL_USER_DATA));
    }

    if (hzd & HZD_FPU) {
        if (!Cmdline::fpu_lazy)
            die ("FPU HZD detected");

        if (current != fpowner)
            Fpu::disable();
    }
}

void Ec::ret_user_sysexit()
{
    mword hzd = (Cpu::hazard | current->regs.hazard()) & (HZD_RECALL | HZD_STEP | HZD_RCU | HZD_FPU | HZD_DS_ES | HZD_SCHED | HZD_TSC_AUX);
    if (EXPECT_FALSE (hzd))
        handle_hazard (hzd, ret_user_sysexit);

    if (current->regs.ARG_IP >= USER_ADDR) {
        current->regs.dst_portal = 13;
        send_msg<Ec::ret_user_sysexit>();
    }

    asm volatile ("lea %0," EXPAND (PREG(sp); LOAD_GPR RET_USER_HYP) : : "m" (current->regs) : "memory");

    UNREACHED;
}

void Ec::ret_user_iret()
{
    // No need to check HZD_DS_ES because IRET will reload both anyway
    mword hzd = (Cpu::hazard | current->regs.hazard()) & (HZD_RECALL | HZD_STEP | HZD_RCU | HZD_FPU | HZD_SCHED | HZD_TSC_AUX);
    if (EXPECT_FALSE (hzd))
        handle_hazard (hzd, ret_user_iret);

    asm volatile ("lea %0," EXPAND (PREG(sp); LOAD_GPR LOAD_SEG RET_USER_EXC) : : "m" (current->regs) : "memory");

    UNREACHED;
}

void Ec::chk_kern_preempt()
{
    if (!Cpu::preemption)
        return;

    if (Cpu::hazard & HZD_SCHED) {
        Cpu::preempt_disable();
        Sc::schedule();
    }
}

void Ec::ret_user_vmresume()
{
    mword hzd = (Cpu::hazard | current->regs.hazard()) & (HZD_RECALL | HZD_TSC | HZD_TSC_AUX | HZD_RCU | HZD_SCHED);
    if (EXPECT_FALSE (hzd))
        handle_hazard (hzd, ret_user_vmresume);

    current->regs.vmcs_state->make_current();

    if (EXPECT_FALSE (Pd::current->gtlb.chk (Cpu::id))) {
        Pd::current->gtlb.clr (Cpu::id);
        if (current->regs.nst_on)
            Pd::current->ept.flush();
        else
            current->regs.vtlb->flush (true);
    }

    if (EXPECT_FALSE (get_cr2() != current->regs.cr2))
        set_cr2 (current->regs.cr2);

    Fpu::State_xsv::make_current (Fpu::hst_xsv, current->regs.gst_xsv);    // Restore XSV guest state

    asm volatile ("lea %0," EXPAND (PREG(sp); LOAD_GPR)
                  "vmresume;"
                  "vmlaunch;"
                  "mov %1," EXPAND (PREG(sp);)
                  : : "m" (current->regs), "i" (CPU_LOCAL_STCK + PAGE_SIZE) : "memory");

    Fpu::State_xsv::make_current (current->regs.gst_xsv, Fpu::hst_xsv);    // Restore XSV host state

    trace (0, "VM entry failed with error %#lx", Vmcs::read (Vmcs::VMX_INST_ERROR));

    die ("VMENTRY");
}

void Ec::ret_user_vmrun()
{
    mword hzd = (Cpu::hazard | current->regs.hazard()) & (HZD_RECALL | HZD_TSC | HZD_TSC_AUX | HZD_RCU | HZD_SCHED);
    if (EXPECT_FALSE (hzd))
        handle_hazard (hzd, ret_user_vmrun);

    current->regs.vmcb_state->make_current();

    if (EXPECT_FALSE (Pd::current->gtlb.chk (Cpu::id))) {
        Pd::current->gtlb.clr (Cpu::id);
        if (current->regs.nst_on)
            current->regs.vmcb_state->vmcb.tlb_control = 1;
        else
            current->regs.vtlb->flush (true);
    }

    Fpu::State_xsv::make_current (Fpu::hst_xsv, current->regs.gst_xsv);    // Restore XSV guest state

    asm volatile ("lea %0," EXPAND (PREG(sp); LOAD_GPR)
                  "clgi;"
                  "sti;"
                  "vmload;"
                  "vmrun;"
                  "vmsave;"
                  EXPAND (SAVE_GPR)
                  "mov %1," EXPAND (PREG(ax);)
                  "mov %2," EXPAND (PREG(sp);)
                  "vmload;"
                  "cli;"
                  "stgi;"
                  "jmp svm_handler;"
                  : : "m" (current->regs), "m" (Vmcb::root), "i" (CPU_LOCAL_STCK + PAGE_SIZE) : "memory");

    UNREACHED;
}

void Ec::idle()
{
    for (;;) {

        mword hzd = Cpu::hazard & (HZD_RCU | HZD_SCHED | HZD_TSC_AUX);
        if (EXPECT_FALSE (hzd))
            handle_hazard (hzd, idle);

        uint64 t1 = rdtsc();

        Cpu::halt_or_mwait([&]() {
            asm volatile ("sti; hlt; cli" : : : "memory");
        }, [&](auto const cstate_hint) {
            mword volatile dummy = 0;
            asm volatile ("monitor" :: "a" (&dummy), "c"(0), "d"(0) : "memory");
            asm volatile ("sti; mwait; cli;" :: "a"(cstate_hint), "c"(0) : "memory");
        });

        uint64 t2 = rdtsc();

        Counter::cycles_idle += t2 - t1;
    }
}

void Ec::root_invoke()
{
    /* transfer memory from second allocator */
    Quota tmp;
    bool ok = Quota::init.transfer_to(tmp, Quota::init.limit());
    assert(ok);
    ok = tmp.transfer_to(Pd::root.quota, tmp.limit());
    assert(ok);

    Eh *e = static_cast<Eh *>(Hpt::remap (Pd::kern.quota, Hip::root_addr));
    if (!Hip::root_addr || e->ei_magic != 0x464c457f || e->ei_class != ELF_CLASS || e->ei_data != 1 || e->type != 2 || e->machine != ELF_MACHINE)
        die ("No ELF");

    auto const hip_addr = USER_ADDR - PAGE_H_SIZE;

    unsigned count = e->ph_count;
    current->regs.set_pt (Cpu::id);
    current->regs.set_ip (e->entry);
    current->regs.set_sp (hip_addr);

    ELF_PHDR *p = static_cast<ELF_PHDR *>(Hpt::remap (Pd::kern.quota, Hip::root_addr + e->ph_offset));

    for (unsigned i = 0; i < count; i++, p++) {

        if (p->type == 1) {

            unsigned attr = !!(p->flags & 0x4) << 0 |   // R
                            !!(p->flags & 0x2) << 1 |   // W
                            !!(p->flags & 0x1) << 2;    // X

            if (p->f_size != p->m_size || p->v_addr % PAGE_SIZE != p->f_offs % PAGE_SIZE)
                die ("Bad ELF");

            mword phys = align_dn (p->f_offs + Hip::root_addr, PAGE_SIZE);
            mword virt = align_dn (p->v_addr, PAGE_SIZE);
            mword size = align_up (p->f_size, PAGE_SIZE);

            for (unsigned long o; size; size -= 1UL << o, phys += 1UL << o, virt += 1UL << o)
                Pd::current->delegate<Space_mem>(&Pd::kern, phys >> PAGE_BITS, virt >> PAGE_BITS, (o = min (max_order (phys, size), max_order (virt, size))) - PAGE_BITS, attr);
        }
    }

    // Map hypervisor information page
    {
        mword phys = align_dn (reinterpret_cast<Paddr>(&FRAME_H), PAGE_SIZE);
        mword virt = align_dn (hip_addr, PAGE_SIZE);
        mword size = align_up (PAGE_H_SIZE, PAGE_SIZE);

        for (unsigned long o; size; size -= 1UL << o, phys += 1UL << o, virt += 1UL << o)
            Pd::current->delegate<Space_mem>(&Pd::kern, phys >> PAGE_BITS, virt >> PAGE_BITS, (o = min (max_order (phys, size), max_order (virt, size))) - PAGE_BITS, 1);
    }

    Space_obj::insert_root (Pd::kern.quota, Pd::current);
    Space_obj::insert_root (Pd::kern.quota, Ec::current);
    Space_obj::insert_root (Pd::kern.quota, Sc::current);

    /* authority capability for ACPI suspend syscall */
    Ec::auth_suspend = new (Pd::root) Sm (&Pd::root, SM_ACPI_SUSPEND);
    auth_suspend->add_ref();
    Space_obj::insert_root (Pd::kern.quota, auth_suspend);

    /* capability for MSR user access */
    auto msr_cap = new (Pd::root) Sm (&Pd::root, SM_MSR_ACCESS);
    Msr::msr_cap = msr_cap;
    Space_obj::insert_root (Pd::kern.quota, Msr::msr_cap);
    msr_cap->add_ref();

    /* adjust root quota used by Pd::kern during bootstrap */
    Quota::boot(Pd::kern.quota, Pd::root.quota);

    /* preserve per CPU 4 pages quota */
    Quota cpus;
    bool s = Pd::root.quota.transfer_to(cpus, Cpu::online * 4);
    assert(s);

    /* preserve for the root task memory that is not transferable */
    bool res = Pd::root.quota.set_limit ((1 * 1024 * 1024) >> 12, 0, Pd::root.quota);
    assert (res);

    /* check PCID handling */
    assert (Pd::kern.did == 0);
    assert (Pd::root.did == 1);

    ret_user_sysexit();
}

void Ec::handle_tss()
{
    Console::panic ("Task gate invoked");
}

bool Ec::fixup (mword &eip)
{
    for (mword *ptr = &FIXUP_S; ptr < &FIXUP_E; ptr += 2)
        if (eip == *ptr) {
            eip = *++ptr;
            return true;
        }

    return false;
}

void Ec::die (char const *reason, Exc_regs *r)
{
    bool const root_pd = current->pd == &Pd::root;
    bool const kern_pd = current->pd == &Pd::kern;
    bool const show    = kern_pd || root_pd ||
                         (reason && strmatch(reason, "EXC", 3));

    if (!current->vcpu() || show) {
        bool const pt_err = reason && strmatch(reason, "PT not found", 12);
        if (show || (!pt_err && !Sc::current->disable)) {
            trace (0, "%sKilled EC:%p SC:%p%s V:%#lx CS:%#lx IP:%#lx(%#lx) CR2:%#lx ERR:%#lx CONT:%p (%s)%s",
                   root_pd ? "Pd::root " : (kern_pd ? "Pd::kern " : ""),
                   current, Sc::current, Sc::current->disable ? "_d" : "",
                   r->vec, r->cs, r->REG(ip), r->ARG_IP,
                   r->cr2, r->err, current->cont, reason,
                   r->user() ? "" : " - fault kernel");
        }
    }

    if (current->vcpu() && !show) {
        if (current->cont != dead && !Sc::current->disable)
            trace (0, "vCPU Killed EC:%p SC:%p%s V:%#lx CR0:%#lx CR3:%#lx CR4:%#lx CONT=%p (%s)",
                   current, Sc::current, Sc::current->disable ? "_d" : "",
                   r->vec, r->cr0_shadow, r->cr3_shadow,
                   r->cr4_shadow, current->cont, reason);
    }

    Ec *ec = current->rcap;

    if (ec)
        ec->cont = (ec->cont == ret_user_sysexit || ec->cont == xcpu_return)
                 ? static_cast<void (*)()>(sys_finish<Sys_regs::COM_ABT>)
                 : dead;

    reply (dead);
}

void Ec::xcpu_clone(Ec & from, uint16 const tcpu)
{
    cont = Ec::sys_call;
    cpu  = tcpu;

    regs            = from.regs;
    regs.vtlb       = nullptr;
    regs.vmcs_state = nullptr;
    regs.vmcb_state = nullptr;

    utcb    =  from.utcb;
    xcpu_sm =  from.xcpu_sm;

    // Make sure we have a PTAB for this CPU in the PD
    from.pd->Space_mem::init (from.pd->quota, cpu);
}

void Ec::xcpu_return()
{
    assert (current->xcpu_sm);
    assert (current->rcap);
    assert (current->utcb);
    assert (Sc::current->ec == current);

    auto sm = current->xcpu_revert();

    /* if last ref it will be handled by schedule_wo_activate */
    Sc::current->del_rcu();

    auto old = Sc::current;
    auto cur = Sc::schedule_wo_activate (true, true);

    /* keep track of time spent on this (remote) CPU */
    Atomic::add(Sc::cross_time[old->cpu], old->time);
    old->time = 0;

    /* wake original caller of remote CPU */
    if (sm)
        sm->up (ret_xcpu_reply);

    cur->ec->activate();
}

Sm * Ec::xcpu_revert()
{
    if (rcap) {
        *rcap->exc_regs() = regs;
         rcap->regs.mtd   = regs.mtd;

        if (rcap->fpu == fpu)
            fpu = nullptr;
    }

    auto sm = xcpu_sm;

    utcb    = nullptr;
    xcpu_sm = nullptr;
    cont    = dead;

    return sm;
}

void Ec::idl_handler()
{
    if (Ec::current->cont == Ec::idle)
        Rcu::update(false);
}

void Ec::hlt_prepare()
{
    if (Hip::feature() & Hip::FEAT_VMX) {
        Vmcs_state::flush_all_vmcs();

        Vmcs_state::vmxoff();
    } else
    if (Hip::feature() & Hip::FEAT_SVM) {
        Vmcb_state::flush_all_vmcb();
    }

    current->flush_fpu();
    Ec::ec_idle->pd->make_current();

    wbinvd();
}

void Ec::hlt_handler()
{
    hlt_prepare();
    shutdown();
}

void Ec::flush_from_cpu()
{
    if (Sc::current->cpu != cpu)
        return;

    if (fpowner == this) {

        fpowner->del_ref();

        fpowner = nullptr;

        if (Cmdline::fpu_lazy) {
            assert (!(Cpu::hazard & HZD_FPU));
            Fpu::disable();
            assert (!(Cpu::hazard & HZD_FPU));
        }
    }

    if (!vcpu())
        return;

    /* flush on right CPU, because of CPU local queues and Vmcs::current */
    if ((Hip::feature() & Hip::FEAT_VMX) && regs.vmcs_state)
        regs.vmcs_state->clear();
    else
    if ((Hip::feature() & Hip::FEAT_SVM) && regs.vmcb_state)
        regs.vmcb_state->clear();
}
