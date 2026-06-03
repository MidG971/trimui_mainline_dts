# Allwinner A523 / Trimui Smart Pro S — Mainline Device Tree

> ## 🚧 WORK IN PROGRESS — not a working port yet
> This is an **early bring-up effort**, not production firmware. The device tree
> here targets what mainline Linux can actually run **today**; large parts of the
> hardware depend on SoC drivers that **do not exist in mainline yet** (see below).
> A clean `dtc` compile only proves the syntax and phandles are valid — it does
> **not** mean the hardware works. Anything not marked 🟢 is unverified or blocked.

Mainline device tree for the **Trimui Smart Pro S** retro-gaming handheld
(board `A523-PRO2-AXP717C`, model TG5050), based on the Allwinner **A523**
(`sun55iw3p1`). Built on the upstream `sun55i-a523.dtsi`, modeled on the
upstream `sun55i-t527-avaota-a1` board (same SoC family).

## Status (honest)

The current DTS is **Phase 2**: serial console, storage, USB, PMIC + regulators,
and the WiFi SDIO slot. It **compiles against mainline Linux 6.19** and produces a
valid DTB. Everything else is on the roadmap in [`PORTING-NOTES.md`](PORTING-NOTES.md).

| Component | Status | Notes |
| :--- | :---: | :--- |
| 8x Cortex-A55 (boot) | 🟢 | Boots at bootloader voltage; per-cluster cpufreq/DVFS not wired upstream yet |
| AXP717 PMIC + regulators | 🟡 | On `r_i2c0`. Vendor fw drives it as **axp2202**; silk says AXP717C — read i2c ID@0x34 on HW to settle |
| Storage: microSD / eMMC | 🟡 | Nodes + supplies set; rails tagged VERIFY |
| USB host / OTG | 🟡 | Controllers enabled; VBUS/ID GPIOs need HW verification |
| WiFi/BT (SDIO, mmc1) | 🟠 | **Chip identified: AICSemi AIC8800** (D80/DC) from vendor fw; out-of-tree module, no `wifi@` child yet |
| Battery / charger | 🟠 | **5000 mAh design, 1000 mA charge** (from vendor DTB); OCV table still needs porting |
| MIPI-DSI display (4-lane) | 🔴 | **No mainline A523 DSI/DE driver** — blocked |
| PWM backlight | 🔴 | **No mainline A523 PWM** — blocked |
| Analog joysticks (GPADC) | 🔴 | **No mainline A523 GPADC** — blocked (two controllers: gpadc0+gpadc1) |
| Audio codec | 🔴 | **No mainline A523 codec** — blocked |
| Gamepad / buttons | 🟠 | Refined: power = **AXP2202 PEK**, volume = **LRADC** (`keyboard_1350mv`, 3 keys), main pad via userspace `trimui_inputd` — *not* a pure USB MCU |
| Vibrator / fan (PWM) | 🔴 | Channels known (ch7 / ch10) but blocked on PWM driver |
| GPU (Mali) / VPU | 🔴 | Not in mainline |

Legend: 🟢 works · 🟡 present, needs HW verification · 🟠 partial/stubbed · 🔴 blocked on missing mainline driver.

Many of the above were pinned down **before the hardware arrived** by mining the
stock firmware — see [`FIRMWARE-FINDINGS.md`](FIRMWARE-FINDINGS.md), whose
*Methodology* section documents exactly how each fact was obtained, plus
[`recon.sh`](recon.sh), the read-only on-device collector for the remaining facts.

## How to compile

```bash
./compile.sh
```
The script preprocesses with the cross GCC and compiles with `dtc`. For a real
mainline build, drop `dts/sun55i-a523-trimui-smart-pro-s.dts` into
`arch/arm64/boot/dts/allwinner/`, add it to that directory's `Makefile`, and run
`make ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- dtbs`.

## Reference

- [`PORTING-NOTES.md`](PORTING-NOTES.md) — hardware truth table extracted from the
  vendor DTB + phased roadmap + the "verify-this-first" checklist.
- [`FIRMWARE-FINDINGS.md`](FIRMWARE-FINDINGS.md) — facts mined from the stock
  firmware before the device arrived (AIC8800 WiFi, 5000 mAh battery, LRADC keys,
  `adb`/serial shell access), **and a Methodology section on how each was obtained**.
- [`recon.sh`](recon.sh) — read-only day-1 collector to run on the stock OS
  (`adb shell`) to capture the residual hardware-only facts (i2c chip IDs, CPU
  regulator, AIC8800 variant, gamepad source, partition map).
</content>

## Star History

<a href="https://www.star-history.com/?repos=trimui_mainline_dts%2Ftrimui_mainline_dts&type=date&legend=top-left">
 <picture>
   <source media="(prefers-color-scheme: dark)" srcset="https://api.star-history.com/chart?repos=trimui_mainline_dts/trimui_mainline_dts&type=date&theme=dark&legend=top-left" />
   <source media="(prefers-color-scheme: light)" srcset="https://api.star-history.com/chart?repos=trimui_mainline_dts/trimui_mainline_dts&type=date&legend=top-left" />
   <img alt="Star History Chart" src="https://api.star-history.com/chart?repos=trimui_mainline_dts/trimui_mainline_dts&type=date&legend=top-left" />
 </picture>
</a>
