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

### CORRECTION (2026-07) — the MMC *clock* is verified correct; the blocker is reframed
The "MMC driver fails" conclusion above needs a caveat. We later captured the vendor's
**live MMC register state** (read from the running stock OS via `/sys/class/sunxi_dump`,
which bypasses `STRICT_DEVMEM`) and checked our U-Boot's A523 MMC **clock / gate / reset**
path against it register-for-register: `SMHC0_CLK`, the mux source (vendor uses
PLL_PERIPH0_600M / **src 2**, not the mainline src 1 which reads back gated), the new-mode
divider, `SAMP_DL`, and the `0x84c` bus-gate/reset — **all correct**. So the SD MMC *clock*
is **not** the blocker (our `src2` U-Boot patch already matches the vendor).

Re-reading U-Boot's init order also explains the `0x00` marker: `mmc_initialize()` runs at
**init time, before `console_init_r()` and before the bootcmd** — so if U-Boot hangs in the
init-time MMC probe, the bootcmd marker-byte code never runs (nothing to read back). The real
open question is therefore **where U-Boot dies before the prompt** — the init-time MMC probe,
or the **BL31→U-Boot (EL3→EL2) handoff** — which only a **UART** can show. Next session leads
with the handoff, not the MMC clock.

## FEL — works, but full-speed only
Enter FEL from stock with `adb reboot efex`; SoC id `0x1890 = A523`. Distro
`sunxi-tools` is too old (no A523 SRAM profile) — build from upstream master. **But
FEL enumerates `bcdUSB 1.10` / full-speed** (64-byte bulk) and every bulk *data*
transfer fails `usb_bulk_send -7`, independent of cable/port (stock adb does
high-speed fine over the same port). So FEL can't move the U-Boot/kernel payloads;
it was only ever useful for the tiny `version`/`sid` probes.

## Paths forward

1. **Boot our kernel via the VENDOR U-Boot — explored, blocked by the DTB.** KNULLI ships
   the vendor `boot0.img` (@128 KiB) + `boot_package.fex` (@16 MiB) — a U-Boot 2018.07 whose
   **MMC works**, with `booti`/`fatload`/`ext4load` — it boots an **Android boot.img v0**
   (kernel@0x40080000, ramdisk@0x42000000, tags@0x40000100, page 2048, `sun55i_arm64`).
   Two blockers, both confirmed on-device: (a) it **ignores a custom `env.img`** on the env
   partition (its env is baked into the package, not SD-writable — a marker-byte bootcmd
   read back `0x00`), so we can't change its bootcmd; (b) it loads the **vendor BSP DTB**
   (`sun55iw3p1-soc-system.dtb`, a `dtb` item in the boot_package **TOC/toc1**) and passes
   that to the kernel — a mainline kernel can't use the BSP DTB. To use this chain we'd have
   to **repack the boot_package TOC**, swapping in our mainline DTB (deep, blind). The
   OrangePi-4A board uses a *different* vendor U-Boot config (extlinux + a separate `.dtb`,
   boot0@8 KiB), which is cleaner, but its `boot_package` is board-specific.
   (KNULLI board: `knulli-cfw/knulli-linux : board/allwinner/a527/{trimui-smart-pro-s,orangepi-4a}`.)
2. **SPL Falcon mode** (`CONFIG_SPL_OS_BOOT`): the mainline SPL loads kernel+DTB from
   raw sectors with its *working* read and jumps straight to the kernel, bypassing
   U-Boot. Real, but fiddly and still blind.
3. **Get a UART on PB9/PB10 and see where U-Boot dies** — the clean path. Since the MMC
   clock/gate/reset is verified correct (see the correction above), the UART is to catch the
   init-time MMC probe or the BL31→U-Boot handoff, *not* to re-check the clock.

## Status

| Piece | State |
| :-- | :-- |
| SD boot @128 KiB / GPT | ✅ (KNULLI-proven; our SPL boots) |
| SPL + DRAM init | ✅ runs on silicon |
| 128 KiB SPL→U-Boot sector | ✅ fixed (deterministic + separate SPL/FIT) |
| A523 BL31 (jernejsk `a523-v4`) | ✅ built — **cleared the BL31 hang** |
| U-Boot proper | ✅ runs; ❌ **doesn't reach kernel** ← blocker (MMC clock verified OK; suspect init-time probe / BL31→U-Boot handoff — needs UART) |
| First lit pixel | pending — next: boot via the vendor U-Boot |

**U-Boot tree:** our working tree is rebased onto **current U-Boot mainline** (the merged
2026.10 A523 base) and pushed to
**[`MidG971/u-boot` branch `trimui-2026.10`](https://github.com/MidG971/u-boot/tree/trimui-2026.10)** —
4 patches on top: the Trimui board defconfig, the SD mux-source (`src 2`) clock, the 128 KiB
deterministic raw-sector, and an eMMC-off bring-up hack. That branch is what the UART session boots.

Note: KNULLI is vendor-BSP (Linux 5.15.147, `CONFIG_ARCH_SUN55IW3`/`AW_*`) — not a
mainline kernel to reuse, but the reference for the *boot* path and hardware bring-up.
