/*
 * Execution Context
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
#include "lapic.hpp"

INIT_PRIORITY (PRIO_SLAB)
Slab_cache Ec::cache (sizeof (Ec), 32);

Ec *Ec::current, *Ec::fpowner;

// Constructors
Ec::Ec (Pd *own, void (*f)(), unsigned c) : Kobject (EC, static_cast<Space_obj *>(own)), cont (f), utcb (nullptr), pd (own), partner (nullptr), prev (nullptr), next (nullptr), fpu (nullptr), cpu (static_cast<uint16>(c)), glb (true), evt (0), timeout (this), user_utcb (0), xcpu_sm (nullptr), pt_oom(nullptr)
{
    trace (TRACE_SYSCALL, "EC:%p created (PD:%p Kernel)", this, own);

    regs.vtlb = nullptr;
    regs.vmcs = nullptr;
    regs.vmcb = nullptr;
}

Ec::Ec (Pd *own, mword sel, Pd *p, void (*f)(), unsigned c, unsigned e, mword u, mword s, Pt *oom) : Kobject (EC, static_cast<Space_obj *>(own), sel, 0xd, free, pre_free), cont (f), pd (p), partner (nullptr), prev (nullptr), next (nullptr), fpu (nullptr), cpu (static_cast<uint16>(c)), glb (!!f), evt (e), timeout (this), user_utcb (u), xcpu_sm (nullptr), pt_oom (oom)
{
    // Make sure we have a PTAB for this CPU in the PD
    pd->Space_mem::init (pd->quota, c);

    regs.vtlb = nullptr;
    regs.vmcs = nullptr;
    regs.vmcb = nullptr;

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

        regs.dst_portal = NUM_EXC - 2;

        trace (TRACE_SYSCALL, "EC:%p created (PD:%p CPU:%#x UTCB:%#lx ESP:%lx EVT:%#x)", this, p, c, u, s, e);

        if (pd == &Pd::root)
            pd->insert_utcb (pd->quota, u, Buddy::ptr_to_phys(utcb) >> 12);

    } else {

        utcb = nullptr;

        regs.dst_portal = NUM_VMI - 2;
        regs.vtlb = new (pd->quota) Vtlb;

        if (Hip::feature() & Hip::FEAT_VMX) {

            regs.vmcs = new (pd->quota) Vmcs (reinterpret_cast<mword>(sys_regs() + 1),
                                              pd->Space_pio::walk(pd->quota),
                                              pd->loc[c].root(pd->quota),
                                              pd->ept.root(pd->quota));

            regs.nst_ctrl<Vmcs>();

            /* allocate and register the host MSR area */
            mword host_msr_area_phys = Buddy::ptr_to_phys(new (pd->quota) Msr_area);
            Vmcs::write(Vmcs::EXI_MSR_LD_ADDR, host_msr_area_phys);
            Vmcs::write(Vmcs::EXI_MSR_LD_CNT, Msr_area::MSR_COUNT);

            /* allocate and register the guest MSR area */
            mword guest_msr_area_phys = Buddy::ptr_to_phys(new (pd->quota) Msr_area);
            Vmcs::write(Vmcs::ENT_MSR_LD_ADDR, guest_msr_area_phys);
            Vmcs::write(Vmcs::ENT_MSR_LD_CNT, Msr_area::MSR_COUNT);
            Vmcs::write(Vmcs::EXI_MSR_ST_ADDR, guest_msr_area_phys);
            Vmcs::write(Vmcs::EXI_MSR_ST_CNT, Msr_area::MSR_COUNT);

            /* allocate and register the virtual APIC page */
            mword virtual_apic_page_phys = Buddy::ptr_to_phys(new (pd->quota) Virtual_apic_page);
            Vmcs::write(Vmcs::APIC_VIRT_ADDR, virtual_apic_page_phys);

            regs.vmcs->clear();
            cont = send_msg<ret_user_vmresume>;
            trace (TRACE_SYSCALL, "EC:%p created (PD:%p VMCS:%p VTLB:%p)", this, p, regs.vmcs, regs.vtlb);

        } else if (Hip::feature() & Hip::FEAT_SVM) {

            regs.REG(ax) = Buddy::ptr_to_phys (regs.vmcb = new (pd->quota) Vmcb (pd->quota, pd->Space_pio::walk(pd->quota), pd->npt.root(pd->quota)));

            regs.nst_ctrl<Vmcb>();
            cont = send_msg<ret_user_vmrun>;
            trace (TRACE_SYSCALL, "EC:%p created (PD:%p VMCB:%p VTLB:%p)", this, p, regs.vmcb, regs.vtlb);
        }
    }
}

