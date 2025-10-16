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

#pragma once

#include "counter.hpp"
#include "fpu.hpp"
#include "mtd.hpp"
#include "pd.hpp"
#include "queue.hpp"
#include "regs.hpp"
#include "sc.hpp"
#include "timeout_hypercall.hpp"
#include "tss.hpp"
#include "si.hpp"
#include "cmdline.hpp"

#include "stdio.hpp"

class Utcb;
class Sm;
class Pt;
class Sys_ec_ctrl;

class Ec : public Kobject, public Refcount, public Queue<Sc>
{
    friend class Queue<Ec>;
    friend class Sc;
    friend class Pt;

    private:
        void        (*cont)() ALIGNED (16);
        Cpu_regs    regs { };
        Ec *        rcap { nullptr };
        Utcb *      utcb { nullptr };
        Refptr<Pd>  pd;
        Ec *        partner { };
        Ec *        prev    { };
        Ec *        next    { };
        Fpu *       fpu     { };
        Ec *        ec_xcpu { };
        Sc *        sc_xcpu { };

        union {
            struct {
                uint16  cpu;
                uint16  glb;
            };
            uint32  xcpu;
        };
        unsigned const evt;
        Timeout_hypercall timeout;
        mword          user_utcb { };

        Sm *         xcpu_sm { };
        Pt *         pt_oom  { };

        uint64      tsc  { 0 };
        uint64      time { 0 };
        uint64      time_m { 0 };

        static uint64 killed_time[NUM_CPU];

        static Sm * auth_suspend;

        REGPARM (1)
        static void handle_exc (Exc_regs *) asm ("exc_handler");

        NORETURN
        static void handle_vmx() asm ("vmx_handler");

        NORETURN
        static void handle_svm() asm ("svm_handler");

        NORETURN
        static void handle_tss() asm ("tss_handler");

        static void handle_exc_nm();
        static bool handle_exc_ts (Exc_regs *);
        static bool handle_exc_gp (Exc_regs *);
        static bool handle_exc_pf (Exc_regs *);

        static inline uint8 ifetch (mword);

        NORETURN
        static inline void svm_exception (mword);

        NORETURN
        static inline void svm_cr(mword);

        NORETURN
        static inline void svm_invlpg();

        NORETURN
        static inline void vmx_exception();

        NORETURN
        static inline void vmx_extint();

        NORETURN
        static inline void vmx_invlpg();

        NORETURN
        static inline void vmx_cr();

        static bool fixup (mword &);

        NOINLINE
        static void handle_hazard (mword, void (*)());

        static void pre_free (Rcu_elem * a)
        {
            Ec * e = static_cast<Ec *>(a);

            assert(e);

            // remove mapping in page table
            if (e->user_utcb) {
                e->pd->remove_utcb(e->user_utcb);
                e->pd->Space_mem::insert (e->pd->quota, e->user_utcb, 0, 0, 0);
                e->user_utcb = 0;
            }

            e->flush_from_cpu();

            if (e->ec_xcpu) {
                auto ec = e->ec_xcpu;
                e->ec_xcpu = nullptr;
                Rcu::call(ec);
            }

            if (e->sc_xcpu) {
                auto sc = e->sc_xcpu;
                e->sc_xcpu = nullptr;
                Rcu::call(sc);
            }
        }

        ALWAYS_INLINE
        static inline void destroy (Ec *obj, Pd &pd) { obj->~Ec(); pd.ec_cache.free (obj, pd.quota); }

        ALWAYS_INLINE
        inline bool idle_ec() { return ec_idle == this; }

        static void free (Rcu_elem * a)
        {
            Ec * e = static_cast<Ec *>(a);

            if (e->del_ref()) {
                assert(e != Ec::current);
                Ec::destroy (e, *e->pd);
            }
        }

        ALWAYS_INLINE
        inline Sys_regs *sys_regs() { return &regs; }

        ALWAYS_INLINE
        inline Exc_regs *exc_regs() { return &regs; }

        ALWAYS_INLINE
        inline void set_partner (Ec *p)
        {
            partner = p;
            bool ok = partner->add_ref();
            assert (ok);
            partner->rcap = this;
            ok = partner->rcap->add_ref();
            assert (ok);
            Sc::ctr_link++;
        }

        ALWAYS_INLINE
        inline unsigned clr_partner()
        {
            assert (partner == current);

            if (partner->rcap) {
                Ec * ec = partner->rcap;
                partner->rcap = nullptr;
                if (ec->del_rcu())
                    Rcu::call(ec);
            }

            Ec * ec = partner;
            partner = nullptr;
            if (ec->del_rcu())
                Rcu::call(ec);

            return Sc::ctr_link--;
        }

