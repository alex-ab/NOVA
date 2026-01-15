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


#pragma once

#include "console.hpp"
#include "extern.hpp"
#include "memory.hpp"

class Console_mem : public Console
{
    private:

        ALWAYS_INLINE
        static inline char *buf()
        {
            return reinterpret_cast<char *>(HV_GLOBAL_LBUF);
        }

        unsigned pos;

        void reenable () override { }

    public:

        Console_mem() : pos(0) { }

        void putc (int c) override
        {
            *reinterpret_cast<unsigned *>(buf()) = pos;

            *(buf() + sizeof(pos) + pos) = static_cast<char>(c);

            pos = (pos + 1) % (static_cast<unsigned>(PAGE_SIZE (0) - sizeof(pos)));
        }

        void setup();

        static Console_mem con;
};
