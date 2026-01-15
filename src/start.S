/*
 * Startup Code
 *
 * Copyright (C) 2009-2011 Udo Steinberg <udo@hypervisor.org>
 * Economic rights: Technische Universitaet Dresden (Germany)
 *
 * Copyright (C) 2012-2013 Udo Steinberg, Intel Corporation.
 * Copyright (C) 2019-2025 Udo Steinberg, BlueRock Security, Inc.
 *
 * Copyright (C) 2017-2026 Alexander Boettcher, Genode Labs GmbH
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

#include "arch.hpp"
#include "memattr.hpp"
#include "memory.hpp"
#include "multiboot.hpp"
#include "patch.hpp"
#include "selectors.hpp"

#define PTE_ATTR_P      BIT (0)     // Present
#define PTE_ATTR_W      BIT (1)     // Writable
#define PTE_ATTR_U      BIT (2)     // User
#define PTE_ATTR_A      BIT (5)     // Accessed
#define PTE_ATTR_D      BIT (6)     // Dirty
#define PTE_ATTR_S      BIT (7)     // Superpage
#define PTE_ATTR_G      BIT (8)     // Global

.global                 __init_bsp, __init_aps, __init_aps__, __wake_vec, __init_ilp

/*
 * Macros
 */
.macro                  PTE_FIX L, PTAB, VIRT, PHYS, ATTR
                        lea     (\PHYS) + (\ATTR)(%ebp), %ecx
                        mov     %ecx, (\PTAB) + __SIZEOF_POINTER__ * ((\VIRT) >> LEVL_BITS (\L) & (BIT (PTE_BPL) - 1))(%ebp)
.endm

.macro                  PTE_REL L, PTAB, VIRT, PHYS, ATTR
                        lea     (\PHYS) + (\ATTR)(%ebp), %ecx
                        lea     (\VIRT)(%ebp), %edx
                        shr     $LEVL_BITS (\L), %edx
                        and     $(BIT (PTE_BPL) - 1), %edx
                        mov     %ecx, (\PTAB)(%ebp, %edx, __SIZEOF_POINTER__)
.endm

.macro                  INIT_PAGING
#ifdef __x86_64__
#ifdef CR_PAT_ENABLE
                        mov     $0x277, %ecx
                        mov     $(PAT_VAL >> 32), %edx
                        mov     $(PAT_VAL & BIT_RANGE (31, 0)), %eax
                        wrmsr
#endif
                        mov     $0xc0000080, %ecx
                        xor     %edx, %edx
                        mov     $(EFER_NXE | EFER_LME | EFER_SCE), %eax
                        wrmsr

                        lea     STACK(%ebp), %esp
                        lea     PT3S_HPAS(%ebp), %ecx
                        mov     $(CR4_OSXMMEXCPT | CR4_OSFXSR | CR4_PGE | CR4_PAE | CR4_DE), %edx
                        mov     $(CR0_PG | CR0_WP | CR0_NE | CR0_TS | CR0_MP | CR0_PE), %eax
                        mov     %edx, %cr4
                        mov     %ecx, %cr3
                        mov     %eax, %cr0
#else
                        mov     $0x698, %edx
                        mov     %edx, %cr4
                        mov     $PDBR, %edx
                        mov     %edx, %cr3
                        mov     $0x8001002b, %edx
                        mov     %edx, %cr0
#endif
.endm

.macro                  RECTIME SYMBOL
                        rdtsc
                        mov     %eax, 0 + \SYMBOL - OFFSET(%ebp)
                        mov     %edx, 4 + \SYMBOL - OFFSET(%ebp)
.endm

.macro                  REL_SYM SYMBOL
                        add     %ebp, \SYMBOL(%ebp)
.endm

/*
 * Initialization Code
 */
.section .init

.macro                  INIT_STATE
#ifdef __i386__
                        mov     $SEL_KERN_DATA, %ecx
                        mov     %ecx, %ss
                        mov     $SEL_USER_DATA, %ecx
                        mov     %ecx, %ds
                        mov     %ecx, %es
                        mov     %ecx, %fs
                        mov     %ecx, %gs
                        mov     $STACK, %esp
