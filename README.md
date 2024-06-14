# NOVA Microhypervisor

This is the source code for the NOVA microhypervisor.

The NOVA microhypervisor combines microkernel and hypervisor functionality
and provides an extremely small trusted computing base for user applications
and virtual machines running on top of it. The microhypervisor implements a
capability-based authorization model and provides basic mechanisms for
virtualization, spatial and temporal separation, scheduling, communication,
and management of platform resources.

NOVA can be used with a multi-server environment that implements additional
operating-system services in user mode, such as device drivers, protocol
stacks, and policies. On machines with hardware virtualization features,
multiple unmodified guest operating systems can run concurrently on top of
the microhypervisor.

**This code is experimental and not feature complete. If it breaks, you get
  to keep both pieces.**

## Building

### Required Tools

The following tools are required to compile the source code:

| **Tool** | **Minimum Version** | **Available From**                |
| :------- | :-----------------: | :-------------------------------- |
| binutils | 2.38                | https://ftp.gnu.org/gnu/binutils/ |
| gcc      | 11.4                | https://ftp.gnu.org/gnu/gcc/      |
| make     | 4.0                 | https://ftp.gnu.org/gnu/make/     |

### Build Environment

The build environment can be customized permanently in `Makefile.conf` or
ad hoc by passing the applicable `ARCH`, `BOARD` and `PREFIX_` variables to
the invocation of `make` as described below.

- `PREFIX_aarch64` sets the path for an **ARMv8-A** cross-toolchain
- `PREFIX_x86_64` sets the path for an **x86 (64bit)** cross-toolchain

For example, if the ARMv8-A cross-toolchain is located at
```
/opt/aarch64-linux/bin/aarch64-linux-gcc
/opt/aarch64-linux/bin/aarch64-linux-as
/opt/aarch64-linux/bin/aarch64-linux-ld
```

then set `PREFIX_aarch64=/opt/aarch64-linux/bin/aarch64-linux-`

### Supported Architectures

#### ARMv8-A (64bit)

For CPUs with ARMv8-A architecture and boards with
- either Advanced Configuration and Power Interface (ACPI)
- or Flattened Device Tree (FDT)

| **Platform**                          | **Build Command**                            |
| :------------------------------------ | :------------------------------------------- |
| Generic Arm ACPI Platform             | `make ARCH=aarch64 BOARD=acpi`               |
| QEMU Virt Platform                    | `make ARCH=aarch64 BOARD=qemu`               |
| Allwinner A64                         | `make ARCH=aarch64 BOARD=allwinner_a64`      |
| Amlogic G12B                          | `make ARCH=aarch64 BOARD=amlogic_g12b`       |
| Amlogic SM1                           | `make ARCH=aarch64 BOARD=amlogic_sm1`        |
| Broadcom BCM2711                      | `make ARCH=aarch64 BOARD=broadcom_bcm2711`   |
| HiSilicon Hi3660                      | `make ARCH=aarch64 BOARD=hisilicon_hi3660`   |
| NVIDIA Tegra X1                       | `make ARCH=aarch64 BOARD=nvidia_tegrax1`     |
| NVIDIA Tegra X2                       | `make ARCH=aarch64 BOARD=nvidia_tegrax2`     |
| NVIDIA Xavier                         | `make ARCH=aarch64 BOARD=nvidia_xavier`      |
| NXP i.MX 8M                           | `make ARCH=aarch64 BOARD=nxp_imx8m`          |
| Qualcomm Snapdragon 670               | `make ARCH=aarch64 BOARD=qualcomm_sdm670`    |
| Renesas R-Car M3                      | `make ARCH=aarch64 BOARD=renesas_rcar3`      |
| Rockchip RK3399                       | `make ARCH=aarch64 BOARD=rockchip_rk3399`    |
| Texas Instruments J721E               | `make ARCH=aarch64 BOARD=ti_j721e`           |
| Xilinx Zynq Ultrascale+ MPSoC CG      | `make ARCH=aarch64 BOARD=xilinx_zynq_cg`     |
| Xilinx Zynq Ultrascale+ MPSoC Ultra96 | `make ARCH=aarch64 BOARD=xilinx_zynq_u96`    |
| Xilinx Zynq Ultrascale+ MPSoC ZCU102  | `make ARCH=aarch64 BOARD=xilinx_zynq_zcu102` |

#### x86 (64bit)

For CPUs with x86 architecture
- Intel VT-x (VMX+EPT) + optionally VT-d
- AMD-V (SVM+NPT)

and boards with Advanced Configuration and Power Interface (ACPI).

| **Platform**                          | **Build Command**  |
| :------------------------------------ | :----------------- |
| Generic x86 ACPI Platform             | `make ARCH=x86_64` |

##### Control-Flow Enforcement Technology (CET)

NOVA can be built with support for control-flow protection. Because
control-flow protected binaries require a CPU with CET support and because
of the resulting performance overhead, CFP is disabled by default.
Protection features can be enabled at build time as follows:

| **Feature Level**                     | **Build Command**             |
| :------------------------------------ | :---------------------------- |
| No Control-Flow Protection (Default)  | `make ARCH=x86_64 CFP=none`   |
| CET Indirect Branch Tracking (IBT)    | `make ARCH=x86_64 CFP=branch` |
| CET Supervisor Shadow Stacks (SSS)    | `make ARCH=x86_64 CFP=return` |
| CET IBT and CET SSS                   | `make ARCH=x86_64 CFP=full`   |

##### Trusted Execution Technology (TXT)

On TXT-enabled platforms, NOVA performs a measured launch to establish a
Dynamic Root of Trust for Measurement (DRTM) if an SINIT Authenticated Code
Module (ACM) matching the platform is present in TXT memory.

The SINIT ACM is typically loaded into TXT memory
- on server platforms: by the firmware
- on client platforms: by the bootloader

## Booting

See the NOVA interface specification in the `doc` directory for details
regarding booting the NOVA microhypervisor.

## License

The NOVA source code is licensed under the **GPL version 2**.

```
Copyright (C) 2009-2011 Udo Steinberg <udo@hypervisor.org>
Economic rights: Technische Universitaet Dresden (Germany)

Copyright (C) 2012-2013 Udo Steinberg, Intel Corporation.
Copyright (C) 2014 Udo Steinberg, FireEye, Inc.
Copyright (C) 2019-2024 Udo Steinberg, BedRock Systems, Inc.

NOVA is free software: you can redistribute it and/or modify it
under the terms of the GNU General Public License version 2 as
published by the Free Software Foundation.

NOVA is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
GNU General Public License version 2 for more details.
```

## Contact

Feedback and comments should be sent to udo@hypervisor.org
