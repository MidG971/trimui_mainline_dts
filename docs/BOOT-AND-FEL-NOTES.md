<!-- SPDX-License-Identifier: (GPL-2.0-only OR MIT) -->
<!-- Copyright (C) 2026 Midgy BALON -->

# A523 mainline boot bring-up — findings & state

Living notes on getting mainline booting on the Trimui Smart Pro S (Allwinner
A523 / sun55iw3, board TG5050). Everything here is from **live silicon**. Several
early conclusions were **wrong and are corrected below** — kept visible because the
corrections are the useful part. eMMC (stock) was never overwritten; all attempts
are SD-card + FEL only, reversible by removing the card / power-cycle.

## Boot model (CORRECTED — the device boots the SD)

The A523 BROM **does boot from the microSD on a normal power-on**, no button combo —
*provided the SD carries a valid boot0 at the 128 KiB offset with a GPT*. Proven by
**KNULLI Scarab** (a Batocera-based CFW), which boots this exact board from SD.

- Earlier we wrote an SD the classic sunxi way (SPL at **8 KiB**, MBR) and it was
  ignored → we *wrongly* concluded "eMMC-first / raw-SD boot is dead." The real
  reason: this BROM boots the SD from **128 KiB**, and the SPL was at 8 KiB.
  (U-Boot docs: newer sunxi, incl. all arm64, check 8 KiB *then* 128 KiB; the
  vendor/KNULLI layout uses 128 KiB + GPT so it coexists with the partition table.)
- Boot order is SD-then-eMMC: pull the (bootable) card and it boots stock from eMMC.

**Vendor factory-restore (recovery, no PC):** put `trimui_tg5050.awing` (Allwinner
LiveSuit image) at the root of a FAT SD, hold **Power then Reset** → on-device
flasher reimages eMMC. A partial/hung update is recovered by the **Reset** button
(eMMC untouched by a partial SD read). This is the brick lifeline.

## The mainline boot chain (and where each piece stands)

```
BROM ──128KiB──▶ SPL (DRAM init) ──▶ FIT{ BL31 + U-Boot + DTB } ──▶ BL31(EL3) ──▶ U-Boot ──▶ kernel
        ✅ boots        ✅ runs on HW          ✅ (with A523 BL31)      ✅ runs      ✅ runs    ❌ not reached
```

### SPL / DRAM — works
`trimui-tg5050_defconfig` (avaota-a1 base + this board's DRAM params + `AXP@0x34`,
`CONFIG_AXP_DCDC3_VOLT=1100`, LPDDR4 1.10 V). The SPL runs on real silicon and
inits DRAM (first seen via FEL: `Executing the SPL... done.`).

### The 128 KiB SD-boot layout — solved, with a caveat
Writing the *combined* `u-boot-sunxi-with-spl.bin` at 128 KiB does **not** work as-is:
`board_spl_mmc_get_uboot_raw_sector()` (arch/arm/mach-sunxi/board.c) is supposed to
add `(128-8)*2` sectors when it detects the "high" boot, but for the very-new A523
that auto-shift doesn't land, so the SPL looks for U-Boot at the 8 KiB-layout sector
(empty) → drops to FEL. **Fix used:** patch the function to `return sector;`
(deterministic), set `CONFIG_SYS_MMCSD_RAW_MODE_U_BOOT_SECTOR=0x200` (sector 512),
and write **`sunxi-spl.bin` @128 KiB + `u-boot.itb` @256 KiB separately** (GPT, boot
partition ≥ 2 MiB). With that, the SPL reliably loads U-Boot.

### BL31 — the wrong SoC was the hang; **fixed**
Mainline TF-A has **no A523 platform** (only ≤ H616), so our first BL31 was a
`sun50i_h616` stand-in. A wrong-SoC BL31 runs at EL3 right after the SPL and **hung
there** — every 128 KiB attempt went black/FEL. Building a real **`sun55i_a523`**
BL31 from **jernejsk/arm-trusted-firmware, branch `a523-v4`** and folding it into the
FIT **got us past the hang — U-Boot now runs.** (This is the single most important fix.)