#else
                        mov     $SEL_KERN_DATA, %ecx
                        mov     %cx, %ss
                        xor     %ecx, %ecx
                        mov     %cx, %ds
                        mov     %cx, %es
                        mov     %cx, %fs
                        mov     %cx, %gs
#endif
.endm

/*
 * 32/64-bit Startup Code: Bootstrap Processor (BSP)
 */
.code32

__init_bsp:             // Check if we launched via Multiboot v2
                        cmp     $MULTIBOOT_V2_LOADER, %eax
                        je      .Lvalid_launch

                        // Check if we launched via Multiboot v1
                        cmp     $MULTIBOOT_V1_LOADER, %eax
                        je      .Lvalid_launch

                        hlt
                        jmp     .
.Lvalid_launch:
                        // Determine relocation offset
                        xchg    %ebx, %esp
                        pop     %esi
                        call    .Lret_addr
.Lret_addr:             pop     %ebp
                        push    %esi
            dec %esi;   sub     $.Lret_addr, %ebp
                        xchg    %ebx, %esp

                        // Store boot parameters
                        mov     %eax, multiboot_p0 - OFFSET(%ebp)
                        mov     %ebx, multiboot_p1 - OFFSET(%ebp)

                        RECTIME multiboot_t0

                        // Store image end address
                        lea     NOVA_HPAE(%ebp), %eax
                        mov     %eax, multiboot_ea - OFFSET(%ebp)

#ifdef __i386__
                        mov     $0xe3, %ecx
                        mov     $LVL2L, %edi
1:                      mov     %ecx, (%edi)
                        add     $(1 << (PTE_BPL + PAGE_BITS)), %ecx
                        add     $SIZE, %edi
                        cmp     $LOAD_E, %ecx
                        jb      1b

                        mov     $(LINK_P + 0x1e3), %ecx
                        mov     $(LVL2H + SIZE * (LINK_ADDR >> (PTE_BPL + PAGE_BITS) & ((1 << PTE_BPL) - 1))), %edi
1:                      mov     %ecx, (%edi)
                        add     $(1 << (PTE_BPL + PAGE_BITS)), %ecx
                        add     $SIZE, %edi
                        cmp     $LINK_E, %ecx
                        jb      1b
#endif

#ifdef __x86_64__
.Lmb2_launch:
                        // Multiboot v2 Launch
                        cmpl    $MULTIBOOT_V2_LOADER, multiboot_p0 - OFFSET(%ebp)
                        jne     .Lmb1_launch

                        // Prepare tag loop
                        lea     0x8(%ebx), %ecx     // Tag Ptr
                        add     0x0(%ebx), %ebx     // Tag End
                        xor     %esi, %esi          // EFI Sys
                        xor     %edi, %edi          // EFI Img
.Lmb2_tag_loop:
                        cmpl    $MULTIBOOT_V2_INFO_END, (%ecx)
                        je      .Lmb_end

                        cmpl    $MULTIBOOT_V2_INFO_CMD, (%ecx)
                        jne     .Lmb2_not_cmd
                        lea     0x8(%ecx), %eax
                        mov     %eax, multiboot_cl - OFFSET(%ebp)
                        jmp     .Lmb2_nxt_tag
.Lmb2_not_cmd:
                        cmpl    $MULTIBOOT_V2_INFO_MOD_32, (%ecx)
                        jne     .Lmb2_not_mod_32
                        mov     multiboot_ra + 0 - OFFSET(%ebp), %edx
                        or      multiboot_ra + 4 - OFFSET(%ebp), %edx
                        jnz     .Lmb2_nxt_tag
                        mov     0x8(%ecx), %eax
                        mov     %eax, multiboot_ra + 0 - OFFSET(%ebp)
                        mov     %edx, multiboot_ra + 4 - OFFSET(%ebp)
                        jmp     .Lmb2_nxt_tag
.Lmb2_not_mod_32:
                        cmpl    $MULTIBOOT_V2_INFO_SYS_64, (%ecx)
                        jne     .Lmb2_not_sys_64
                        mov     0x8(%ecx), %esi
                        jmp     .Lmb2_nxt_tag
.Lmb2_not_sys_64:
                        cmpl    $MULTIBOOT_V2_INFO_RSDP_2, (%ecx)
                        jne     .Lmb2_not_rsdp_2
                        lea     0x8(%ecx), %eax
                        mov     %eax, uefi_info - OFFSET(%ebp)
                        jmp     .Lmb2_nxt_tag
