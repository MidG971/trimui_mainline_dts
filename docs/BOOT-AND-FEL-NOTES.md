<!-- SPDX-License-Identifier: (GPL-2.0-only OR MIT) -->
<!-- Copyright (C) 2026 Midgy BALON -->

# Boot model & FEL bring-up — device night 1 (2026-07-23)

First hands-on attempt to boot mainline on the device. Everything here is from
**live silicon** and supersedes earlier assumptions where they differ. No eMMC or
SD was written during any of this — all attempts were RAM-only and reversible by a
power-cycle.

## Boot order — the device boots eMMC first

A microSD written the normal sunxi way (SPL at 8 KiB + an extlinux partition) is
**not** picked up by the BROM: with a valid eMMC bootloader present, the device
boots stock from eMMC and never falls through to the card. **Raw SD-card boot is a
dead end here.** (The BROM boot-order restriction is a BROM thing; U-Boot itself can
still read the SD once it is running — see "path forward".)

### Vendor firmware update / factory restore (recovery, no PC)
Per the user manual: place `trimui_tg5050.awing` (an Allwinner LiveSuit image) at the
**root of a FAT SD card**, then **hold Power, then press Reset**. The device enters an
on-device "upgrade mode" and reflashes eMMC from the image. This is the clean
brick-recovery path and needs no host tooling. A partial/interrupted update that hangs
at the boot logo is recovered by the **Reset button** — eMMC is untouched by a partial
SD read, and the device returns to stock.

## FEL (USB) — the intended brick-safe path

- **Enter FEL from stock:** `adb reboot efex` (stock has adb root). No button combo.
- **SoC id = `0x1890` = A523**, confirmed over FEL.
- **Tooling:** distro `sunxi-tools` 1.4.2 (2022) is too old — `no soc_sram_info for
  id=1890`. Build `sunxi-tools` from upstream master, which has the A523 profile
  (scratchpad `0x61500`); `sunxi-fel version` then reports `soc=…(A523)`.

## U-Boot — DRAM rail fix + first on-silicon SPL run

- The mainline U-Boot (`trimui-tg5050_defconfig`, avaota-a1 base + this board's DRAM
  params + PMIC@0x34) had a stale DRAM-rail voltage. Corrected
  `CONFIG_AXP_DCDC3_VOLT` **1160 → 1100** (LPDDR4 VCC-DRAM 1.10 V typ; see
  `uboot-a523/DRAM-VALIDATION.md`) and rebuilt.
- **Milestone:** FEL-executing this SPL prints `Executing the SPL... done.` — clocks
  and the DRAM-init sequence run to completion and return cleanly. Our bring-up SPL
  **runs on the real A523.**

## Blocker — FEL enumerates full-speed, bulk data fails

The FEL device comes up as **`bcdUSB 1.10` / full-speed** (`wMaxPacketSize 0x40` =
64-byte bulk). `sunxi-fel version` (tiny control exchanges) always works, but **every
bulk data transfer fails with `usb_bulk_send ERROR -7`** — the 48 KiB SPL upload, a
64 KiB DRAM write, and larger. This is independent of cable, port, and connector
orientation.

It is **not a cable problem:** stock adb moves large data over the same USB-C port
reliably, so the physical path is good. The issue is specific to how this A523's FEL
brings up USB (full-speed) combined with sunxi-tools' fresh, unproven A523 support at
full-speed. DRAM read/back validation is blocked behind this.

## Path forward (ranked)

1. **kexec mainline from the running stock system.** Stock has adb root; if its kernel
   has `CONFIG_KEXEC`, `adb push` the Image + DTB and `kexec -l … ; kexec -e`. This
   sidesteps FEL, U-Boot, *and* the eMMC-first boot order, and uses the USB path that
   already works. Best next step.
2. **Get FEL to high-speed.** A cold hardware FEL entry (vs `adb reboot efex`), or
   whatever the vendor USB flasher negotiates, may bring FEL up at `bcdUSB 2.00` /
   480M, where sunxi-fel bulk transfers should work.
3. **FEL-load only U-Boot, then have U-Boot load the kernel from SD** (`mmc` read works
   even though the BROM won't boot the card). Avoids pushing the ~50 MB kernel through
   FEL — but still needs FEL bulk to move the 835 KB U-Boot, so it depends on (2).

## Status summary

| Item | State |
| :-- | :-- |
| Kernel (display + USB-gadget console built in) | built, clean |
| U-Boot (DRAM rail fixed) | built; SPL runs on silicon |
| Boot model | understood (eMMC-first; FEL is the route) |
| FEL entry + tooling | working (upstream sunxi-tools) |
| FEL bulk data / DRAM validation | **blocked** (full-speed `-7`) |
| First lit pixel | pending — lead with kexec next |
