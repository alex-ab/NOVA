/*
 * IO-MMU abstract
 *
 * Copyright (C) 2020 Alexander Boettcher <alexander.boettcher@genode-labs.com>
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

#include "lapic.hpp"
#include "types.hpp"
#include "x86.hpp"
#include "stdio.hpp"

class Pd;

namespace Iommu
{
    class Interface;
};

class Iommu::Interface
{
    protected:

        struct {
            uint16 rid     { 0 };
            uint8  count   { 0 };
            uint8  changed { 0 };
        } fault_info[4] { };

        bool disable_reporting(uint16 const rid)
        {
            unsigned const max = sizeof(fault_info) / sizeof(fault_info[0]);
            unsigned free = max;
            unsigned i = 0;

            for (; i < max; i++) {
                if (free >= max && !fault_info[i].count)
                    free = i;

                if (fault_info[i].count && fault_info[i].rid == rid) {
                    fault_info[i].count ++;
                    fault_info[i].changed = 1;
                    break;
                }
            }

            if (i >= max && free < max) {
                i = free;
                fault_info[i].count = 1;
                fault_info[i].changed = 1;
                fault_info[i].rid = rid;
            }

            /* heuristics are bad ... */
            if (i >= max || fault_info[i].count > 8) {
                trace(TRACE_SMMU, "IOMMU:%p - disabling fault reporting", this);
                return true;
            }
            return false;
        }

        void update_reporting(bool const disabled)
        {
            unsigned const max = sizeof(fault_info) / sizeof(fault_info[0]);

            for (unsigned i = 0; i < max; i++) {
                if (disabled)
                    fault_info[i].changed = 0;

                if (fault_info[i].changed)
                    fault_info[i].changed = 0;
                else
                    fault_info[i].count = 0;
            }
        }

    public:
        virtual void assign (uint16, Pd *) = 0;

        REGPARM (1)
        static void vector (unsigned) asm ("msi_vector");

        static void set_irt (unsigned, unsigned, unsigned, unsigned, unsigned);
        static void release (uint16, Pd *);
        static void flush_pgt(uint16, Pd &);
};
