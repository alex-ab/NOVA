/*
 * Advanced Configuration and Power Interface (ACPI)
 *
 * Copyright (C) 2009-2011 Udo Steinberg <udo@hypervisor.org>
 * Economic rights: Technische Universitaet Dresden (Germany)
 *
 * Copyright (C) 2012-2013 Udo Steinberg, Intel Corporation.
 * Copyright (C) 2019-2024 Udo Steinberg, BedRock Systems, Inc.
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

#include "acpi.hpp"
#include "ptab_hpt.hpp"

void Acpi_table_xsdt::parse (size_t size) const
{
    auto const len { table.header.length };

    if (EXPECT_FALSE (len < sizeof (*this)))
        return;

    for (auto ptr { reinterpret_cast<uintptr_t>(this + 1) }, end { reinterpret_cast<uintptr_t>(this) + len }; ptr < end; ptr += size) {

        uint64_t const p { size == sizeof (uint64_t) ? *reinterpret_cast<Unaligned_le<uint64_t> const *>(ptr) : *reinterpret_cast<Unaligned_le<uint32_t> const *>(ptr) };

        if (EXPECT_TRUE (p))
            static_cast<Acpi_table const *>(Hptp::map (MMAP_GLB_MAP1, p))->validate (p);
    }
}