Ec::Ec (Pd *own, Pd *p, void (*f)(), unsigned c, Ec *clone) : Kobject (EC, static_cast<Space_obj *>(own), 0, 0xd, free, pre_free), cont (f), regs (clone->regs), rcap (clone), utcb (clone->utcb), pd (p), partner (nullptr), prev (nullptr), next (nullptr), fpu (clone->fpu), cpu (static_cast<uint16>(c)), glb (!!f), evt (clone->evt), timeout (this), user_utcb (0), xcpu_sm (clone->xcpu_sm), pt_oom(clone->pt_oom)
{
    // Make sure we have a PTAB for this CPU in the PD
    pd->Space_mem::init (pd->quota, c);

    regs.vtlb = nullptr;
    regs.vmcs = nullptr;
    regs.vmcb = nullptr;

    if (pt_oom && !pt_oom->add_ref())
        pt_oom = nullptr;
}

//De-constructor
Ec::~Ec()
{
    if (xcpu_sm) {
        Sm::destroy(xcpu_sm, pd->quota);
        xcpu_sm = nullptr;
    }

    pre_free(this);

    if (pt_oom && pt_oom->del_ref())
        Pt::destroy(pt_oom, pd->quota);

    if (fpu)
        Fpu::destroy(fpu, pd->quota);

    if (utcb) {
        Utcb::destroy(utcb, pd->quota);
        return;
    }

    /* skip xCPU EC */
    if (!regs.vtlb)
        return;

    /* vCPU cleanup */
    Vtlb::destroy(regs.vtlb, pd->quota);

    if (Hip::feature() & Hip::FEAT_VMX) {

        regs.vmcs->make_current();

        mword host_msr_area_phys = Vmcs::read(Vmcs::EXI_MSR_LD_ADDR);
        Msr_area *host_msr_area = reinterpret_cast<Msr_area*>(Buddy::phys_to_ptr(host_msr_area_phys));
        Msr_area::destroy(host_msr_area, pd->quota);

        mword guest_msr_area_phys = Vmcs::read(Vmcs::EXI_MSR_ST_ADDR);
        Msr_area *guest_msr_area = reinterpret_cast<Msr_area*>(Buddy::phys_to_ptr(guest_msr_area_phys));
        Msr_area::destroy(guest_msr_area, pd->quota);

        mword virtual_apic_page_phys = Vmcs::read(Vmcs::APIC_VIRT_ADDR);
        Virtual_apic_page *virtual_apic_page =
            reinterpret_cast<Virtual_apic_page*>(Buddy::phys_to_ptr(virtual_apic_page_phys));
        Virtual_apic_page::destroy(virtual_apic_page, pd->quota);

        regs.vmcs->clear();

        Vmcs::destroy(regs.vmcs, pd->quota);
    } else if (Hip::feature() & Hip::FEAT_SVM)
        Vmcb::destroy(regs.vmcb, pd->quota);
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
            current->regs.dst_portal = NUM_VMI - 1;
            send_msg<ret_user_vmresume>();
        }

        if (func == ret_user_vmrun) {
            current->regs.dst_portal = NUM_VMI - 1;
            send_msg<ret_user_vmrun>();
        }

        if (func == ret_user_sysexit)
            current->redirect_to_iret();

        current->regs.dst_portal = NUM_EXC - 1;
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
            current->regs.vmcs->make_current();
            Vmcs::write (Vmcs::TSC_OFFSET,    static_cast<mword>(current->regs.tsc_offset));
            Vmcs::write (Vmcs::TSC_OFFSET_HI, static_cast<mword>(current->regs.tsc_offset >> 32));
        } else
            current->regs.vmcb->tsc_offset = current->regs.tsc_offset;
    }

    if (hzd & HZD_DS_ES) {
        Cpu::hazard &= ~HZD_DS_ES;
        asm volatile ("mov %0, %%ds; mov %0, %%es" : : "r" (SEL_USER_DATA));
    }

    if (hzd & HZD_FPU)
        if (current != fpowner)
            Fpu::disable();
}

