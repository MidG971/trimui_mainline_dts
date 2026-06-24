<!-- SPDX-License-Identifier: (GPL-2.0-only OR MIT) -->
<!-- Copyright (C) 2026 Midgy BALON -->

# Allwinner A523 / Trimui Smart Pro S — Mainline Device Tree

> ## 🚧 WORK IN PROGRESS — not a working port yet
> This is an **early bring-up effort**, not production firmware. The device tree
> here targets what mainline Linux can actually run **today**; large parts of the
> hardware depend on SoC drivers that **do not exist in mainline yet** (see below).
> A clean `dtc` compile only proves the syntax and phandles are valid — it does
> **not** mean the hardware works. Anything not marked 🟢 is unverified or blocked.

> ## ⚠️ Disclaimer — experimental, use at your own risk
> **Everything in this repository is experimental and, as of now, completely untested on
> real hardware.** The device tree, the U-Boot defconfig, the DRAM parameters extracted
> from the vendor firmware, and every procedure described here are unverified and may be
> wrong. Flashing, FEL-booting, or otherwise running custom firmware on your device **can
> permanently brick it**, corrupt data, or damage hardware. **You use this entirely at your
> own risk.** The authors and contributors accept **no liability** for any damage, data
> loss, or bricked devices, and provide everything here **with no warranty of any kind**.
> Always back up your stock firmware first, and prefer the brick-safe FEL (RAM-only) boot
> path until things are validated.

Mainline device tree for the **Trimui Smart Pro S** retro-gaming handheld
(board `A523-PRO2-AXP717C`, model TG5050), based on the Allwinner **A523**
(`sun55iw3p1`). Built on the upstream `sun55i-a523.dtsi`, modeled on the
upstream `sun55i-t527-avaota-a1` board (same SoC family).

## Status

The board DTS targets **mainline Linux v7.1**. The base — serial console, storage
(microSD/eMMC), USB2, PMIC + regulators, RTC, the LEDC RGB array, the WiFi SDIO slot
(power/reset sequencing) + BT UART, the Mali GPU (Panfrost, already upstream), and the
PWM fan/vibrator — builds against mainline and produces a valid, `dt-validate`-clean
DTB. The MIPI-DSI display, PWM backlight and audio rely on **drivers not yet upstream**;
those ship as a patch series + driver sources under [`kernel/`](kernel/) (build +
dt-validate clean, **unverified on hardware**). Roadmap in [`PORTING-NOTES.md`](PORTING-NOTES.md).