.Lmb2_not_rsdp_2:
                        cmpl    $MULTIBOOT_V2_INFO_IMG_64, (%ecx)
                        jne     .Lmb2_not_img_64
                        mov     0x8(%ecx), %edi
                        jmp     .Lmb2_nxt_tag
.Lmb2_not_img_64:
                        cmpl    $MULTIBOOT_V2_INFO_MOD_64, (%ecx)
                        jne     .Lmb2_not_mod_64
                        mov     multiboot_ra + 0 - OFFSET(%ebp), %edx
                        or      multiboot_ra + 4 - OFFSET(%ebp), %edx
                        jnz     .Lmb2_nxt_tag
                        mov     0x8(%ecx), %eax
                        mov     0xc(%ecx), %edx
                        mov     %eax, multiboot_ra + 0 - OFFSET(%ebp)
                        mov     %edx, multiboot_ra + 4 - OFFSET(%ebp)
                        jmp     .Lmb2_nxt_tag
.Lmb2_not_mod_64:
                        jmp     .Lmb2_nxt_tag
#ifdef kkkk
                        cmpl    $MULTIBOOT_V2_INFO_KMEM, (%ecx)
                        jne     .Lmb2_nxt_tag
                        mov     $(MMAP_GLB_MMIO - LINK_ADDR), %edx
                        sub     $(NOVA_HPAE - LOAD_ADDR), %edx
                        mov     0x8(%ecx), %eax
                        cmp     %edx, %eax
                        cmova   %edx, %eax
                        and     $~OFFS_MASK (1), %eax
                        add     %eax, multiboot_ea - OFFSET(%ebp)
#endif
.Lmb2_nxt_tag:
                        mov     0x4(%ecx), %eax
                        lea     0x7(%ecx,%eax), %ecx
                        and     $~7, %ecx
                        cmp     %ebx, %ecx
                        jb      .Lmb2_tag_loop
#ifdef llll
.Lmb2_end:
                        // UEFI init if applicable (guarantees we have >= 128K stack space)
                        test    %edi, %edi
                        jz      .Lmb_end
                        test    %esi, %esi
                        jz      .Lmb_end
                        lea     uefi_info - OFFSET(%ebp), %edx
                        call    uefi_init
                        jmp     .Lmb_end
#endif
.Lmb1_launch:
                        // Multiboot v1 Launch
                        cmpl    $MULTIBOOT_V1_LOADER, multiboot_p0 - OFFSET(%ebp)
                        jne     .Lmb_end

                        testb   $BIT (MULTIBOOT_V1_INFO_CMD), (%ebx)
                        jz      .Lmb1_not_cmd
                        mov     16(%ebx), %eax
                        mov     %eax, multiboot_cl - OFFSET(%ebp)
.Lmb1_not_cmd:
                        testb   $BIT (MULTIBOOT_V1_INFO_MOD), (%ebx)
                        jz      .Lmb_end
                        mov     20(%ebx), %eax
                        test    %eax, %eax
                        jz      .Lmb_end
                        mov     24(%ebx), %eax
                        mov     (%eax), %eax
                        mov     %eax, multiboot_ra - OFFSET(%ebp)
.Lmb_end:
                        RECTIME multiboot_t1

                        // Ensure flags are in a defined state (after UEFI init)
                        cli
                        cld

                        // Load relocated boot GDT
                        REL_SYM (__relo_pdb - OFFSET)
                        lgdt    (__relo_pdl - OFFSET)(%ebp)

                        // Configure MLE Page Tables (4K pages)
                        PTE_FIX 2, MLE_L2, 0, MLE_L1, PTE_ATTR_P
                        PTE_FIX 1, MLE_L1, 0, MLE_L0, PTE_ATTR_P

                        lea     MLE_L0(%ebp), %edx
                        lea     HASH_HPAS + PTE_ATTR_P(%ebp), %ecx
                        lea     HASH_HPAE(%ebp), %eax
.Lmle_loop:             mov     %ecx, (%edx)
                        add     $__SIZEOF_POINTER__, %edx
                        add     $PAGE_SIZE (0), %ecx
                        cmp     %eax, %ecx
                        jb      .Lmle_loop
