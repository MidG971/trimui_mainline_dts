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
> This is an **active bring-up effort**, not production firmware. It now **boots mainline
> Linux to a shell on real hardware**, but most peripherals (display, audio, WiFi, gamepad)
> are **not yet validated**, and the device tree may still be wrong. Flashing or FEL-booting
> custom firmware **can permanently brick your
> device**, corrupt data, or damage hardware. **No warranty, no liability — you use
> this entirely at your own risk.** Back up your stock firmware first. Booting from
> microSD leaves the eMMC untouched (pull the card to return to stock), and the vendor
> `trimui_tg5050.awing` factory-restore is the brick lifeline.

## Where things stand

**🎉 Mainline Linux boots on the device — all the way to an interactive shell.** The full
chain works on real silicon: **BROM → SPL → BL31 → U-Boot → distro-boot → Linux (v7.2-rc3)
→ busybox shell** (over a UART on PB9/PB10). Working today: 8× Cortex-A55 **SMP**, **DRAM**
(1 GiB), **microSD/MMC** (the kernel `sunxi-mmc` new-timings path), the **AXP2202/AXP717
PMIC** + regulators, **USB2** host, and the RTC.

**What cracked the months-long boot blocker** — all three were SPL-stage config bugs, all
found the moment a UART gave us eyes, and *none* were what we'd theorized:

1. **DRAM parameters** — the hand-decoded LPDDR4 timings failed training; the values proven
   on the Radxa Cubie A5E (same SoC + LPDDR4) train correctly and auto-size to 1 GiB.
2. **FIT sector** — the SPL read U-Boot from the wrong raw sector (a broken `spl_size`-based
   calc); pinned to the configured sector.
3. **FIT offset** — a stray `+8 KiB` `DATA_PART_OFFSET` overshot the FIT header.

The long-suspected "A523 U-Boot MMC driver" was **never** the problem — boot simply never
reached U-Boot. A real **`sun55i_a523` BL31** (from Jernej Škrabec's `a523-v4` TF-A branch)
+ mainline U-Boot do the rest. The U-Boot side is the
[`trimui-2026.10`](https://github.com/MidG971/u-boot/tree/trimui-2026.10) branch of our fork.

**Display (current focus).** With the DRM stack built in, the whole **DE33 pipeline binds on
hardware**: `card0` + the **DSI-1 connector** + the **panel attached** + `fb0`; the remaining
step is the **mixer scanout** (first lit pixel). The plan simplified sharply once **mainline
caught up**: `drm-misc-next` now carries the **entire A523 display pipeline upstream** (DSI host
+ TCON-LCD + DE33 mixer + the SoC DT pipeline + Jernej Škrabec's DE33 fixes), so our out-of-tree
DSI/TCON/mixer/dtsi patches are redundant. We now **base on drm-misc-next** and add only what is
still ours — the **combo-PHY driver**, the **panel driver**, and a **board dts** — then build and
test. Details: [`docs/DISPLAY-PORT-STATUS.md`](docs/DISPLAY-PORT-STATUS.md).

The full boot journey + the SPL fixes are in
[`docs/BOOT-AND-FEL-NOTES.md`](docs/BOOT-AND-FEL-NOTES.md); the captured first boot to a shell
is [`docs/FIRST-MAINLINE-BOOT-2026-08-05.txt`](docs/FIRST-MAINLINE-BOOT-2026-08-05.txt). KNULLI
(a vendor-BSP CFW) also boots this board as a reference chain.

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
