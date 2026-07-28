<!-- SPDX-License-Identifier: (GPL-2.0-only OR MIT) -->
<!-- Copyright (C) 2026 Midgy BALON -->

# Allwinner A523 / Trimui Smart Pro S — Mainline Linux

Mainline Linux bring-up for the **Trimui Smart Pro S** retro-gaming handheld
(board `A523-PRO2-AXP717C`, model TG5050), based on the Allwinner **A523**
(`sun55iw3p1`) — built on the upstream `sun55i-a523.dtsi` and modeled on the
`sun55i-t527-avaota-a1` board (same SoC family).

📖 **Full documentation lives in the [Wiki](https://github.com/MidG971/trimui_mainline_dts/wiki)** —
[Roadmap](https://github.com/MidG971/trimui_mainline_dts/wiki/Roadmap) ·
[Status](https://github.com/MidG971/trimui_mainline_dts/wiki/Status) ·
[Hardware Overview](https://github.com/MidG971/trimui_mainline_dts/wiki/Hardware-Overview) ·
[Bring-Up Runbook](https://github.com/MidG971/trimui_mainline_dts/wiki/Bring-Up-Runbook) ·
[Building](https://github.com/MidG971/trimui_mainline_dts/wiki/Building)

> ## ⚠️ Experimental — use at your own risk
> This is an **active bring-up effort**, not production firmware, and is **largely
> unvalidated on hardware**. The device tree, U-Boot defconfig, and DRAM parameters
> may be wrong. Flashing or FEL-booting custom firmware **can permanently brick your
> device**, corrupt data, or damage hardware. **No warranty, no liability — you use
> this entirely at your own risk.** Back up your stock firmware first. Booting from
> microSD leaves the eMMC untouched (pull the card to return to stock), and the vendor
> `trimui_tg5050.awing` factory-restore is the brick lifeline.

## Where things stand

**The device is in hand and on-device bring-up is underway.** The board DTS targets
**mainline Linux v7.2**; the base — serial console, microSD/eMMC, USB2, PMIC +
regulators, RTC, the LEDC RGB array, WiFi-SDIO sequencing + BT UART, and the Mali
GPU (Panfrost) — builds against mainline and is `dt-validate`-clean. The MIPI-DSI
display, PWM backlight and audio ship as an out-of-tree patch series under
[`kernel/`](kernel/) (build + dt-validate clean).

**Boot bring-up (current focus).** On real silicon so far:

- The A523 BROM **boots the microSD from 128 KiB + GPT** (no button combo) — proven
  by KNULLI; our earlier 8 KiB/MBR card was simply at the wrong offset.
- Our mainline **SPL runs and inits DRAM**; a real **`sun55i_a523` BL31** (mainline
  TF-A has none yet — built from Jernej Škrabec's `a523-v4` branch) **clears the EL3
  hang**, and **mainline U-Boot now runs on the A523** — our working tree is the
  [`trimui-2026.10`](https://github.com/MidG971/u-boot/tree/trimui-2026.10) branch of a
  U-Boot fork, rebased on current mainline with the A523 bring-up patches.
- **Current blocker:** U-Boot's **A523 MMC/DM driver hangs** before it can load the
  kernel (the SPL's minimal read works, U-Boot proper's full driver doesn't). This is
  why the display/audio/etc. aren't silicon-verified yet. Fixing (or seeing) it wants
  a **UART** on PB9/PB10.

The full journey + diagnosis, the 128 KiB SPL sector fix, and the vendor-chainload
dead-end are in **[`docs/BOOT-AND-FEL-NOTES.md`](docs/BOOT-AND-FEL-NOTES.md)**. KNULLI
(a vendor-BSP CFW) boots this board and doubles as the reference boot chain.

👉 Per-subsystem detail in the **[Status page](https://github.com/MidG971/trimui_mainline_dts/wiki/Status)**;
the plan in the **[Roadmap](https://github.com/MidG971/trimui_mainline_dts/wiki/Roadmap)**
(deep mainline bring-up → daily-driver optimization → forward-maintained).

## Help wanted 🙏

More hands are very welcome — especially anyone with the device who can capture
hardware facts or test patches.

- 📣 **Reddit:** [Help wanted — Trimui Smart Pro S mainline kernel](https://www.reddit.com/r/trimui/comments/1ug6411/help_wanted_trimui_smart_pro_s_mainline_kernel/)
- 🛠️ **How to contribute:** [`CONTRIBUTING.md`](CONTRIBUTING.md) ·
  [Code of Conduct](CODE_OF_CONDUCT.md) · [security policy](SECURITY.md)

## Build

```bash
./compile.sh                                   # board DTB only (syntax-level)
./kernel/build-trimui-kernel.sh <v7.2-src>     # full kernel: patches + drivers + dtbs
```

Details and validation gates: [Building](https://github.com/MidG971/trimui_mainline_dts/wiki/Building).

## Repository layout

- [`dts/`](dts/) — the board device tree (`sun55i-a523-trimui-smart-pro-s.dts`) +
  panel; [`dts/staging/`](dts/staging/) holds drafts gated on upstream work.
- [`kernel/`](kernel/) — out-of-tree drivers, the `0001`–`0011` patch series, and
  DT bindings for the not-yet-upstream display/PWM/audio stack.
- [`uboot/`](uboot/) — mainline U-Boot defconfig + the DRAM params decoded from
  this board's vendor boot0. **Binaries are intentionally not committed** — the full
  patched build tree is the [`trimui-2026.10`](https://github.com/MidG971/u-boot/tree/trimui-2026.10)
  branch of our [U-Boot fork](https://github.com/MidG971/u-boot).
- [`docs/`](docs/) — technical deep-dives (display, GPU, codec, pinmap, doc index).
- [`PORTING-NOTES.md`](PORTING-NOTES.md) / [`FIRMWARE-FINDINGS.md`](FIRMWARE-FINDINGS.md) /
  [`recon.sh`](recon.sh) — the hardware truth table, firmware-mined facts, and the
  read-only on-device collector.

## License

Copyright (C) 2026 Midgy BALON. Dual-licensed **`GPL-2.0-only OR MIT`** (your
choice), matching the kernel convention for device trees. Each file carries an
SPDX tag; full texts in [`LICENSES/`](LICENSES/), provenance in [`NOTICE`](NOTICE).
The proprietary vendor firmware and the decompiled vendor device tree are **not**
included here.

## Star History

<a href="https://www.star-history.com/#MidG971/trimui_mainline_dts&Date">
 <picture>
   <source media="(prefers-color-scheme: dark)" srcset="https://api.star-history.com/svg?repos=MidG971/trimui_mainline_dts&type=Date&theme=dark" />
   <source media="(prefers-color-scheme: light)" srcset="https://api.star-history.com/svg?repos=MidG971/trimui_mainline_dts&type=Date" />
   <img alt="Star History Chart" src="https://api.star-history.com/svg?repos=MidG971/trimui_mainline_dts&type=Date" />
 </picture>
</a>
