/*
 * User Thread Control Block (UTCB)
 *
 * Copyright (C) 2009-2011 Udo Steinberg <udo@hypervisor.org>
 * Economic rights: Technische Universitaet Dresden (Germany)
 *
 * Copyright (C) 2012 Udo Steinberg, Intel Corporation.
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

#include "buddy.hpp"
#include "crd.hpp"
#include "util.hpp"

class Cpu_regs;

class Utcb_segment
{
    public:
        uint16  sel, ar;
        uint32  limit;
        uint64  base;

        ALWAYS_INLINE
        inline void set_vmx (mword s, mword b, mword l, mword a)
        {
            sel   = static_cast<uint16>(s);
            ar    = static_cast<uint16>((a >> 4 & 0x1f00) | (a & 0xff));
            limit = static_cast<uint32>(l);
            base  = b;
        }
};

class Utcb_head
{
    protected:
        mword items {0};

    public:
        Crd     xlt {}, del {};
        mword   tls {0};
};

class Utcb_data
{
    protected:
        union {
            struct {
                mword           mtd, inst_len, rip, rflags;
                uint32          intr_state, actv_state;
                union {
                    struct {
                        uint32  intr_info, intr_error;
                    };
                    uint64      inj;
                };

                mword           rax, rcx, rdx, rbx, rsp, rbp, rsi, rdi;
#ifdef __x86_64__
                mword           r8,  r9,  r10, r11, r12, r13, r14, r15;
#endif
                uint64          qual[2];
                uint32          ctrl[2];
                uint64          reserved;
                mword           cr0, cr2, cr3, cr4;
                mword           pdpte[4];
#ifdef __x86_64__
                mword           cr8, efer;
                uint64          star;
                uint64          lstar;
                uint64          cstar;
                uint64          sfmask;
                uint64          kernel_gs_base;
                uint32          tpr;
                uint32          tpr_threshold;
#endif
                mword           dr7, sysenter_cs, sysenter_rsp, sysenter_rip;
                Utcb_segment    es, cs, ss, ds, fs, gs, ld, tr, gd, id;
                uint64          tsc_val, tsc_off;
            };

            mword mr[(PAGE_SIZE - sizeof (Utcb_head)) / sizeof(mword)];
        };
};

class Utcb : public Utcb_head, private Utcb_data
{
    private:
        static mword const words = (PAGE_SIZE - sizeof (Utcb_head)) / sizeof (mword);

    public:
        WARN_UNUSED_RESULT bool load_exc (Cpu_regs *);
        WARN_UNUSED_RESULT bool load_vmx (Cpu_regs *);
        WARN_UNUSED_RESULT bool load_svm (Cpu_regs *);
        WARN_UNUSED_RESULT bool save_exc (Cpu_regs *);
        WARN_UNUSED_RESULT bool save_vmx (Cpu_regs *);
        WARN_UNUSED_RESULT bool save_svm (Cpu_regs *);

        inline mword ucnt() const { return static_cast<uint16>(items); }
        inline mword tcnt() const { return static_cast<uint16>(items >> 16); }

        inline mword ui() const { return min (words / 1, ucnt()); }
        inline mword ti() const { return min (words / 2, tcnt()); }

        ALWAYS_INLINE NONNULL
        inline void save (Utcb *dst)
        {
            mword n = ui();

            dst->items = items;
#if 0
            mword *d = dst->mr, *s = mr;
            asm volatile ("rep; movsl" : "+D" (d), "+S" (s), "+c" (n) : : "memory");
#else
            for (unsigned long i = 0; i < n; i++)
                dst->mr[i] = mr[i];
#endif
        }

        ALWAYS_INLINE
        inline Xfer *xfer() { return reinterpret_cast<Xfer *>(this) + PAGE_SIZE / sizeof (Xfer) - 1; }

        ALWAYS_INLINE
        static inline void *operator new (size_t, Quota &quota) { return Buddy::allocator.alloc (0, quota, Buddy::FILL_0); }

        ALWAYS_INLINE
        static inline void destroy(Utcb *obj, Quota &quota) { obj->~Utcb(); Buddy::allocator.free (reinterpret_cast<mword>(obj), quota); }

        template <typename R, typename W>
        void for_each_word (R const &fn_read, W const &fn_write)
        {
            mword const max = min(ui(), sizeof(mword)*8 - 1);
            mword success = 0;
            for (unsigned i = 0; i < max; i++) {
                bool const write = !!(mr[i] & (1ul << 31));
                if (write) {
                    if (i + 1 >= max) break;

                    if (fn_write(mr[i] & ~(1ul << 31), mr[i+1]))
                        success |= 3ul << i;

                    i += 1;
                } else
                    if (fn_read(mr[i]))
                        success |= 1ul << i;
            }
            items = success;
        }
};

static_assert (sizeof(Utcb) == 4096, "Unsupported size of Utcb");
