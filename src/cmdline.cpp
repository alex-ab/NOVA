/*
 * Command Line Parser
 *
 * Copyright (C) 2009-2011 Udo Steinberg <udo@hypervisor.org>
 * Economic rights: Technische Universitaet Dresden (Germany)
 *
 * Copyright (C) 2012-2013 Udo Steinberg, Intel Corporation.
 * Copyright (C) 2014 Udo Steinberg, FireEye, Inc.
 * Copyright (C) 2015-2018 Alexander Boettcher, Genode Labs GmbH.
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

#include "cmdline.hpp"
#include "string.hpp"
#include "pd.hpp"

bool Cmdline::iommu_amd;
bool Cmdline::iommu_intel;
bool Cmdline::keyb;
bool Cmdline::serial;
bool Cmdline::spinner;
bool Cmdline::vtlb;
bool Cmdline::nodl;
bool Cmdline::nopcid;
bool Cmdline::vga;
bool Cmdline::novpid;
bool Cmdline::logmem;
bool Cmdline::fpu_lazy;
bool Cmdline::hlt;

uint16 Cmdline::serial_ioport;

struct Cmdline::param_map Cmdline::map[] INITDATA =
{
    { "iommu_amd",   &Cmdline::iommu_amd   },
    { "iommu_intel", &Cmdline::iommu_intel },
    { "keyb",        &Cmdline::keyb        },
    { "serial",      &Cmdline::serial      },
    { "spinner",     &Cmdline::spinner     },
    { "vtlb",        &Cmdline::vtlb        },
    { "nodl",        &Cmdline::nodl        },
    { "nopcid",      &Cmdline::nopcid      },
    { "vga",         &Cmdline::vga         },
    { "novpid",      &Cmdline::novpid      },
    { "logmem",      &Cmdline::logmem      },
    { "fpu_lazy",    &Cmdline::fpu_lazy    },
    { "hlt",         &Cmdline::hlt         },
};

char const *Cmdline::get_arg (char const **line, unsigned &len)
{
    len = 0;

    for (; **line == ' '; ++*line) ;

    if (!**line)
        return nullptr;

    char const *arg = *line;

    for (; **line != ' '; ++*line) {
        if (!**line)
            return arg;
        len ++;
    }

    return arg;
}

static uint16 str_to_hex(char value)
{
    switch(value) {
    case '0' : return  0;
    case '1' : return  1;
    case '2' : return  2;
    case '3' : return  3;
    case '4' : return  4;
    case '5' : return  5;
    case '6' : return  6;
    case '7' : return  7;
    case '8' : return  8;
    case '9' : return  9;
    case 'a' : return 10;
    case 'b' : return 11;
    case 'c' : return 12;
    case 'd' : return 13;
    case 'e' : return 14;
    case 'f' : return 15;
    case 'A' : return 10;
    case 'B' : return 11;
    case 'C' : return 12;
    case 'D' : return 13;
    case 'E' : return 14;
    case 'F' : return 15;
    default  : return 65535;
    }
}

void Cmdline::init (char const * line)
{
    char const *arg;
    unsigned len;

    while ((arg = get_arg (&line, len))) {
        for (unsigned i = 0; i < sizeof map / sizeof *map; i++) {
            if (strmatch (map[i].arg, arg, len))
                *map[i].ptr = true;

            if (map[i].ptr && strmatch(map[i].arg, "serial", len)) {
                auto arg_next = get_arg (&line, len);

                if (len > 2 && strmatch("0x", arg_next, 2)) {
                    uint16 value { };

                    for (auto j = len - 1, step = 0u; i > 2 && step < 16; j--, step += 4) {
                        auto val = str_to_hex(arg_next[j]);
                        if (val > 15) continue;

                        value += uint16(str_to_hex(arg_next[j]) << step);
                    }

                    Cmdline::serial_ioport = value;
                    arg = arg_next;
                }
            }
        }
    }
}
