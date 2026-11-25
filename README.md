# m1n1, m2pro-s2r-wip: personal experiments, not for use

**This branch is a personal experiment log, not a release and not a contribution.** It is the m1n1 stage 2 that
boots one MacBook Pro 16" M2 Pro (t6020, J416s) running NixOS, with native suspend to RAM (S2R) on top of m1n1
v2.0.0. It is unreviewed, carries bench-only switches and hypervisor tracing, and is tested on that one machine
only. Don't use it on a machine you care about, and don't report problems with it to Asahi Linux or upstream
m1n1. The changes were written with AI assistance ([Claude Code](https://claude.ai/code)); upstream m1n1 does not
accept AI-assisted contributions, and nothing here is offered to it.

The matching kernel is https://github.com/nekolyanich/linux/tree/m2pro-s2r-wip (Asahi 7.3.4 line). Discussion of
the S2R work on t6020: https://github.com/JJRPF/Aurora-DeepIdle/issues/5.

## What this branch adds for S2R

- The wake path for an OS at EL2: at boot iBoot locks every core's RVBAR to m1n1's image; m1n1 stays resident in
  reserved memory (kept out of Linux's RAM) and publishes a mailbox. At the wake the boot core enters m1n1 at the
  RVBAR, m1n1 tells a real wake from the short bounce, redoes `init_cpu`, writes back each cluster's cpufreq word,
  replays the AON writes, reruns its apcie bring-up and jumps to Linux's `cpu_resume` at EL2. Secondaries come back
  through RVBAR -> m1n1 -> spin table.
- Daily defaults: only the SLC is skipped at boot; a WD1 dead hand at kboot until Linux's apple_wdt takes over.
- 256K of FDT room (the PMP nub's tunables overflow 96K), `apfs-preboot-uuid` forwarded to /chosen, sep-warm on M2.

## Kernel command line used with it (NixOS, all as of 2026-11-25)

The S2R words on top of the usual Asahi line. "Bench" in a name means the switch started on the test bench; these
are still needed for the cut to work.

```
apple_sleep.s2r=1 apple_sleep.via_firmware=1 apple_sleep.resume=2 apple_sleep.slc_ok=1
apple_sleep.edge_timer=1 apple_sleep.edge_on=1040207 apple_sleep.groups=0xfcfb55ffdfffffbf
apple_sleep.smc_notify=7 apple_sleep.pmvc=0x100 apple_sleep.sep_sleep_early=1 apple_sleep.aop_sleep=1
apple_sleep.hybrid=0x5e32600 appledrm.sleep_hold=1
spmi_apple_controller.boot_crumb=1 apple_sleep.dispext_crumb=1
nvme_apple.sleep_idle=1 nvme_apple.sleep_keep_ps=1 nvme_apple.bench_reinit_first=1
nvme_apple.bench_ans_tunables=7 nvme_apple.bench_reinit_keep=1
pcie_apple.bench_perst=3 pcie_apple.bench_gpfix=5 pcie_apple.bench_resume=1
dwc3_apple.sleep_stop=1 phy_apple_atc.s2r_assume_cut=1
asahi.sleep_s2r=3
```

| Words | What they do |
|---|---|
| `apple_sleep.s2r`, `via_firmware`, `resume=2` | the full S2R entry (the sleep-seq table), `pm_suspend_via_firmware()` for the drivers' firmware-sleep paths, and the return through `cpu_suspend` with m1n1's mailbox |
| `slc_ok`, `edge_timer`, `edge_on`, `groups` | the entry with the SLC on, cpu0's edge as macOS's, the power-state words and entry groups the cut runs with |
| `smc_notify=7`, `pmvc=0x100` | slpw/slpa/0x201, WKTP=2 and pmVC to the SMC, as macOS |
| `sep_sleep_early`, `aop_sleep` | the SEP's SLEEP at device suspend (the cut needs it), the AOP's sleep path |
| `hybrid=0x5e32600` | the bit set the cut runs with (DCP boot, SD, PCIe MSI, CPUs on, native hold/quench, edge witness, DCP at EL2) |
| `appledrm.sleep_hold` | the external DCPs' firmware stops for the sleep; without it nothing cuts |
| `boot_crumb`, `dispext_crumb` | progress bytes in the PMU, so a failed sleep says how far it got |
| `nvme_apple.*` | the root NVMe across the power loss (ANS re-init and tunables after the cut) |
| `pcie_apple.*` | macOS's PCIe port teardown before the sleep and its way back (Wi-Fi, BT, SD) |
| `dwc3_apple.sleep_stop`, `phy_apple_atc.s2r_assume_cut` | stop the USB hosts before the sleep (together or neither); without them the second sleep of a boot aborts at xhci (Run/Stop -110). Cost: a mounted USB disk loses its block device at every sleep |
| `asahi.sleep_s2r=3` | the GPU firmware idles at `.prepare` and restarts warm after the wake (works 2 of 2; an aborted sleep loses the GPU until a reboot). The commit idle (`=1`) fails: GFX is auto-gated at the commit |

Module defaults the kernel branch changes, no cmdline needed: `clk_apple_nco.s2r_enable` (the audio NCO channels
stay enabled at the commit; with them off the cut doesn't happen with the sound card bound) and
`snd_soc_apple_mca.fmt_restore` (the MCA DAI format written again after a wake; the speakers need it).

---

# m1n1: A bootloader and experimentation playground for Apple Silicon

## Building

You need an `aarch64-linux-gnu-gcc` cross-compiler toolchain (or a native one, if running on ARM64).
You will also need to install the `aarch64-unknown-none-softfloat` toolchain for rust.

```shell
$ rustup target add aarch64-unknown-none-softfloat
```

```shell
$ git clone --recursive https://github.com/AsahiLinux/m1n1.git
$ cd m1n1
$ make
```

To build on a native ARM64 machine:
* On Linux, use `make ARCH=`.
* On macOS using Homebrew:
```shell
$ brew install llvm lld
$ make
```
* On macOS using MacPorts:
```shell
$ sudo port install llvm clang
$ sudo port select llvm llvm-mp-<version>
$ make
```

The output will be in `build/m1n1.macho`.

To build verbosely, use `make V=1`.

### Building using the container setup

If you have a container runtime installed, like Podman or Docker, you can make use of the compose setup, which contains all build dependencies.

```shell
$ git clone --recursive https://github.com/AsahiLinux/m1n1.git
$ cd m1n1
$ podman-compose run m1n1 make
$ # or
$ docker-compose run m1n1 make
```

## Usage

Our [wiki](https://asahilinux.org/docs/sw/m1n1-user-guide/) has more information on how to
use m1n1.

To install on an OS container based on macOS <12.1, use `m1n1.macho`:

```shell
kmutil configure-boot -c m1n1.macho -v <path to your OS volume>
```

To install on an OS container based on macOS >=12.1, use `m1n1.bin`:

```shell
kmutil configure-boot -c m1n1.bin --raw --entry-point 2048 --lowest-virtual-address 0 -v <path to your OS volume>
```

## Payloads

m1n1 supports running payloads by simple concatenation:

```shell
$ cat build/m1n1.macho Image.gz build/dtb/apple-j274.dtb initramfs.cpio.gz > m1n1-payload.macho
$ cat build/m1n1.bin Image.gz build/dtb/apple-j274.dtb initramfs.cpio.gz > m1n1-payload.bin
```

Supported payload file formats:

* Kernel images (or compatible). Must be compressed or last payload.
* Devicetree blobs (FDT). May be uncompressed or compressed.
* Initramfs cpio images. Must be compressed.

Supported compression formats:

* gzip
* xz

## License
m1n1 was made available under the terms of the [MIT license](LICENSE.MIT)
until version 1.9.9. Code present in those releases continues to be available
under that same license. Starting from version 2.0.0, m1n1 is licensed
under the terms of the [GNU General Public License version 2](LICENSE) (or later); any
code checked in thereafter is available under the terms of the GPL exclusively.

* Copyright The Asahi Linux Contributors

Please see the Git history for authorship information.

Portions of m1n1 are based on mini:

* Copyright (C) 2008-2010 Hector Martin "marcan" <marcan@marcan.st>
* Copyright (C) 2008-2010 Sven Peter <sven@svenpeter.dev>
* Copyright (C) 2008-2010 Andre Heider <a.heider@gmail.com>

m1n1 embeds libfdt, which is dual [BSD](3rdparty_licenses/LICENSE.BSD-2.libfdt) and
[GPL-2](3rdparty_licenses/LICENSE.GPL-2) licensed and copyright:

* Copyright (C) 2014 David Gibson <david@gibson.dropbear.id.au>
* Copyright (C) 2018 embedded brains GmbH
* Copyright (C) 2006-2012 David Gibson, IBM Corporation.
* Copyright (C) 2012 David Gibson, IBM Corporation.
* Copyright 2012 Kim Phillips, Freescale Semiconductor.
* Copyright (C) 2016 Free Electrons
* Copyright (C) 2016 NextThing Co.

The ADT code in mini is also based on libfdt and subject to the same license.

m1n1 embeds [minlzma](https://github.com/ionescu007/minlzma), which is
[MIT](3rdparty_licenses/LICENSE.minlzma) licensed and copyright:

* Copyright (c) 2020 Alex Ionescu

m1n1 embeds a slightly modified version of [tinf](https://github.com/jibsen/tinf), which is
[ZLIB](3rdparty_licenses/LICENSE.tinf) licensed and copyright:

* Copyright (c) 2003-2019 Joergen Ibsen

m1n1 embeds portions taken from
[arm-trusted-firmware](https://github.com/ARM-software/arm-trusted-firmware), which is
[BSD](3rdparty_licenses/LICENSE.BSD-3.arm) licensed and copyright:

* Copyright (c) 2013-2020, ARM Limited and Contributors. All rights reserved.

m1n1 embeds [Doug Lea's malloc](ftp://gee.cs.oswego.edu/pub/misc/malloc.c) (dlmalloc), which is in
the public domain ([CC0](3rdparty_licenses/LICENSE.CC0)).

m1n1 embeds portions of [PDCLib](https://github.com/DevSolar/pdclib), which is in the public
domain ([CC0](3rdparty_licenses/LICENSE.CC0)).

m1n1 embeds the [Source Code Pro](https://github.com/adobe-fonts/source-code-pro) font, which is
licensed under the [OFL-1.1](3rdparty_licenses/LICENSE.OFL-1.1) license and copyright:

* Copyright 2010-2019 Adobe (http://www.adobe.com/), with Reserved Font Name 'Source'. All Rights Reserved. Source is a trademark of Adobe in the United States and/or other countries.
* This Font Software is licensed under the SIL Open Font License, Version 1.1.

m1n1 embeds portions of the [dwc3 usb linux driver](https://git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git/tree/drivers/usb/dwc3/core.h?id=7bc5a6ba369217e0137833f5955cf0b0f08b0712), which was [BSD-or-GPLv2 dual-licensed](3rdparty_licenses/LICENSE.BSD-3.dwc3) and copyright
* Copyright (C) 2010-2011 Texas Instruments Incorporated - http://www.ti.com

m1n1 embeds portions of [musl-libc](https://musl.libc.org/)'s floating point library, which are MIT licensed and copyright
* Copyright (c) 2017-2018, Arm Limited.

m1n1 embeds some rust crates. Licenses can be found in the vendor directory for every crate.
