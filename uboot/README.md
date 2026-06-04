<!-- SPDX-License-Identifier: (GPL-2.0-only OR MIT) -->
<!-- Copyright (C) 2026 Midgy BALON -->

# Mainline U-Boot for Allwinner A523 — reference build (2026-06-04)

Built on a cross-build server (`/root/trimui-uboot`) while the
Trimui Smart Pro S was in transit. This is a **reference A523 build** to validate the
toolchain and the FEL boot path — it is **not yet Trimui-tuned** (see Caveats).

## Artifacts (in this dir)
| File | Size | What |
|---|---|---|
| **`u-boot-sunxi-with-spl-trimui.bin`** | 816 KB | **Trimui-tuned** image — DRAM params from the device's own boot0 (see `DRAM-PARAMS.md`). **Use this one for FEL.** |
| `u-boot-sunxi-with-spl.bin` | 816 KB | Avaota-A1 reference image (generic A523 DRAM) — kept for comparison |
| `trimui-tg5050_defconfig` | — | the Trimui defconfig (Avaota + 5 DRAM overrides + PMIC@0x34) |
| `sunxi-spl.bin` | 48 KB | SPL only (from reference build) |
| `u-boot.bin` | 726 KB | U-Boot proper |
| `bl31.bin` | 40 KB | TF-A PSCI (built from `sun50i_h616` platform — see Caveats) |
| `vendor-boot0/` | — | carved vendor boot0 the DRAM params came from |

**DRAM is now retargeted** — caveat #1 below is resolved by `u-boot-sunxi-with-spl-trimui.bin`
+ `DRAM-PARAMS.md`. Build it on the server with `make trimui-tg5050_defconfig` instead of avaota.

## Exact recipe
```bash
# Toolchain: aarch64-linux-gnu-gcc 12.2 (Debian)
export CROSS_COMPILE=aarch64-linux-gnu-

# 1) TF-A BL31 — mainline TF-A has NO A523 platform; use H616 as stand-in
git clone --depth1 https://github.com/ARM-software/arm-trusted-firmware.git tfa   # da738d5
cd tfa && make -j$(nproc) PLAT=sun50i_h616 DEBUG=0 bl31
#   -> build/sun50i_h616/release/bl31.bin

# 2) U-Boot (master, VERSION=2026; A523 landed in v2025.10)                       # a4c8728f
git clone --depth1 https://github.com/u-boot/u-boot.git u-boot
cd u-boot && make avaota-a1_defconfig
make -j$(nproc) BL31=/root/trimui-uboot/tfa/build/sun50i_h616/release/bl31.bin
#   -> u-boot-sunxi-with-spl.bin
# deps: swig python3-dev libssl-dev libgnutls28-dev device-tree-compiler bc
```

## FEL boot (no eMMC writes — brick-safe first boot)
`sunxi-fel` on the workstation already supports the A523 (SoC id 0x1890). With the
device in FEL mode over USB-C:
```bash
sunxi-fel -v uboot /path/to/u-boot-sunxi-with-spl.bin
# watch the serial console (ttyS0) for the U-Boot prompt
```

## ⚠️ Caveats — must retarget before it will reliably boot the Trimui
1. **DRAM timings are Avaota-A1's**, not the Trimui's. The `CONFIG_DRAM_SUNXI_*`
   values in `avaota-a1_defconfig` are board-specific; if the Trimui's DRAM differs,
   **SPL DRAM init can hang** → no boot. **Next step: extract the Trimui's real DRAM
   parameters from the vendor `boot0`** (present in the stock firmware image) and make
   a `trimui_*_defconfig`. This is the most likely first-boot blocker.
2. **PMIC mismatch.** This config uses `CONFIG_AXP717_POWER`, `AXP_I2C_ADDRESS=0x35`,
   DCDC2=920mV/DCDC3=1160mV (Avaota). The Trimui PMIC is at **0x34** and the vendor
   drives it as **axp2202** (see `../FIRMWARE-FINDINGS.md`). SPL sets DRAM/CPU voltage
   via the PMIC, so the address/driver/voltages must be corrected after the on-device
   i2c ID check (`../recon.sh` §3).
3. **BL31 is the H616 platform** (mainline TF-A has no A523 platform yet). Fine for
   reaching the U-Boot/serial console; full 8-core SMP/PSCI may need a real A523 TF-A
   platform later. A523 dual-cluster CPU_ON may be partial under H616 bl31.
4. The embedded control DTB is `sun55i-t527-avaota-a1`; irrelevant to bring-up (the
   Linux kernel DTB is loaded separately — our `dts/sun55i-a523-trimui-smart-pro-s`).

**Bottom line:** the build pipeline works and the image is well-formed. It may even
reach U-Boot on the Trimui as-is, but plan on retargeting DRAM (#1) + PMIC (#2) before
counting on it. Server tree kept at `/root/trimui-uboot` for fast rebuilds.
