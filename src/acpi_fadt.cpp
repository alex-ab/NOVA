/*
 * Advanced Configuration and Power Interface (ACPI)
 *
 * Copyright (C) 2009-2011 Udo Steinberg <udo@hypervisor.org>
 * Economic rights: Technische Universitaet Dresden (Germany)
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
#include "acpi_fadt.hpp"
#include "io.hpp"
#include "x86.hpp"
#include "assert.hpp"


/**
 * Initialize STS and ENA from register-block configuration
 */
void Acpi_table_fadt::init_gas(Acpi_gas &sts, Acpi_gas &ena, uint8 len, uint32 blk, Acpi_gas const &x_gas) const
{
    /*
     * If the FADT comprises (all) extended addresses for register blocks,
     * check those first according to the standard, which states: "If this
     * [extended address] field contains a nonzero value which can be used by
     * the OSPM, then the [corresponding BLK] field must be ignored by the
     * OSPM."
     *
     * BLK and LEN configuration is used as fallback, and if this fails too,
     * the Acpi_gas is invalidated.
     */

    if (length >= 236 && x_gas.addr != 0) {
        uint8 const reg_len = x_gas.bits/8/2;
        sts.init(x_gas.asid, reg_len, x_gas.addr);
        ena.init(x_gas.asid, reg_len, x_gas.addr + reg_len);
    } else if (blk != 0 && len != 0) {
        uint8 const reg_len = len/2;
        sts.init(Acpi_gas::IO, reg_len, blk);
        ena.init(Acpi_gas::IO, reg_len, blk + reg_len);
    } else {
        sts.init(0, 0, 0);
        ena.init(0, 0, 0);
    }
}


/**
 * Initialize single register from register-block configuration
 */
void Acpi_table_fadt::init_gas(Acpi_gas &gas, uint8 len, uint32 blk, Acpi_gas const &x_gas) const
{
    /* see overload above for description of the conditions */
    if (length >= 236 && x_gas.addr != 0)
        gas.init(x_gas.asid, x_gas.bits/8, x_gas.addr);
    else if (blk != 0 && len != 0)
        gas.init(Acpi_gas::IO, len, blk);
    else
        gas.init(0, 0, 0);
}


void Acpi_table_fadt::parse() const
{
    Acpi::irq     = sci_irq;
    Acpi::feature = flags;

    init_gas(Acpi::pm1a_sts, Acpi::pm1a_ena, pm1_evt_len, pm1a_evt_blk, x_pm1a_evt_blk);
    init_gas(Acpi::pm1b_sts, Acpi::pm1b_ena, pm1_evt_len, pm1b_evt_blk, x_pm1b_evt_blk);
    init_gas(Acpi::pm1a_cnt, pm1_cnt_len, pm1a_cnt_blk, x_pm1a_cnt_blk);
    init_gas(Acpi::pm1b_cnt, pm1_cnt_len, pm1b_cnt_blk, x_pm1b_cnt_blk);
    init_gas(Acpi::pm2_cnt, pm2_cnt_len, pm2_cnt_blk, x_pm2_cnt_blk);
    init_gas(Acpi::pm_tmr, pm_tmr_len, pm_tmr_blk, x_pm_tmr_blk);
    init_gas(Acpi::gpe0_sts, Acpi::gpe0_ena, gpe0_blk_len, gpe0_blk, x_gpe0_blk);
    init_gas(Acpi::gpe1_sts, Acpi::gpe1_ena, gpe1_blk_len, gpe1_blk, x_gpe1_blk);

    if (length >= 129) {
        Acpi::reset_reg = reset_reg;
        Acpi::reset_val = reset_value;
    }

    if (smi_cmd && acpi_enable) {
        Io::out (smi_cmd, acpi_enable);
        while (!(Acpi::read (Acpi::PM1_CNT) & Acpi::PM1_CNT_SCI_EN))
            pause();
    }
}