        ALWAYS_INLINE
        inline void redirect_to_iret()
        {
            regs.REG(sp) = regs.ARG_SP;
            regs.REG(ip) = regs.ARG_IP;
        }

        void load_fpu();
        void save_fpu();

        void import_fpu_data (void *);
        void export_fpu_data (void *);

        void claim_fpu ();
        void flush_fpu ();

        void flush_from_cpu ();

        Ec(const Ec&);
        Ec &operator = (Ec const &);

    public:
        static Ec *current CPULOCAL_HOT;
        static Ec *fpowner CPULOCAL;
        static Ec *ec_idle CPULOCAL;

        Ec (Pd *, void (*)(), unsigned);
        Ec (Pd *, mword, Pd *, void (*)(), unsigned, unsigned, mword, mword, Pt *);
        Ec (Pd *, Pd *, void (*f)(), unsigned, Ec *);
        Ec (Pd *, Pd *, void (*f)(), unsigned, Ec *, Pt *);

        ~Ec();

        ALWAYS_INLINE
        inline void add_tsc_offset (uint64 const t)
        {
            regs.add_tsc_offset (t);
        }

        ALWAYS_INLINE
        inline bool blocked() const { return next || !cont; }

        ALWAYS_INLINE
        inline void set_timeout (uint64 t, Sm *s)
        {
            if (EXPECT_FALSE (t))
                timeout.enqueue (t, s);
        }

        ALWAYS_INLINE
        inline void clr_timeout()
        {
            if (EXPECT_FALSE (timeout.active()))
                timeout.dequeue();
        }

        ALWAYS_INLINE
        inline void set_si_regs(mword sig, mword cnt)
        {
            regs.ARG_2 = sig;
            regs.ARG_3 = cnt;
        }

        ALWAYS_INLINE NORETURN
        inline void make_current()
        {
            if (EXPECT_FALSE (current->del_rcu()))
                Rcu::call (current);

            claim_fpu();

            check_hazard_tsc_aux();

            uint64 const t = rdtsc();

            current->time += t - current->tsc;

            current = this;

            current->tsc = t;

            bool ok = current->add_ref();
            assert (ok);

            Tss::run.sp0 = reinterpret_cast<mword>(exc_regs() + 1);

            pd->make_current();

            asm volatile ("mov %0," EXPAND (PREG(sp);) "jmp *%1" : : "g" (CPU_LOCAL_STCK + PAGE_SIZE), "q" (cont) : "memory"); UNREACHED;
        }

        ALWAYS_INLINE
        inline void check_hazard_tsc_aux()
        {
            if (!Cpu::feature (Cpu::Feature::RDTSCP))
                return;

            bool const current_is_vm = current->vcpu();
            bool const next_is_vm    = this->vcpu();

            if (!current_is_vm && !next_is_vm)
                return;

            if (current_is_vm && !next_is_vm) {
                if (current->regs.tsc_aux != Cpu::id)
                    this->regs.set_hazard (HZD_TSC_AUX);
                return;
            }

            if (!current_is_vm && next_is_vm) {
                if (Cpu::id != this->regs.tsc_aux)
                    this->regs.set_hazard (HZD_TSC_AUX);
                return;
            }

            if (current->regs.tsc_aux != this->regs.tsc_aux)
                this->regs.set_hazard (HZD_TSC_AUX);
        }

        ALWAYS_INLINE
        static inline Ec *remote (unsigned c)
        {
            return *reinterpret_cast<volatile typeof current *>(reinterpret_cast<mword>(&current) - CPU_LOCAL_DATA + HV_GLOBAL_CPUS + c * PAGE_SIZE);
        }

        NOINLINE
        void help (void (*c)())
        {
            if (EXPECT_FALSE (cont == dead))
                return;

            current->cont = c;

            /* permit re-scheduling in case of long chain or livelock loop */
            Cpu::preemption_point();
            if (EXPECT_FALSE (Cpu::hazard & HZD_SCHED))
                Sc::schedule (false);

            Counter::print<1,16> (++Counter::helping, Console_vga::COLOR_LIGHT_WHITE, SPN_HLP);

            /* debug helper */
            if (EXPECT_FALSE ((++Sc::ctr_loop % HELPING_LOOP_TOO_LONG_CHECK) == 0)) {
                auto now   = Lapic::time();
                auto after = Lapic::ms_to_tsc(HELPING_LOOP_LIMIT_RATE_MESSAGE_MS, Sc::long_loop);

                /* limit rate of messages according to helping loop define */
                if (!Sc::long_loop || now > after) {
                    trace(0, "Sc:%p Ec:%p - long helping chain - %u",
                             Sc::current, Ec::current, Sc::ctr_loop);
                    Sc::long_loop = now;
                }
            }

            activate();
        }

