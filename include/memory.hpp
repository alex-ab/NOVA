/*
 * Virtual-Memory Layout
 *
 * Copyright (C) 2009-2011 Udo Steinberg <udo@hypervisor.org>
 * Economic rights: Technische Universitaet Dresden (Germany)
 *
 * Copyright (C) 2012-2013 Udo Steinberg, Intel Corporation.
 * Copyright (C) 2019-2025 Udo Steinberg, BlueRock Security, Inc.
 * Copyright (C) 2015-2026 Alexander Boettcher, Genode Labs GmbH.
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

#include "alignment.hpp"

#define AP_BOOT_PADDR   0x1000
#define LOAD_ADDR       (0x200000 * 8)

#if     defined(__i386__)
#define PTE_BPL         10
#else
#define PTE_BPL         9
#endif
#define PAGE_BITS       12
#define LEVL_BITS(L)    ((L) * PTE_BPL + PAGE_BITS)
#define PAGE_SIZE(L)    BITN (LEVL_BITS (L))
#define OFFS_MASK(L)    (PAGE_SIZE (L) - 1)

#define VIRT_ADDR(L3,L2,L1,L0)  (VALN_SHIFT (0xffff, LEVL_BITS (4)) | VALN_SHIFT (L3, LEVL_BITS (3)) | VALN_SHIFT (L2, LEVL_BITS (2)) | VALN_SHIFT (L1, LEVL_BITS (1)) | VALN_SHIFT (L0, LEVL_BITS (0)))

#define PAGE_MASK       (PAGE_SIZE (0) - 1)

#if     defined(__i386__)
#define USER_ADDR       0xc0000000
#define CANONICAL_ADDR  USER_ADDR
#define LINK_ADDR       0xc0000000
#define CPU_LOCAL       0xcfc00000
#define SPC_LOCAL       0xd0000000
#elif   defined(__x86_64__)
#define USER_ADDR       0x00007fffc0000000
#define CANONICAL_ADDR  0x0000800000000000
#define LINK_ADDR       0xffffffff81000000
#define CPU_LOCAL       0xffffffffbfe00000
#define SPC_LOCAL       0xffffffffc0000000
#endif

#define MMAP_GLB_TXTH_PAGES  (512 * 2)
#define MMAP_GLB_MAP0_PAGES  (512 * 2)
#define MMAP_GLB_TPM2_OFFSET (512 - 320) // -> 0x140000 offset
#define MMAP_GLB_TXTC_OFFSET (512 - 288) // -> 0x120000 offset

#define MMAP_GLB_MAX    0x2000000
#define MMAP_GLB_CPUS   (CPU_LOCAL      - MMAP_GLB_MAX)
#define MMAP_GLB_TXTH   (CPU_LOCAL      - PAGE_SIZE (0) * MMAP_GLB_TXTH_PAGES)  //   4M + gap
#define MMAP_GLB_MAP0   (MMAP_GLB_TXTH  - PAGE_SIZE (0) * MMAP_GLB_MAP0_PAGES)  //   4M + gap
#define MMAP_GLB_TPM2   (MMAP_GLB_MAP0  - PAGE_SIZE (0) * MMAP_GLB_TPM2_OFFSET) //   2M (0xfed40000 offset in 2M window)
#define MMAP_GLB_TXTC   (MMAP_GLB_MAP0  - PAGE_SIZE (0) * MMAP_GLB_TXTC_OFFSET) //   2M (0xfed20000 offset in 2M window)
#define HV_GLOBAL_LBUF  (MMAP_GLB_MAP0  - PAGE_SIZE (1) - PAGE_SIZE (0) * 1)    // avoid overlap with TPM2 and TXTC by PAGE_SIZE(1) !
#define HV_GLOBAL_FBUF  (HV_GLOBAL_LBUF - PAGE_SIZE (0) * 1)
/* (CPU_LOCAL - PAGE_SIZE (0) * 3) used by ioapic/dmar/pci !!! hwdev_addr (hwdev_addr = HV_GLOBAL_FBUF) - PAGE_SIZE (0) !!! */

#define BUDDY_V_MAX     (MMAP_GLB_CPUS - 0x400000)

#define CPU_LOCAL_STCK  (SPC_LOCAL - PAGE_SIZE (0) * 6)
#define CPU_LOCAL_APIC  (SPC_LOCAL - PAGE_SIZE (0) * 4)
#define MMAP_CPU_DATA   (SPC_LOCAL - PAGE_SIZE (0) * 2)

#define SPC_LOCAL_IOP   (SPC_LOCAL)
#define SPC_LOCAL_IOP_E (SPC_LOCAL_IOP + PAGE_SIZE (0) * 2)
#define SPC_LOCAL_REMAP (SPC_LOCAL_OBJ - 0x1000000)
#define SPC_LOCAL_OBJ   (END_SPACE_LIM - 0x20000000)

#define END_SPACE_LIM   (~0UL + 1)

#define PAGE_H_SIZE     0x4000

#define OFFSET          (LINK_ADDR - LOAD_ADDR)