.Lilp:
                        // Configure Page Tables: Load Section (2M page)
                        PTE_REL 3, PT3S_HPAS, NOVA_HPAS, PT2L_HPAS, PTE_ATTR_A | PTE_ATTR_U | PTE_ATTR_W | PTE_ATTR_P
                        PTE_REL 2, PT2L_HPAS, NOVA_HPAS, PT1L_HPAS, PTE_ATTR_A | PTE_ATTR_U | PTE_ATTR_W | PTE_ATTR_P
                        PTE_REL 1, PT1L_HPAS, NOVA_HPAS, NOVA_HPAS, PTE_ATTR_S | PTE_ATTR_D | PTE_ATTR_A | PTE_ATTR_W | PTE_ATTR_P

                        PTE_REL 1, PT1L_HPAS, AP_BOOT_PADDR, AP_BOOT_PADDR, PTE_ATTR_S | PTE_ATTR_D | PTE_ATTR_A | PTE_ATTR_W | PTE_ATTR_P

                        // Configure Page Tables: Link Section (2M pages)
                        PTE_FIX 3, PT3S_HPAS, LINK_ADDR, PT2H_HPAS, PTE_ATTR_A | PTE_ATTR_U | PTE_ATTR_W | PTE_ATTR_P
                        PTE_FIX 2, PT2H_HPAS, LINK_ADDR, PT1H_HPAS, PTE_ATTR_A | PTE_ATTR_U | PTE_ATTR_W | PTE_ATTR_P

                        lea     PT1H_HPAS + __SIZEOF_POINTER__ * (LINK_ADDR >> LEVL_BITS (1) & (BIT (PTE_BPL) - 1))(%ebp), %edx
                        lea     NOVA_HPAS + (PTE_ATTR_G | PTE_ATTR_S | PTE_ATTR_D | PTE_ATTR_A | PTE_ATTR_W | PTE_ATTR_P)(%ebp), %ecx
                        mov     multiboot_ea - OFFSET(%ebp), %eax
.Lpte_loop:             mov     %ecx, (%edx)
                        add     $__SIZEOF_POINTER__, %edx
                        add     $PAGE_SIZE (1), %ecx
                        cmp     %eax, %ecx
                        jb      .Lpte_loop
#endif

                        RECTIME multiboot_t2

                        // Initialize jump target
                        lea     __init_all(%ebp), %ebx
                        mov     %ebx,           __relo_jip - OFFSET(%ebp)
                        movw    $SEL_KERN_CODE, __relo_jcs - OFFSET(%ebp)
.Lrlp:
                        INIT_PAGING
                        ljmp   *__relo_jip - OFFSET(%ebp)

/*
 * ILP Re-Entry
 */