        NOINLINE
        void block_sc()
        {
            {   Lock_guard <Spinlock> guard (lock);

                if (!blocked())
                    return;

                bool ok = Sc::current->add_ref();
                assert (ok);

                enqueue (Sc::current);
            }

            Sc::schedule (true);
        }

        ALWAYS_INLINE
        inline void release (void (*c)())
        {
            if (c)
                cont = c;

            Lock_guard <Spinlock> guard (lock);

            for (Sc *s; dequeue (s = head()); ) {
                if (EXPECT_TRUE(!s->last_ref()) || s->ec->partner) {
                    s->remote_enqueue(false);
                    continue;
                }

                Rcu::call(s);
            }
        }

        HOT NORETURN
        static void ret_user_sysexit();

        HOT NORETURN
        static void ret_user_iret() asm ("ret_user_iret");

        HOT
        static void chk_kern_preempt() asm ("chk_kern_preempt");

        NORETURN
        static void ret_user_vmresume();

        NORETURN
        static void ret_user_vmrun();

        NORETURN
        static void ret_xcpu_reply();

        template <void (*)()>
        NORETURN
        static void ret_xcpu_reply_oom();

        template <Sys_regs::Status S, bool T = false>

        NOINLINE NORETURN
        static void sys_finish();

        NORETURN
        void activate();

        template <void (*)()>
        NORETURN
        static void send_msg();

        HOT NORETURN
        static void recv_kern();

        HOT NORETURN
        static void recv_user();

        HOT NORETURN
        static void reply (void (*)() = nullptr, Sm * = nullptr);

        HOT NORETURN
        static void sys_call();

        HOT NORETURN
        static void sys_reply();

        NORETURN
        static void sys_create_pd();

        NORETURN
        static void sys_create_ec();

        NORETURN
        static void sys_create_sc();

        NORETURN
        static void sys_create_pt();

        NORETURN
        static void sys_create_sm();

        NORETURN
        static void sys_revoke();

        NORETURN
        static void sys_misc();

        NORETURN
        static void sys_ec_ctrl();

        NORETURN
        static void sys_sc_ctrl();

        NORETURN
        static void sys_pt_ctrl();

        NORETURN
        static void sys_sm_ctrl();

        NORETURN
        static void sys_pd_ctrl();

        NORETURN
        static void sys_assign_pci();

        NORETURN
        static void sys_assign_gsi();

        NORETURN
        static void sys_xcpu_call();

        template <void (*)()>
        NORETURN
        static void sys_xcpu_call_oom();

        NORETURN
        static void idle();

        NORETURN
        static void xcpu_return();

        Sm * xcpu_revert();

        void xcpu_clone(Ec &, uint16);

        template <void (*)()>
        NORETURN
        static void oom_xcpu_return();

        NORETURN
        static void root_invoke();

        template <bool>
        static void delegate();

        NORETURN
        static void dead() { die ("IPC Abort"); }

        NORETURN
        static void die (char const *, Exc_regs * = &current->regs);

        static void idl_handler();

        static void hlt_prepare();

        NORETURN
        static void hlt_handler();

        ALWAYS_INLINE
        static inline void *operator new (size_t, Pd &pd) { return pd.ec_cache.alloc(pd.quota); }

        template <void (*)()>
        NORETURN
        void oom_xcpu(Pt *, mword, mword);

        NORETURN
        void oom_delegate(Ec *, Ec *, Ec *, bool, bool);

        NORETURN
        void oom_call(Pt *, mword, mword, void (*)(), void (*)());

        NORETURN
        void oom_call_cpu(Pt *, mword, void (*)(), void (*)());

        template <void(*C)()>
        static void check(mword, bool = true);

        bool migrate(Capability &, Ec *, Sys_ec_ctrl const &);

        ALWAYS_INLINE
        void inline measured() { time_m = time; }

        ALWAYS_INLINE
        inline bool vcpu()
        {
            return !utcb && (regs.vtlb || regs.vmcb_state || regs.vmcs_state);
        }
};
