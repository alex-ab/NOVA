/*
 * Execution Context (SVM)
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

#include "ec.hpp"
#include "svm.hpp"
#include "vtlb.hpp"

uint8 Ec::ifetch (mword virt)
{
    mword phys, attr = 0, type = 0;
    uint8 opcode;

    if (!Vtlb::gwalk (&current->regs, virt, phys, attr, type))
        die ("SVM TLB failure");

    if (User::peek (reinterpret_cast<uint8 *>(phys), opcode) != ~0UL)
        die ("SVM ifetch failure");

    return opcode;
}

void Ec::svm_exception (mword reason)
{
    Vmcb &vmcb = current->regs.vmcb_state->vmcb;

    if (vmcb.exitintinfo & 0x80000000) {

        mword t = static_cast<mword>(vmcb.exitintinfo) >> 8 & 0x7;
        mword v = static_cast<mword>(vmcb.exitintinfo) & 0xff;

        if (t == 0 || (t == 3 && v != 3 && v != 4))
            vmcb.inj_control = vmcb.exitintinfo;
    }

    switch (reason) {

        default:
            current->regs.dst_portal = reason;
            break;

        case 0x47:          // #NM
            handle_exc_nm();
            ret_user_vmrun();

        case 0x4e:          // #PF
            if (current->regs.nst_on) {
                current->regs.dst_portal = reason;
                break;
            }

            mword err = static_cast<mword>(vmcb.exitinfo1);
            mword cr2 = static_cast<mword>(vmcb.exitinfo2);

            switch (Vtlb::miss (&current->regs, cr2, err)) {

                case Vtlb::GPA_HPA:
                    current->regs.nst_error = 0;
                    current->regs.dst_portal = VM_EXIT_NPT;
                    break;

                case Vtlb::GLA_GPA:
                    vmcb.cr2 = cr2;
                    vmcb.inj_control = static_cast<uint64>(err) << 32 | 0x80000b0e;

                    [[fallthrough]];

                case Vtlb::SUCCESS:
                    ret_user_vmrun();
            }
    }

    send_msg<ret_user_vmrun>();
}

void Ec::svm_invlpg()
{
    Vmcb &vmcb = current->regs.vmcb_state->vmcb;

    current->regs.svm_update_shadows();

    mword virt = current->regs.linear_address<Vmcb>(static_cast<mword>(vmcb.cs.base) + static_cast<mword>(vmcb.rip));

    assert (ifetch (virt) == 0xf && ifetch (virt + 1) == 0x1);

    uint8 mrm = ifetch (virt + 2);
    uint8 r_m = mrm & 7;

    unsigned len = 3;

    switch (mrm >> 6) {
        case 0: len += (r_m == 4 ? 1 : r_m == 5 ? 4 : 0); break;
        case 1: len += (r_m == 4 ? 2 : 1); break;
        case 2: len += (r_m == 4 ? 5 : 4); break;
    }

    current->regs.tlb_flush<Vmcb>(true);
    vmcb.adjust_rip (len);
    ret_user_vmrun();
}

void Ec::svm_cr(mword const reason)
{
    Vmcb &vmcb = current->regs.vmcb_state->vmcb;

    current->regs.svm_update_shadows();

    mword virt = current->regs.linear_address<Vmcb>(static_cast<mword>(vmcb.cs.base) + static_cast<mword>(vmcb.rip));

    assert (ifetch (virt) == 0xf);

    uint8 opc = ifetch (virt + 1);
    uint8 mrm = ifetch (virt + 2);

    unsigned len, gpr = mrm & 0x7, cr = mrm >> 3 & 0x7;

    switch (opc) {

        case 0x6:       // CLTS
            current->regs.write_cr<Vmcb> (0, current->regs.read_cr<Vmcb> (0) & ~Cpu::CR0_TS);
            len = 2;
            break;

        case 0x20:      // MOV from CR
            current->regs.svm_write_gpr (gpr, current->regs.read_cr<Vmcb>(cr));
            len = 3;
            break;

        case 0x22:      // MOV to CR
            current->regs.write_cr<Vmcb> (cr, current->regs.svm_read_gpr (gpr));
            len = 3;
            break;

        case 0x1:
        {
            bool const op_ext = (mrm >> 6) == 0x3;
            if (op_ext && (cr == 4)) { // SMSW
                current->regs.dst_portal = reason;
                send_msg<ret_user_vmrun>();
            }
            [[fallthrough]];
        }
        default:
            die ("SVM decode failure");
    }

    vmcb.adjust_rip (len);
    ret_user_vmrun();
}

void Ec::handle_svm()
{
    Fpu::State_xsv::make_current (current->regs.gst_xsv, Fpu::hst_xsv);    // Restore XSV host state

    Vmcb &vmcb = current->regs.vmcb_state->vmcb;

    vmcb.tlb_control = 0;

    mword reason = VM_EXIT_NOSUPP;

    switch (vmcb.exitcode) {
        case -1U:               // Invalid state
        case -1ULL:             // Invalid state
            reason = VM_EXIT_INVSTATE;
            break;
        case 0x400:             // NPT
            reason = VM_EXIT_NPT;
            current->regs.nst_error = static_cast<mword>(vmcb.exitinfo1);
            current->regs.nst_fault = static_cast<mword>(vmcb.exitinfo2);
            break;
        default:
            reason = static_cast<mword>(vmcb.exitcode);
            break;
    }

    /* all unsupported exits are remapped to a specific exit */
    if (reason >= NUM_VMI) {
        trace (TRACE_VIRT, "svm: unsupported exit reason=%lx\n", reason);
        reason = VM_EXIT_NOSUPP;
    }

    /* sanity check, the array has solely NUM_VMI elements */
    if (reason < NUM_VMI)
        Counter::vmi[reason]++;

    switch (reason) {

        case 0x0 ... 0x1f:      // CR Access
            if (!current->regs.nst_on) svm_cr (reason);
            else break;

        case 0x40 ... 0x5f:     // Exception
            svm_exception (reason);
            break;

        case 0x60:              // EXTINT
            asm volatile ("sti; nop; cli" : : : "memory");
            /* Set a default value in case there is an asynchronous recall. */
            current->regs.dst_portal = VM_EXIT_RECALL;
            ret_user_vmrun();

        case 0x79:              // INVLPG
            if (!current->regs.nst_on) svm_invlpg();
            else break;
    }

    current->regs.dst_portal = reason;

    send_msg<ret_user_vmrun>();
}
