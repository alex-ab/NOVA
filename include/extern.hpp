/*
 * External Symbols
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

#include "memory.hpp"
#include "types.hpp"

extern char GIT_VER, NOVA_HPAS, NOVA_HPAE, HASH_INIT, HASH_HEAD, MLE_TL, MLE_L2, MLE_L1, MLE_L0, HASH_HPAS, HASH_HPAE, PT3S_HPAS;

extern char __head_mle[52];

extern char PAGE_0[PAGE_SIZE (0)];
extern char PAGE_1[PAGE_SIZE (0)];
extern char PAGE_H[PAGE_H_SIZE];
extern mword PAGE_L;

extern char FRAME_0;
extern char FRAME_1;
extern char FRAME_H;

extern char PDBR;
extern char PTAB_HPAS;

extern char LINK_P;
extern char LINK_E;
extern char LOAD_E;

extern mword FIXUP_S;
extern mword FIXUP_E;

extern void (*CTORS_L)();
extern void (*CTORS_C)();
extern void (*CTORS_S)();
extern void (*CTORS_E)();

extern char entry_sysenter;
extern char entry_vmx;
extern mword handlers[];
extern mword hwdev_addr;