__init_ilp:             // Determine load skew
                        mov     %ebx, %ebp
                        sub     $__init_ilp, %ebp

                        // Crash if load skew is misaligned
                        test    $ALIGNMENT_OFFS (ALIGNMENT_NOVA), %ebp
                        jnz     .Lcrash

                        // Crash if pseudo descriptor limit is wrong
                        mov     $.Lboot_gdt - __boot_gdt - 1, %eax
                        cmp     %ax, %cs:__relo_pdl - OFFSET(%ebp)
                        jne     .Lcrash

                        // Crash if pseudo descriptor base is wrong
                        lea     __boot_gdt(%ebp), %ebx
                        cmp     %ebx, %cs:__relo_pdb - OFFSET(%ebp)
                        jne     .Lcrash

                        // Load measured boot GDT
                        lgdt    %cs:__relo_pdl - OFFSET(%ebp)

                        // Establish valid 32-bit segments for SS/DS/ES
                        mov     $SEL_KERN_DATA + 16, %ecx
                        mov     %cx, %ss
                        mov     %cx, %ds
                        mov     %cx, %es

                        // Initialize MLE join structure
                        sub     $8, %ecx
                        lea     __init_rlp(%ebp), %edx
                        lea     mle_join - OFFSET(%ebp), %edi
                        mov     %eax,  0(%edi)                      // GDT Limit
                        mov     %ebx,  4(%edi)                      // GDT Base
                        mov     %ecx,  8(%edi)                      // SEL
                        mov     %edx, 12(%edi)                      // EIP

                        // Traverse TXT heap
                        mov     0xfed30300, %esi                    // Data_efi_pre
                        add     (%esi), %esi                        // Data_pre_mle
                        add     (%esi), %esi                        // Data_pre_acm

                        // Crash if Data_pre_acm does not point to MLE_L2
                        lea     MLE_L2(%ebp), %eax
                        cmp     %eax, 16(%esi)
                        jne     .Lcrash

                        // Crash if MLE_L2 does not point to MLE_L1 + attributes
                        lea     MLE_L1 + PTE_ATTR_P(%ebp), %eax
                        cmp     %eax, MLE_L2(%ebp)
                        jne     .Lcrash

                        // Crash if MLE_L1 does not point to MLE_L0 + attributes
                        lea     MLE_L0 + PTE_ATTR_P(%ebp), %eax
                        cmp     %eax, MLE_L1(%ebp)
                        jne     .Lcrash

                        // Crash if MLE first page is not virt/phys contiguous with re-entry point
                        lea     HASH_HPAS + PTE_ATTR_P(%ebp), %eax
                        cmp     %eax, MLE_L0(%ebp)
                        jne     .Lcrash

                        // Crash if MLE last page is not virt/phys contiguous with re-entry point
                        lea     HASH_HPAL + PTE_ATTR_P(%ebp), %eax
                        cmp     %eax, MLE_L0_LS(%ebp)
                        jne     .Lcrash

                        // Determine image range
                        lea     NOVA_HPAS(%ebp), %eax               // NOVA.S
                        lea     NOVA_HPAE(%ebp), %ecx               // NOVA.E (original)
                        mov     multiboot_ea - OFFSET(%ebp), %ebx   // NOVA.E (extended)

                        // Crash if NOVA.E is wrong (extended < original)
                        cmp     %ecx, %ebx
                        jb      .Lcrash

                        // Crash if NOVA.E is misaligned
                        test    $OFFS_MASK (1), %ebx
                        jnz     .Lcrash

                        // Crash if PMR does not cover NOVA.S...NOVA.E
                        mov     %ebx, %ecx
                        sub     %eax, %ecx
                        cmp     %eax, 40(%esi)                      // PMR Lo Base
                        jne     .Lcrash
                        cmp     %ecx, 48(%esi)                      // PMR Lo Size
                        jne     .Lcrash

                        // Traverse TXT heap
                        add     (%esi), %esi                        // Data_acm_mle

                        // Crash if Data_acm_mle version < 5
                        cmpl    $5, 8(%esi)
                        jb      .Lcrash

                        // Traverse MDRs
                        mov     %esi, %edi
                        mov     136(%esi), %ecx                     // MDR count
                        add     140(%esi), %edi                     // MDR pointer
.Lmdr_loop:
                        // Check if NOVA.S (ESP:EAX) >= MDR.S (ESI:EDX)
                        mov     0(%edi), %edx
                        mov     4(%edi), %esi
                        xor     %esp, %esp
                        cmp     %edx, %eax
                        sbb     %esi, %esp
                        jc      .Lmdr_next                          // NOVA.S < MDR.S

                        // Check if NOVA.E (ESP:EBX) <= MDR.E (ESI:EDX)
                        add      8(%edi), %edx
                        adc     12(%edi), %esi
                        xor     %esp, %esp
                        cmp     %ebx, %edx
                        sbb     %esp, %esi
                        jc      .Lmdr_next                          // NOVA.E > MDR.E

                        // Crash if MDR covering NOVA is != type 0
                        cmpb    $0, 16(%edi)
                        jne     .Lcrash

                        // All post-launch checks succeeded
                        orb     $1, launched - OFFSET(%ebp)
                        jmp     .Lilp
.Lmdr_next:
                        lea     24(%edi), %edi
                        loop    .Lmdr_loop
.Lcrash:
                        ud2

/*
 * RLP Re-Entry
 */
__init_rlp:             // Determine load skew
                        mov     0xfed30290, %ebp
                        sub     $mle_join - OFFSET, %ebp

                        // Mark as RLP
                        xor     %ebx, %ebx

                        jmp     .Lrlp

/*
 * 16-bit Startup Code: Application Processors (APs)
 */
.code16