void Ec::ret_user_sysexit()
{
    mword hzd = (Cpu::hazard | current->regs.hazard()) & (HZD_RECALL | HZD_STEP | HZD_RCU | HZD_FPU | HZD_DS_ES | HZD_SCHED);
    if (EXPECT_FALSE (hzd))
        handle_hazard (hzd, ret_user_sysexit);

    if (current->regs.ARG_IP >= USER_ADDR) {
        current->regs.dst_portal = 13;
        send_msg<Ec::ret_user_sysexit>();
    }
    
    Lapic::update_counter();
    Console::print("SysExit Counter %llx", Msr::read<uint64>(Msr::MSR_PERF_FIXED_CTR0));

    asm volatile ("lea %0," EXPAND (PREG(sp); LOAD_GPR RET_USER_HYP) : : "m" (current->regs) : "memory");

    UNREACHED;
}

void Ec::ret_user_iret()
{
    // No need to check HZD_DS_ES because IRET will reload both anyway
    mword hzd = (Cpu::hazard | current->regs.hazard()) & (HZD_RECALL | HZD_STEP | HZD_RCU | HZD_FPU | HZD_SCHED);
    if (EXPECT_FALSE (hzd))
        handle_hazard (hzd, ret_user_iret);

    Console::print("Iret Counter %llx", Msr::read<uint64>(Msr::MSR_PERF_FIXED_CTR0));
    Lapic::program_pmi();
    
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
    mword hzd = (Cpu::hazard | current->regs.hazard()) & (HZD_RECALL | HZD_TSC | HZD_RCU | HZD_SCHED);
    if (EXPECT_FALSE (hzd))
        handle_hazard (hzd, ret_user_vmresume);

    current->regs.vmcs->make_current();

    if (EXPECT_FALSE (Pd::current->gtlb.chk (Cpu::id))) {
        Pd::current->gtlb.clr (Cpu::id);
        if (current->regs.nst_on)
            Pd::current->ept.flush();
        else
            current->regs.vtlb->flush (true);
    }

    if (EXPECT_FALSE (get_cr2() != current->regs.cr2))
        set_cr2 (current->regs.cr2);

    asm volatile ("lea %0," EXPAND (PREG(sp); LOAD_GPR)
                  "vmresume;"
                  "vmlaunch;"
                  "mov %1," EXPAND (PREG(sp);)
                  : : "m" (current->regs), "i" (CPU_LOCAL_STCK + PAGE_SIZE) : "memory");

    trace (0, "VM entry failed with error %#lx", Vmcs::read (Vmcs::VMX_INST_ERROR));

    die ("VMENTRY");
}

void Ec::ret_user_vmrun()
{
    mword hzd = (Cpu::hazard | current->regs.hazard()) & (HZD_RECALL | HZD_TSC | HZD_RCU | HZD_SCHED);
    if (EXPECT_FALSE (hzd))
        handle_hazard (hzd, ret_user_vmrun);

    if (EXPECT_FALSE (Pd::current->gtlb.chk (Cpu::id))) {
        Pd::current->gtlb.clr (Cpu::id);
        if (current->regs.nst_on)
            current->regs.vmcb->tlb_control = 1;
        else
            current->regs.vtlb->flush (true);
    }

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

        mword hzd = Cpu::hazard & (HZD_RCU | HZD_SCHED);
        if (EXPECT_FALSE (hzd))
            handle_hazard (hzd, idle);

        uint64 t1 = rdtsc();
        asm volatile ("sti; hlt; cli" : : : "memory");
        uint64 t2 = rdtsc();

        Counter::cycles_idle += t2 - t1;
    }
}

