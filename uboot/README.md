<!-- SPDX-License-Identifier: (GPL-2.0-only OR MIT) -->
<!-- Copyright (C) 2026 Midgy BALON -->

# Mainline U-Boot for the Trimui Smart Pro S (Allwinner A523)

Trimui-tuned mainline U-Boot for the TG5050 (A523 / `sun55iw3`). Built on a cross-build
server (`/root/trimui-uboot`).

## What's in this dir
| File | What |
|---|---|
| **`trimui-tg5050_defconfig`** | the board U-Boot defconfig — Avaota-A1 base + this board's LPDDR4 DRAM params (decoded from the vendor `boot0`) + `AXP@0x34` / `CONFIG_AXP_DCDC3_VOLT=1100` (LPDDR4 1.10 V) |
| `DRAM-PARAMS.md` | how the DRAM params were decoded from the vendor `boot0` |
| `DRAM-VALIDATION.md` | DRAM parameter validation notes |

**Binaries are intentionally not committed.** The full patched U-Boot **build tree** lives on
the fork: **[`MidG971/u-boot` branch `trimui-2026.10`](https://github.com/MidG971/u-boot/tree/trimui-2026.10)**
— rebased onto current U-Boot mainline (the merged 2026.10 A523 base) with 4 patches on top:
this defconfig, the SD mux-source (`src 2`) clock, the 128 KiB deterministic raw-sector, and an
eMMC-off bring-up hack.

## Build recipe
```bash
export CROSS_COMPILE=aarch64-linux-gnu-

# 1) TF-A BL31 — mainline TF-A still has NO A523 platform; use Jernej Škrabec's a523 branch
git clone https://github.com/jernejsk/arm-trusted-firmware.git tfa-a523
cd tfa-a523 && git checkout a523-v4
make -j"$(nproc)" PLAT=sun55i_a523 DEBUG=0 bl31
#   -> build/sun55i_a523/release/bl31.bin   (the real A523 BL31 is what cleared the EL3 hang)

# 2) U-Boot — the trimui-2026.10 branch (current mainline + our 4 patches)
git clone https://github.com/MidG971/u-boot.git -b trimui-2026.10 u-boot
cd u-boot && make trimui-tg5050_defconfig
./scripts/config --set-val CONFIG_SYS_MMCSD_RAW_MODE_U_BOOT_SECTOR 0x200 && make olddefconfig
make -j"$(nproc)" BL31=/root/trimui-uboot/tfa-a523/build/sun55i_a523/release/bl31.bin
#   -> spl/sunxi-spl.bin + u-boot.itb   (write SPL @128 KiB, u-boot.itb @256 KiB on a GPT card)
# deps: swig python3-dev libssl-dev libgnutls28-dev device-tree-compiler bc
```

## Status (see [`../docs/BOOT-AND-FEL-NOTES.md`](../docs/BOOT-AND-FEL-NOTES.md) for the full story)
- ✅ **SPL runs + inits DRAM** on real silicon (this board's own DRAM params).
- ✅ **Real `sun55i_a523` BL31** (jernejsk `a523-v4`) cleared the EL3 hang → **U-Boot runs**.
- ✅ **128 KiB SD-boot** layout solved (deterministic raw-sector + separate SPL/FIT on GPT).
- ❌ **Doesn't reach the kernel yet** — the MMC clock/gate/reset is now verified correct against
  the live vendor registers, so the suspect is the **init-time MMC probe** or the **BL31→U-Boot
  handoff**; a **UART** on PB9/PB10 is needed to pin it down.

## FEL (brick-safe, but full-speed-only)
`sunxi-fel` supports the A523 (SoC id `0x1890`); enter FEL from stock with `adb reboot efex`.
Caveat: on this device FEL enumerates **full-speed** and bulk *data* transfers fail (`-7`), so
FEL can't move the U-Boot/kernel payloads — it's only useful for the tiny `version`/`sid`
probes. Boot from **microSD** instead (SPL @128 KiB on a GPT card).

## Historical note — old caveats now RESOLVED
The original 2026-06-04 reference build used the Avaota-A1 config + an **H616 stand-in BL31**
and a placeholder PMIC address (`0x35`). All three are fixed above (real board DRAM params,
`AXP@0x34`, real A523 BL31) — don't follow the old avaota/H616 recipe.
