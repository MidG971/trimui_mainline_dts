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
> Linux to a shell on real hardware** with **WiFi, storage, PMIC/battery, USB2 host and the
> GPU working**, but several peripherals (display, audio, gamepad) are **not yet validated**,
> and the device tree may still be wrong. Flashing or FEL-booting
> custom firmware **can permanently brick your
> device**, corrupt data, or damage hardware. **No warranty, no liability — you use
> this entirely at your own risk.** Back up your stock firmware first. Booting from
> microSD leaves the eMMC untouched (pull the card to return to stock), and the vendor
> `trimui_tg5050.awing` factory-restore is the brick lifeline.

## Where things stand

**🎉 Mainline Linux boots on the device — all the way to an interactive shell.** The full
chain works on real silicon: **BROM → SPL → BL31 → U-Boot → distro-boot → Linux (v7.2-rc3)
→ busybox shell** (over a UART on PB9/PB10). Working today: 8× Cortex-A55 **SMP**, **DRAM**
(1 GiB), **microSD + eMMC** (the kernel `sunxi-mmc` new-timings path), the **AXP2202/AXP717
PMIC** + regulators, the **battery fuel gauge** (charge %, voltage, charge current/status, and
USB-power detection straight from the AXP717 hardware E-gauge), **USB2** host, the **Mali-G57
GPU** (Panfrost), the RTC, and — the latest — **on-board WiFi** (AIC8800D80 → `wlan0`), with the
device reachable **headless over Tailscale**.

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

**WiFi + headless networking (latest).** On-board **WiFi is up on hardware** — the AIC8800D80 SDIO
chip enumerates on `mmc1`, firmware loads (from `/lib/firmware/aic8800_sdio/`), and **`wlan0` scans
and associates** (WPA2, 5 GHz VHT). The wall was the power-on sequence: the vendor `sunxi-rfkill`
drives **three** chip enables (`power_en` PL7 + `chip_en` PM5 + `wlan_regon` PM1) and mainline was
driving only `wlan_regon`, so the chip stayed silent to CMD5. Adding the other two + capping SDIO to
25 MHz (the A523 sunxi-mmc high-speed read sample-phase is uncalibrated → data reads CRC-fail) brings
it fully alive. The device now runs **headless** — a Debian rootfs auto-connects WiFi and brings up
**Tailscale** on boot (a `fake-hwclock` fixes the no-persistent-RTC clock that otherwise breaks TLS),
so it's reachable over the network without the UART. *(Fix committed: `29f4eee`.)*

**Display (deferred to last).** Paused while the input/audio subsystems get brought up — but a lot is
already solved on hardware: the panel's **backlight is on** and the
entire clock / PHY / TCON / DSI stack is solved and committed. Working on silicon: the from-scratch
**combo-PHY DISPLL locks** (`pll_enable ret=0`), the **TCON pixel clock** runs at the correct
**93 MHz**, and the **DSI** comes up in video mode with the **panel attached + initialised**. Two
bugs were cracked to get there:

- **Combo-PHY was writing into a dead block** — its DT node was missing the APB register clock
  (`CLK_BUS_MIPI_DSI1`), so every register read/wrote to an unclocked block (all-zero read-back).
  With that plus a register-bit (ENLDOR) misdefine, wrong analog trim, and wrong PLL dividers all
  corrected, the DISPLL locks. *(HW-verified; committed.)*
- **The DSI was driving the wrong TCON** — `tcon_tv0` (the TCON-TV, no channel-0) binds before the
  LCD TCON and grabs **CRTC 0**, but `sun6i_mipi_dsi` hardcodes the encoder to `possible_crtcs =
  BIT(0)`. So `sun4i_dclk_create()` never ran on the LCD TCON and its pixel clock was NULL.
  Disabling `tcon_tv0` (no HDMI on this handheld) + pinning `tcon-ch0` to 372 MHz fixes it — dclk
  is now exactly 93 MHz. *(HW-verified; committed.)*

**The pipeline now runs end-to-end.** An A523-specific **continuous-TCON vblank rework** — the DSI
drives the TCON like an RGB panel with a *free-running* vblank, instead of the 8080 CPU-interface
per-frame trigger that never completes on this SoC — got the CRTC vblank firing at 60 Hz and every
atomic commit completing (no more `flip_done` timeouts). Two more DSI-path fixes went in alongside:
the tcon-top `PORT_SEL` mixer→TCON routing (the DSI case never programmed it) and the tcon-top DSI
datapath gate. The **one remaining blocker** is now the **DE33 mixer's continuous streaming**: the
DSI transmits ~one line then starves for data (its `video_curr_line` counter freezes at 1), so the
panel shows backlight but no image yet — the mixer→TCON data feed is the last thing standing. A note
on strategy: `drm-misc-next` carries mainline's A523 DSI/TCON/DE33 drivers, but we verified its TCON
path is byte-identical to ours. It *may* still be the cleanest route for the last piece — the
**DE33 mixer**. Details:
[`docs/DISPLAY-PORT-STATUS.md`](docs/DISPLAY-PORT-STATUS.md).

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