void Ec::root_invoke()
{
    Eh *e = static_cast<Eh *>(Hpt::remap (Pd::kern.quota, Hip::root_addr));
    if (!Hip::root_addr || e->ei_magic != 0x464c457f || e->ei_class != ELF_CLASS || e->ei_data != 1 || e->type != 2 || e->machine != ELF_MACHINE)
        die ("No ELF");

    unsigned count = e->ph_count;
    current->regs.set_pt (Cpu::id);
    current->regs.set_ip (e->entry);
    current->regs.set_sp (USER_ADDR - PAGE_SIZE);

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
    Pd::current->delegate<Space_mem>(&Pd::kern, reinterpret_cast<Paddr>(&FRAME_H) >> PAGE_BITS, (USER_ADDR - PAGE_SIZE) >> PAGE_BITS, 0, 1);

    Space_obj::insert_root (Pd::kern.quota, Pd::current);
    Space_obj::insert_root (Pd::kern.quota, Ec::current);
    Space_obj::insert_root (Pd::kern.quota, Sc::current);

    /* adjust root quota used by Pd::kern during bootstrap */
    Quota::boot(Pd::kern.quota, Pd::root.quota);

    /* preserve per CPU 4 pages quota */
    Quota cpus;
    bool s = Pd::root.quota.transfer_to(cpus, Cpu::online * 4);
    assert(s);

    /* preserve for the root task memory that is not transferable */
    bool res = Pd::root.quota.set_limit ((1 * 1024 * 1024) >> 12, 0, Pd::root.quota);
    assert (res);

    /* setup PCID handling */
    Space_mem::boot_init();
    assert (Pd::kern.did == 0);
    assert (Pd::root.did == 1);

    /* quirk */
    if (Dpt::ord != ~0UL && Dpt::ord > 0x8) {
       trace (0, "disabling super pages for DMAR");
       Dpt::ord = 0x8;
    }

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
    bool const show = current->pd == &Pd::kern || current->pd == &Pd::root;

    if (current->utcb || show) {
        if (show || !strmatch(reason, "PT not found", 12))
        trace (0, "Killed EC:%p SC:%p V:%#lx CS:%#lx IP:%#lx(%#lx) CR2:%#lx ERR:%#lx (%s) %s",
               current, Sc::current, r->vec, r->cs, r->REG(ip), r->ARG_IP, r->cr2, r->err, reason, current->pd == &Pd::root ? "Pd::root" : current->pd == &Pd::kern ? "Pd::kern" : "");
    } else
        trace (0, "Killed EC:%p SC:%p V:%#lx CR0:%#lx CR3:%#lx CR4:%#lx (%s)",
               current, Sc::current, r->vec, r->cr0_shadow, r->cr3_shadow, r->cr4_shadow, reason);

    Ec *ec = current->rcap;

    if (ec)
        ec->cont = ec->cont == ret_user_sysexit ? static_cast<void (*)()>(sys_finish<Sys_regs::COM_ABT>) : dead;

    reply (dead);
}

void Ec::xcpu_return()
{
    assert (current->xcpu_sm);
    assert (current->rcap);
    assert (current->utcb);
    assert (Sc::current->ec == current);

    current->rcap->regs =  current->regs;

    current->xcpu_sm->up (ret_xcpu_reply);

    current->rcap    = nullptr;
    current->utcb    = nullptr;
    current->fpu     = nullptr;
    current->xcpu_sm = nullptr;

    Rcu::call(current);
    Rcu::call(Sc::current);

    Sc::schedule(true);
}

void Ec::idl_handler()
{
    if (Ec::current->cont == Ec::idle)
        Rcu::update();
}
