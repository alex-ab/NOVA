/*
 * Log kernel log to memory
 *
 * Copyright (C) 2016 Alexander Boettcher, Genode Labs GmbH
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

#include "console_mem.hpp"
#include "pd.hpp"
#include "string.hpp"

INIT_PRIORITY (PRIO_CONSOLE) Console_mem Console_mem::con;

mword PAGE_L = 0;

void Console_mem::setup()
{
    if (!PAGE_L)
        return;

    Pd::kern.Space_mem::insert (Pd::kern.quota, HV_GLOBAL_LBUF, 0, Hpt::HPT_NX | Hpt::HPT_G | Hpt::HPT_UC | Hpt::HPT_W | Hpt::HPT_P, PAGE_L);

    memset (reinterpret_cast<void *>(HV_GLOBAL_LBUF), 0, PAGE_SIZE (0));

    enable();
}