| Component | Status | Notes |
| :--- | :---: | :--- |
| 8x Cortex-A55 (boot) | 🟢 | Boots at bootloader voltage; per-cluster cpufreq/DVFS not wired upstream yet — draft OPP tables in [`dts/staging/trimui-cpu-opp.dtsi`](dts/staging/trimui-cpu-opp.dtsi) (blocked on the A523 CPU clock not being exposed upstream + the tcs4838 big-cluster regulator driver) |
| AXP717 PMIC + regulators | 🟡 | On `r_i2c0`. Vendor fw drives it as **axp2202**; silk says AXP717C — read i2c ID@0x34 on HW to settle |
| Storage: microSD / eMMC | 🟡 | Nodes + supplies set; rails tagged VERIFY |
| USB2 host / OTG | 🟡 | Controllers enabled; VBUS/ID GPIOs need HW verification |
| USB3 (Type-C SS) | 🟠 | Board has USB3 host (vendor dwc3 + GMA340 SuperSpeed mux); drafted in [`dts/staging/`](dts/staging/) pending the in-flight A523 USB3 series upstream |
| WiFi/BT (SDIO, mmc1) | 🟠 | **Chip = AICSemi AIC8800** (D80/DC); power/reset sequencing wired (`wifi_pwrseq`, BT on UART1 w/ RTS-CTS). Needs the out-of-tree aic8800 module |
| Battery / charger | 🟠 | **5000 mAh design, 1000 mA charge** (from vendor DTB); OCV table still needs porting |
| MIPI-DSI display (4-lane) | 🟡 | DSI host + combo-PHY + TCON-LCD + panel drivers written (`kernel/`), dt-validate clean; the **DE3.5 mixer/CRTC** (lit pixel) is the remaining blocker. Not upstream; needs HW — see [`docs/DISPLAY-PORT-STATUS.md`](docs/DISPLAY-PORT-STATUS.md) |
| PWM backlight | 🟡 | PWM driver ported (`kernel/patches/`, `pwm-sun20i`); `pwm-backlight` wired. Builds + dt-validates; needs HW |
| Analog joysticks (GPADC) | 🟠 | GPADC lands **mainline v7.2**; `adc-joystick` nodes drafted in [`dts/staging/`](dts/staging/) (vendor: gpadc0+gpadc1, 2 ch each) |
| Audio codec | 🟡 | ASoC driver + DT integration done (`kernel/`, `sun55i-codec`): playback + capture, mixer controls, DAPM, self-registered card, `audio-routing` + speaker amp (PH6), **headset/HMIC jack detection** (IRQ-driven; buttons→keys). Builds (W=1) + dt-validates clean; detection thresholds + on-device tuning need HW — see [`docs/AUDIO-CODEC-NOTES.md`](docs/AUDIO-CODEC-NOTES.md) |
| Side keys (LRADC) | 🟡 | `lradc@2009800` + 3 keys (Home / Vol±) wired (`sun4i-lradc-keys`); vref + voltages need HW calibration |
| Gamepad / buttons | 🟠 | Power = **AXP2202 PEK**; main D-pad/ABXY via userspace `trimui_inputd` — *not* a pure USB MCU |
| Vibrator / fan (PWM) | 🟡 | `pwm-vibrator` (ch7) + `pwm-fan` (ch10, inverted, cooling-device) nodes added; needs HW |
| Thermal (THS) | 🔴 | In-flight upstream (A523 THS series, not yet merged); the `pwm-fan` cooling-map hooks the zones once it lands |
| GPU (Mali-G57) | 🟡 | **Upstream** (Panfrost, Valhall-JM); `&gpu` **enabled** in board DTS (`mali-supply` = AXP2202 dcdc2). Optional DVFS OPP table (150–600 MHz, compiles today) in [`dts/staging/trimui-gpu-opp.dtsi`](dts/staging/trimui-gpu-opp.dtsi). Needs HW — see [`docs/GPU-NOTES.md`](docs/GPU-NOTES.md) |
| VPU | 🔴 | Not in mainline |

Legend: 🟢 works · 🟡 present, needs HW verification · 🟠 partial/stubbed · 🔴 blocked on missing mainline driver.

Many of the above were pinned down **before the hardware arrived** by mining the
stock firmware — see [`FIRMWARE-FINDINGS.md`](FIRMWARE-FINDINGS.md), whose
*Methodology* section documents exactly how each fact was obtained, plus
[`recon.sh`](recon.sh), the read-only on-device collector for the remaining facts.

## Kernel side (out-of-tree, not upstream)

The display/PWM bring-up that mainline lacks lives under [`kernel/`](kernel/):
- [`kernel/patches/`](kernel/patches/) — the `0001`–`0011` series (git-format-patch,
  checkpatch-clean, `Signed-off-by`): DSI host variant, combo-PHY Kconfig, SoC display
  nodes, TCON-LCD compat, PWM node, PWM driver wiring, panel, DE33 mixer cfg, codec
  (driver wiring + DT node), LRADC side keys.
- [`kernel/drivers/`](kernel/drivers/) — new sources: `phy-sun55i-dsi-combo.c`,
  `pwm-sun20i.c`, `panel-trimui-smart-pro-s.c`, `sun55i-codec.c`.
- [`kernel/bindings/`](kernel/bindings/) — DT bindings (combo-PHY, panel, PWM, codec).
- [`kernel/build-trimui-kernel.sh`](kernel/build-trimui-kernel.sh) — one-shot: apply the
  series + drop the drivers + build Image/dtbs/modules on a v7.1 tree.

