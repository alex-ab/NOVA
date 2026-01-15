/*
 * Linker Script
 *
 * Copyright (C) 2009-2011 Udo Steinberg <udo@hypervisor.org>
 * Economic rights: Technische Universitaet Dresden (Germany)
 *
 * Copyright (C) 2012-2013 Udo Steinberg, Intel Corporation.
 * Copyright (C) 2019-2025 Udo Steinberg, BlueRock Security, Inc.
 * Copyright (C) 2013-2026 Alexander Boettcher
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
#include "memory.hpp"

#ifdef __i386__
OUTPUT_FORMAT("elf32-i386")
OUTPUT_ARCH("i386")
#else
OUTPUT_FORMAT("elf64-x86-64")
OUTPUT_ARCH("i386:x86-64")
#endif
ENTRY(__init_bsp)

PHDRS
{
    ptab PT_LOAD FLAGS (6);
    init PT_LOAD FLAGS (6);
    kern PT_LOAD FLAGS (6);
}

SECTIONS
{
    . = LOAD_ADDR;

    PROVIDE_HIDDEN (NOVA_HPAS = .);

#if defined(__x86_64__)
    .ptab :
    {
        PROVIDE_HIDDEN (MLE_TL = .); . += 4K;
        PROVIDE_HIDDEN (MLE_L2 = .); . += 4K;
        PROVIDE_HIDDEN (MLE_L1 = .); . += 4K;
        PROVIDE_HIDDEN (MLE_L0 = .); . += 4K;
    } : ptab
#endif

    PROVIDE_HIDDEN (HASH_HPAS = .);

    .init :
    {
        *(.init .init.*)
        *(.initdata)
        KEEP (*(.head))
    } : init

    PROVIDE (LOAD_E = .);
    PROVIDE (LINK_P = LINK_ADDR - OFFSET);

    .text . + OFFSET : AT (ADDR (.text) - OFFSET)
    {
        *(.text.hot .text.hot.*)
        *(.text .text.* .gnu.linkonce.t.*)
    } : kern

    .fixup : AT (ADDR (.fixup) - OFFSET)
    {
        PROVIDE (FIXUP_S = .);
        KEEP (*(.fixup))
        PROVIDE (FIXUP_E = .);
    } : kern

    .patch : AT (ADDR (.patch) - OFFSET)
    {
        PROVIDE_HIDDEN (PATCH_S = .);
        KEEP (*(.patch.data))
        PROVIDE_HIDDEN (PATCH_E = .);
        KEEP (*(.patch.code))
    } : kern

    .rodata : AT (ADDR (.rodata) - OFFSET)
    {
        *(.rodata .rodata.* .gnu.linkonce.r.*)
    } : kern

    .init_array : AT (ADDR (.init_array) - OFFSET)
    {
        /*
         * Portability note: Some compilers (e.g., on MacOS) still use
         * .ctors instead of .init_array, so we need to handle both.
         */
        PROVIDE_HIDDEN (CTORS_L = .);
        KEEP (*(SORT_BY_INIT_PRIORITY (.init_array.65534) SORT_BY_INIT_PRIORITY (.ctors.00001)))
        PROVIDE_HIDDEN (CTORS_C = .);
        KEEP (*(SORT_BY_INIT_PRIORITY (.init_array.65533) SORT_BY_INIT_PRIORITY (.ctors.00002)))
        PROVIDE_HIDDEN (CTORS_S = .);
        KEEP (*(SORT_BY_INIT_PRIORITY (.init_array.*) SORT_BY_INIT_PRIORITY (.ctors.*)))
        KEEP (*(.init_array .ctors))
        PROVIDE_HIDDEN (CTORS_E = .);
    } : kern

    PROVIDE_HIDDEN (HASH_HPAE = . - OFFSET);

    .data : AT (ADDR (.data) - OFFSET)
    {
        *(.data .data.* .gnu.linkonce.d.*)
    } : kern

    .bss : AT (ADDR (.bss) - OFFSET)
    {
        . = ALIGN(8K) + 8K;
        PROVIDE_HIDDEN (STACK = . - OFFSET);

#if defined(__x86_64__)
        PROVIDE_HIDDEN (PTAB_HPAS = . - OFFSET);
        PROVIDE (PDBR  = . - OFFSET);
        PROVIDE_HIDDEN (PT3S_HPAS = . - OFFSET); . += 4K;
        PROVIDE_HIDDEN (PT2H_HPAS = . - OFFSET); . += 4K;
        PROVIDE_HIDDEN (PT1H_HPAS = . - OFFSET); . += 4K;
        PROVIDE_HIDDEN (PT2L_HPAS = . - OFFSET); . += 4K;
        PROVIDE_HIDDEN (PT1L_HPAS = . - OFFSET); . += 4K;
#endif

        *(.bss .bss.* .gnu.linkonce.b.*)

        . = ALIGN(4K);

        _mempool_l = .; PROVIDE (_mempool_p = _mempool_l - OFFSET);

        PROVIDE (PAGE_0 = .); PROVIDE (FRAME_0 = . - OFFSET); . += 4K;
        PROVIDE (PAGE_1 = .); PROVIDE (FRAME_1 = . - OFFSET); . += 4K;
        PROVIDE (PAGE_H = .); PROVIDE (FRAME_H = . - OFFSET); . += PAGE_H_SIZE;

#ifdef __i386__
        PROVIDE (PDBR  = . - OFFSET);
        PROVIDE (LVL2L = . - OFFSET);
        PROVIDE (LVL2H = . - OFFSET); . += 4K;
#endif
        _mempool_f = .;

        . += CONFIG_MEMORY_BOOT;
        . = ALIGN (ABSOLUTE (.) + 4M, PAGE_SIZE (1));

        PROVIDE (LINK_E = . - OFFSET);

        PROVIDE (_mempool_e = .);

    } : kern

    PROVIDE_HIDDEN (NOVA_HPAE = . - OFFSET);

    PROVIDE_HIDDEN (HASH_SIZE =  HASH_HPAE - HASH_HPAS);
    PROVIDE_HIDDEN (HASH_HEAD = __head_mle - HASH_HPAS);
    PROVIDE_HIDDEN (HASH_INIT = __init_ilp - HASH_HPAS);
    PROVIDE_HIDDEN (HASH_HPAL = (HASH_HPAE - 1) & ~OFFS_MASK (0));
    PROVIDE_HIDDEN (MLE_L0_LS = MLE_L0 + __SIZEOF_POINTER__ * ((HASH_SIZE - 1) >> PAGE_BITS));

    .cpulocal MMAP_CPU_DATA :
    {
        *(SORT_BY_ALIGNMENT (.cpulocal.hot))
        *(SORT_BY_ALIGNMENT (.cpulocal))
    }

    . = ASSERT (SIZEOF (.cpulocal) <= PAGE_SIZE (0), "CPU-Local section too large");

    /DISCARD/ :
    {
        *(.comment*)
        *(.note*)
    }

#if defined(__x86_64__)
    . = ASSERT ((HASH_HPAS & OFFS_MASK (0))                   == 0, "HASH_HPAS misaligned");
    . = ASSERT ((PTAB_HPAS & OFFS_MASK (0))                   == 0, "PTAB_HPAS misaligned");
    . = ASSERT ((NOVA_HPAE & OFFS_MASK (1))                   == 0, "NOVA_HPAE misaligned");
#endif

    . = ASSERT (HASH_SIZE <= (1 << 21), "HASH_SIZE too large");
}
