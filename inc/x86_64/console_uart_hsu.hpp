/*
 * Console: High-Speed UART (HSU)
 *
 * Copyright (C) 2019-2024 Udo Steinberg, BlueRock Security, Inc.
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

#include "console_uart_ns16550.hpp"

class Console_uart_hsu final : private Console_uart_ns16550
{
    private:
        static Console_uart_hsu uart[];

        static Regs probe (pci_t);

    protected:
        explicit Console_uart_hsu (pci_t pci) : Console_uart_ns16550 { probe (pci), 1'843'200 } {}
};