Submission status in [`docs/UPSTREAM-READINESS.md`](docs/UPSTREAM-READINESS.md).
Drafts gated on upstream work (GPADC sticks, USB3) live in [`dts/staging/`](dts/staging/).

## How to compile

```bash
./compile.sh
```
The script preprocesses with the cross GCC and compiles with `dtc` (DTB only). For
the full kernel build — apply the `kernel/patches/` series, drop in the driver
sources, wire the board DTS + panel, and build `Image`/`dtbs`/modules on a v7.1
tree — use [`kernel/build-trimui-kernel.sh`](kernel/build-trimui-kernel.sh).

## Reference

- [`PORTING-NOTES.md`](PORTING-NOTES.md) — hardware truth table extracted from the
  vendor DTB + phased roadmap + the "verify-this-first" checklist.
- [`FIRMWARE-FINDINGS.md`](FIRMWARE-FINDINGS.md) — facts mined from the stock
  firmware before the device arrived (AIC8800 WiFi, 5000 mAh battery, LRADC keys,
  `adb`/serial shell access), **and a Methodology section on how each was obtained**.
- [`recon.sh`](recon.sh) — read-only day-1 collector to run on the stock OS
  (`adb shell`) to capture the residual hardware-only facts (i2c chip IDs, CPU
  regulator, AIC8800 variant, gamepad source, partition map).
- [`docs/`](docs/) — deep-dives: [`GPU-NOTES.md`](docs/GPU-NOTES.md) (Mali-G57 /
  Panfrost), [`DISPLAY-PORT-STATUS.md`](docs/DISPLAY-PORT-STATUS.md),
  [`DE35-NOTES.md`](kernel/DE35-NOTES.md), [`UPSTREAM-READINESS.md`](docs/UPSTREAM-READINESS.md),
  [`HARDWARE-BRINGUP.md`](docs/HARDWARE-BRINGUP.md) (day-1 runbook), and the
  [`A523-DOCS-INDEX.md`](docs/A523-DOCS-INDEX.md) / [`BOARD-PINMAP.md`](docs/BOARD-PINMAP.md).
- [`uboot/`](uboot/) — mainline U-Boot bring-up for the A523:
  [`trimui-tg5050_defconfig`](uboot/trimui-tg5050_defconfig) (Avaota-A1 base + the
  DRAM params extracted from this board's vendor boot0 + PMIC@0x34),
  [`DRAM-PARAMS.md`](uboot/DRAM-PARAMS.md) (how the LPDDR4 timings were decoded), and
  [`README`](uboot/README.md) (build recipe, FEL boot, and caveats). **Prebuilt binaries
  are intentionally not committed** — build from the defconfig, or grab a Release once
  the image is hardware-validated.

## License

Copyright (C) 2026 Midgy BALON. Dual-licensed **`GPL-2.0-only OR MIT`** (your
choice) — matching the Linux kernel convention for device trees, so the board
DTS can be upstreamed cleanly while staying permissively reusable. Each file
carries an SPDX tag; full texts are in [`LICENSES/`](LICENSES/), and
[`NOTICE`](NOTICE) documents provenance (original work vs. the unmodified
upstream `sun55i-a523.dtsi` from Arm Ltd, vs. factual hardware data observed
from the vendor firmware). The proprietary vendor firmware and the decompiled
vendor device tree are **not** included here.

## Star History

<a href="https://www.star-history.com/?repos=trimui_mainline_dts%2Ftrimui_mainline_dts&type=date&legend=top-left">
 <picture>
   <source media="(prefers-color-scheme: dark)" srcset="https://api.star-history.com/chart?repos=trimui_mainline_dts/trimui_mainline_dts&type=date&theme=dark&legend=top-left" />
   <source media="(prefers-color-scheme: light)" srcset="https://api.star-history.com/chart?repos=trimui_mainline_dts/trimui_mainline_dts&type=date&legend=top-left" />
   <img alt="Star History Chart" src="https://api.star-history.com/chart?repos=trimui_mainline_dts/trimui_mainline_dts&type=date&legend=top-left" />
 </picture>
</a>