### U-Boot proper — runs, but its **MMC driver fails on the A523** (current blocker)
With the A523 BL31, U-Boot runs but cannot boot the kernel. Diagnosed **without UART**
via a "printf-through-SD" trick: a bootcmd that writes a marker byte to an unused SD
sector at each step (reached-bootcmd / Image-loaded / DTB-loaded / pre-booti). The
marker read back as **`0x00`** → U-Boot never completed even an `mmc write`. So the
**full MMC/DM driver hangs/fails** while the SPL's minimal raw-sector read works (which
is why the SPL loads U-Boot fine). This also explains earlier flakiness: with the eMMC
enabled, probing that controller crashes U-Boot → warm reset → **eMMC stock boots**
(seen as `18d1:d002 "TRIMUI ADB"`); with the eMMC disabled in the U-Boot DT
(`&mmc2 { status="disabled"; }`) it just hangs → black. Confirmed U-Boot *runs* because
disabling the eMMC changed the outcome (no more stock boot).

Things ruled out along the way: ext4-vs-FAT boot partition (both fail the same),
distro/bootstd vs a hardcoded `fatload…booti` (both fail), Volume− combo (no effect).

## FEL — works, but full-speed only
Enter FEL from stock with `adb reboot efex`; SoC id `0x1890 = A523`. Distro
`sunxi-tools` is too old (no A523 SRAM profile) — build from upstream master. **But
FEL enumerates `bcdUSB 1.10` / full-speed** (64-byte bulk) and every bulk *data*
transfer fails `usb_bulk_send -7`, independent of cable/port (stock adb does
high-speed fine over the same port). So FEL can't move the U-Boot/kernel payloads;
it was only ever useful for the tiny `version`/`sid` probes.

## Paths forward

1. **Boot our kernel via the VENDOR U-Boot (highest odds).** KNULLI ships the vendor
   `boot0.img` (@128 KiB) + `boot_package.fex` (@16 MiB) — a U-Boot whose **MMC works**.
   Its env: `boot_normal=sunxi_flash read 44000000 boot; bootm 44000000`. So: keep the
   vendor boot0/boot_package, and put our mainline kernel where it boots from — either
   package Image+DTB+initramfs as an **Android boot.img** in the `boot` partition, or
   ship a **custom `env.img`** whose bootcmd loads our kernel. This sidesteps the broken
   mainline-U-Boot MMC entirely, reusing a proven chain. (KNULLI board:
   `knulli-cfw/knulli-linux : board/allwinner/a527/trimui-smart-pro-s`.)
2. **SPL Falcon mode** (`CONFIG_SPL_OS_BOOT`): the mainline SPL loads kernel+DTB from
   raw sectors with its *working* read and jumps straight to the kernel, bypassing
   U-Boot. Real, but fiddly and still blind.
3. **Fix the A523 U-Boot MMC/DM driver** — the clean upstream fix; wants **UART**
   (PB9/PB10) to see where MMC init hangs.

## Status

| Piece | State |
| :-- | :-- |
| SD boot @128 KiB / GPT | ✅ (KNULLI-proven; our SPL boots) |
| SPL + DRAM init | ✅ runs on silicon |
| 128 KiB SPL→U-Boot sector | ✅ fixed (deterministic + separate SPL/FIT) |
| A523 BL31 (jernejsk `a523-v4`) | ✅ built — **cleared the BL31 hang** |
| U-Boot proper | ✅ runs, ❌ **MMC driver fails** ← current blocker |
| First lit pixel | pending — next: boot via the vendor U-Boot |

Note: KNULLI is vendor-BSP (Linux 5.15.147, `CONFIG_ARCH_SUN55IW3`/`AW_*`) — not a
mainline kernel to reuse, but the reference for the *boot* path and hardware bring-up.