__init_aps:             jmp     .Lskip_vec
__wake_vec:             orb     $1, %bl
.Lskip_vec:             mov     %cs:.Lrelo_pdb - __init_aps, %ebp
                        sub     $__boot_gdt, %ebp
                        INIT_PAGING
                        lgdtl   %cs:.Lrelo_pdl - __init_aps
                        ljmpl  *%cs:.Lrelo_jip - __init_aps

.Lrelo_jip:             .long   __init_all                      // Jump IP
.Lrelo_jcs:             .word   SEL_KERN_CODE                   // Jump CS
.Lrelo_pdl:             .word   .Lboot_gdt - __boot_gdt - 1     // Pseudo Descriptor Limit
.Lrelo_pdb:             .long   __boot_gdt                      // Pseudo Descriptor Base

__init_aps__:

/*
 * Global Descriptor Table (GDT)
 */
                        .balign 8
__boot_gdt:             .quad   0                               // SEL_NULL
#ifdef __x86_64__
                        .quad   0x00a09b0000000000              // SEL_KERN_CODE (64-bit)
                        .quad   0x00a0930000000000              // SEL_KERN_DATA (64-bit)
                        .quad   0x00cf9b000000ffff              // SEL_KERN_CODE (32-bit)
                        .quad   0x00cf93000000ffff              // SEL_KERN_DATA (32-bit)
#else
                        .quad   0x00cf9b000000ffff
                        .quad   0x00cf93000000ffff
                        .quad   0x00cffb000000ffff
                        .quad   0x00cff3000000ffff
#endif
.Lboot_gdt:


/*
 * Common Startup Code
 */
#ifdef __x86_64__
.code64
#else
.code32
#endif

__init_all:             mov     $.Lhigh, %rax
                        jmp     *%rax

//.text

.Lhigh:                 INIT_STATE

                        // Preinit (BSP only)
                        test    %ebx, %ebx
                        jz      __start_ap_high

/* 32/64 bit entry for boot CPU - BSP */
_start_bsp_high:
                        // Initialize kmem offset, based on 32-bit load skew
                        lea     NOVA_HPAS(%ebp), %REG(ax)
                        lea     LINK_ADDR, %REG(di)
                        sub     %REG(ax), %REG(di)
                        mov     %REG(di), __kmem_offset

                        call    preinit

                        // Restore TXT state
                        call    txt_restore

                        /* move ax/bx according to REGPARM(2) calling convention */
#ifdef __x86_64__
                        mov     multiboot_p0 - OFFSET(%ebp), %REG(di)
                        mov     multiboot_p1 - OFFSET(%ebp), %REG(si)
#else
                        mov     multiboot_p0 - OFFSET(%ebp), %REG(ax)
                        mov     multiboot_p1 - OFFSET(%ebp), %REG(dx)
#endif
                        // Apply these relocations post-launch
                        REL_SYM .Lrelo_jip
                        REL_SYM .Lrelo_pdb

                        call    init
                        jmp     __start_all


/* 32/64 bit AP code entry and entry on resume for *all* CPUs */
__start_ap_high:        INIT_STATE
                        mov     $0, %REG(bx) /* make explicit to avoid confusion */
                        jmp 2f
1:                      pause
                        /* swap bx (==0) with boot_lock (0 or 1) */
2:                      xchg    %REG(bx), boot_lock
                        test    %REG(bx), %REG(bx)
                        /* if !bx then the $STACK can be used with kern_ptab_setup */
                        je      1b

                        // Restore TXT state
                        call    txt_restore

__start_all:
3:                      call    kern_ptab_setup
                        jmp     4f

.text

4:                      mov     %REG(ax), %cr3
                        /* switch to stack backed physically different per CPU */
                        mov     $(CPU_LOCAL_STCK + PAGE_SIZE (0)), %REG(sp)
                        jmp     bootstrap

.data

                        .balign 8
__relo_jip:             .long   0                               // Jump IP
__relo_jcs:             .word   0                               // Jump CS
__relo_pdl:             .word   .Lboot_gdt - __boot_gdt - 1     // Pseudo Descriptor Limit
#ifdef __x86_64__
__relo_pdb:             .quad   __boot_gdt                      // Pseudo Descriptor Base
#else
__relo_pdb:             .long   __boot_gdt                      // Pseudo Descriptor Base
#endif
