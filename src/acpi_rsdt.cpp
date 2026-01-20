/*
 * Advanced Configuration and Power Interface (ACPI)
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

#include "acpi.hpp"
#include "acpi_rsdt.hpp"
#include "hpt.hpp"
#include "pd.hpp"
#include "checksum.hpp"

struct Acpi_table_rsdt::table_map Acpi_table_rsdt::map[] INITDATA =
{
    { SIG ('A','P','I','C'),  44,  &Acpi::madt },
    { SIG ('D','M','A','R'),  48,  &Acpi::dmar },
    { SIG ('F','A','C','P'), 244,  &Acpi::fadt },
    { SIG ('H','P','E','T'),  56,  &Acpi::hpet },
    { SIG ('M','C','F','G'),  44,  &Acpi::mcfg },
    { SIG ('I','V','R','S'),  48,  &Acpi::ivrs },
};

void Acpi_table_rsdt::parse (Paddr addr, size_t size) const
{
    if (!good_checksum (addr))
        return;

    unsigned long count = entries (size);

    Paddr table[count];
    for (unsigned i = 0; i < count; i++)
        table[i] = static_cast<Paddr>(size == 8 ? xsdt(i) : rsdt(i));

    for (unsigned i = 0; i < count; i++) {

        Acpi_table *acpi = static_cast<Acpi_table *>(Hpt::remap (Pd::kern.quota, table[i]));

        if (acpi->good_checksum (table[i]))
            for (unsigned j = 0; j < sizeof map / sizeof *map; j++)
                if (acpi->signature == map[j].sig)
                    *map[j].ptr = table[i];
    }
}

bool Acpi_table::validate (uint64_t phys) const
{
    // Checksum must be correct
    auto const valid { Checksum::additive (reinterpret_cast<uint8_t const *>(this), length) == 0 };

    trace (TRACE_FIRM, "%4.4s: %#010llx OEM:%6.6s TBL:%8.8s REV:%2u LEN:%8u (%s)",
           reinterpret_cast<char const *>(&signature), phys, oem_id, oem_table_id,
           uint8_t { revision }, uint32_t { length }, valid ? "ok" : "bad");

    auto & tables = Acpi_table_rsdt::map;

    // If table address was already set by measured launch, then do not overwrite it
    if (valid) [[likely]]
        for (unsigned i { 0 }; i < sizeof (tables) / sizeof (*tables); i++)
            if (tables[i].sig == signature && tables[i].len <= length && !tables[i].ptr)
                *tables[i].ptr = uintptr_t(phys);

    return valid;
}
