<!-- SPDX-License-Identifier: (GPL-2.0-only OR MIT) -->
<!-- Copyright (C) 2026 Midgy BALON -->

# Display bring-up notes — Trimui Smart Pro S (Allwinner A523, sun55iw3p1)

Work stream: **display**. This file captures (1) mainline upstream status, (2) the
complete vendor panel spec decoded from the board DTB, (3) the clock/power/reset
dependency map from the A523 User Manual, and (4) a concrete bring-up plan.

Sources used are cited inline. The A523 User Manual / Datasheet are Allwinner
**Confidential** — only derived functional facts are reproduced here, never
verbatim register dumps.

---

## 1. Upstream status (verified live, June 2026)

### Verdict

**There is no usable in-tree or in-flight mainline driver for the A523 display
pipeline. The full path (DE3.5 → TCON-LCD → MIPI-DSI host → combo D-PHY) must be
ported from the BSP / written fresh.** SoC core support exists, but nothing that
drives a panel.

### What IS mainline

- **SoC base support** for the sun55iw3 family (A523/A527/T527/H728) landed in
  **Linux v6.15** and is being extended (MMC, RGMII, UART, GPU, OPP, etc.).
  Mainline carries everything under the **A523** name (`sun55i-a523`) even though
  the die is shared with T527. [linux-sunxi A523 wiki]
- **DE2 and DE3** display engines are supported by the existing `drm/sun4i`
  mixer/TCON stack (older SoCs: A64, H3/H5, H6, R40, D1/T113, etc.).
- **MIPI-DSI host** `sun6i-mipi-dsi` exists in-tree for A31/A64-class and the
  D1/T113 combo-PHY variant — but **not** wired up for sun55i.

### What is IN FLIGHT (but does not cover us)

- **DE33 series** — "drm: sun4i: add Display Engine 3.3 (DE33) support",
  originally RFC June 2024 (Ryan Walklin), iterated to v12 (May 2025). Targets
  **H616/H618/H700/T507 only**, and only **RGB / LVDS / DesignWare-HDMI** TCON
  outputs. **No MIPI-DSI host. No A523/T527.** YUV/AFBC/HDMI were split into
  follow-up sets. [LWN 977570, dri-devel v8/v12 threads, patchwork
  20240607110227]
- **Combo D-PHY LVDS** — "drm/sun4i: Support LVDS on D1s/T113 combo D-PHY"
  (phy-sun6i-mipi-dphy LVDS mode). Relevant only as a *reference* for how the
  combo PHY is modeled; it is the D1/T113 PHY, not the A523 one. [LWN 1046783]
- Mikhail Kalashnikov is noted on the sunxi wiki as working on **LCD timing
  controller + display engine** for A523/H728/A527/T527, but no merged or posted
  DSI-capable series was found as of this writing. [linux-sunxi A523 wiki]

### Why our SoC is harder than the DE33 work

The vendor DTB declares the DE as **`allwinner,display-engine-v350`** (DE **3.5**),
which is *newer* than the DE33 the upstream series adds. The A523 UM DE feature
list (7 alpha-blend channels, 4 overlay layers/channel, AFBC decoder, keystone,
4096×2048) confirms a DE3.x-class block but a different revision than any block
currently being upstreamed. So even the in-flight DE33 mixer code is at best a
**starting template**, not a drop-in.

### Reuse-vs-port summary

| Block | Mainline today | Plan |
|---|---|---|
| DE3.5 mixer (`de@5000000`, `display-engine-v350`) | No (DE33 in flight, diff rev) | **Port** — base on DE33 mixer series, adjust register layout for v350 |
| DISPLAY0_TOP glue (`vo0@5500000`) | No | **Port** (TCON-TOP-equivalent for sun55i) |
| TCON-LCD1 (`tcon1@5502000`, `allwinner,tcon-lcd`) | Partial (sun4i tcon exists, no sun55i compat) | **Extend** sun4i_tcon with sun55i quirk/compat |
| MIPI-DSI host (`dsi1@5508000`) | `sun6i-mipi-dsi` exists, not for sun55i | **Extend/port** — DSI v1.02, should be close to A31/A64 host |
| Combo D-PHY (`phy@5509000`) | No (D1/T113 combo PHY is different) | **Port** new phy driver `allwinner,sunxi-dsi-combo-phy1` |
| CCU clocks (DSI1/TCONLCD1/COMBOPHY_DSI1/DE) | sun55iw3 CCU is in-tree | **Verify** these gates/muxes are exported by the in-tree CCU; add if missing |
| panel | generic | Use **panel-mipi-dbi-style init blob** via a small panel driver or `panel-simple`+init-seq |
| backlight | `pwm-backlight` ✅ | Reuse as-is once PWM driver for sun55i PWM is in-tree |

The single biggest blocker is therefore the **MIPI-DSI host + combo-PHY pair** for
sun55i: without it the panel cannot be addressed at all, and neither has any
mainline code targeting this SoC. The DE/TCON can lean on existing sun4i + the
DE33 series; the DSI host/PHY is greenfield.

---

## 2. Vendor panel specification (decoded from the board DTB)

Source: `/home/dio/Downloads/trimui_smart_pro_s/trimui_smart_pro_source.dts`
(`panel_0@0`, `backlight0`, `dsi1@5508000`, pinctrl groups, AXP2202 regulators).

### Panel summary

| Item | Value | DTB source |
|---|---|---|
| Compatible (vendor) | `allwinner,panel-dsi` | panel_0@0 |
| Bus | **MIPI-DSI1** (`dsi1@5508000`, status okay; dsi0 disabled) | dsi1 node |
| Lanes | **4** | `dsi,lanes = <4>` |
| Format | **RGB888** (24bpp) | `dsi,format = <0>` (0 = RGB888) |
| DSI flags | `<1>` = video mode burst flag (vendor) | `dsi,flags = <1>` |
| Resolution | **720 × 1280** portrait (panel native); landscape after rotate | timing0 |
| Pixel clock | **62.0 MHz** (`0x3b25180` = 62 000 000) | `clock-frequency` |
| Refresh | ≈ 60 Hz (Ht=776, Vt=1330 → 62e6/(776·1330) ≈ 60.1 Hz) | derived |
| H: active / hbp / hfp / hsync | 720 / 18 / 32 / 6 | timing0 |
| V: active / vbp / vfp / vsync | 1280 / 34 / 16 / 2 | timing0 |
| Reset GPIO | **PD22**, vendor "active" (flag 0) | `reset-gpios = <&pio 3 22 0>` (0x59 3 0x16 0) |
| Reset timing | `reset-num=1`, `reset-delay-ms=120` (0x78) | panel node |
| Backlight | `backlight0` (`pwm-backlight`) | `backlight = <&backlight0>` |

**Decoded timing math** (hbp includes hsync per Allwinner convention is NOT applied
here — these are discrete porches as listed):
Ht = 720 + 18 + 32 + 6 = 776; Vt = 1280 + 34 + 16 + 2 = 1332. Use the discrete
values directly in a mainline `drm_display_mode` (mainline porches are independent).

### Power rails (resolved through phandles → AXP2202)

The PMIC silk is AXP717C, driven by the vendor as **axp2202**. Panel rails:

| Panel prop | DTB phandle | Regulator | Notes |
|---|---|---|---|
| `power0-supply` | `0xc9` | **axp2202 cldo4** | panel logic/IO rail (VERIFY voltage on HW) |
| `power1-supply` | `0x53` | **axp2202 cldo1** | second panel rail (VERIFY) |

Both cldo regulators in the vendor DTB carry only a wide range
(min 0x7a120 = 500000 µV … max 0x3567e0 = 3500000 µV) with a 1 ms enable ramp and
**no fixed voltage** — the panel/PMIC sequence sets them at runtime. **Action: read
the actual cldo1/cldo4 voltages from the running device** (`cat
/sys/class/regulator/.../microvolts` on vendor FW, or i2c dump) before pinning a
value in mainline. Typical for this class: one ~1.8 V (IOVCC) + one ~2.8–3.3 V
(VCC/AVDD), but do not assume.

> NOTE: the current buildable mainline DTS
> (`sun55i-a523-trimui-smart-pro-s.dts`) tentatively assigns
> **cldo1 = vqmmc 1.8 V (mmc)** and **cldo3 = vmmc 3.3 V**. The vendor panel uses
> **cldo1 + cldo4**. cldo1 being shared between MMC-VQMMC and the panel is
> plausible (1.8 V IO), but this overlap must be reconciled when the panel rails
> are confirmed — a shared always-needed 1.8 V rail is fine, a conflicting fixed
> voltage is not.

### Backlight (PWM channel + pin resolved)

- `backlight0`: `pwms = <&pwm0 0 50000 0>` → phandle **0x109 = `pwm0@2000c00`**
  (the main PWM controller, `allwinner,sunxi-pwm-v201`, base 0x02000c00),
  **channel 0**, period **50000 ns** (0xc350) = 20 kHz, polarity normal.
- PWM channel-0 output pin = **PD23** (pinctrl group `pwm0_0@0`,
  `function = "pwm0_0"`, drive-strength 10). Sleep state muxes PD23 to gpio_in.
- `default-brightness-level = 0x32` (50 of 0..255), 256-entry linear brightness
  table.

### MIPI init / exit sequences (format decoded)

`panel-init-sequence` is the standard Allwinner DSI command blob, byte layout:

```
<dsi_type> <wait_ms> <payload_len> <payload[0..len-1]>
```

- `dsi_type`: `0x05` = DCS short write, 0 param; `0x15` = DCS short write, 1
  param; `0x39` = DCS/generic long write; `0x23`/`0x29` would be generic. In this
  blob only `0x15` (len 2) and `0x39` (len 3..) appear.
- `wait_ms`: post-command delay in ms (almost all `0x00` here).
- `payload`: first byte = command/register, rest = data.

The blob is a long register-bank programming sequence using a vendor
page-switch idiom: pairs like `15 00 02 00 80` (write reg 0x00 = 0x80 → select
page/offset 0x80) followed by `39 00 NN CC ...` (write command 0xCC with the
page-selected sub-address). The signature `39 00 04 ff 87 56 01`
(write 0xFF = {0x87,0x56,0x01}) identifies an **Ilitek/Fitipower-style "0xFF"
manufacturer-command-set unlock** — i.e. an ILI7807/ILI9881-class or FT-class DDIC.
Sequence ends with **0x11 (SLPOUT)** + **0x29 (DSPON)** embedded in the tail
(`...d0 56 05 78 01 11 05 14 01 29 ...` — note the `11` sleep-out with 120 ms-ish
waits and `29` display-on with waits). The complete decoded blob is preserved
verbatim in `dts/trimui-panel.dtsi` as a `panel-init-sequence` byte property so it
can be replayed exactly once a DSI host driver exists.

`panel-exit-sequence = <0x5000128 0x5780110>` decodes (same packing, big-endian
per-word) to:
- `05 00 01 28` → DCS short write, wait 0, len 1, **0x28 = DSPOFF** (display off)
- `05 78 01 10` → DCS short write, wait **0x78 = 120 ms**, len 1, **0x10 = SLPIN**
  (enter sleep)

i.e. standard "display off → sleep in (wait 120 ms)" shutdown.

---

## 3. Register-block & clock/power dependency map (A523 User Manual)

Bases from UM §2.1 memory map (confirmed in task brief):

| Block | Base | Vendor DT node | Mainline-equivalent role |
|---|---|---|---|
| Display Engine (DE3.5) | `0x05000000` | `de@5000000` v350 | mixer / overlay / blender |
| DISPLAY0_TOP | `0x05500000` | `vo0@5500000` | TCON-TOP glue / mux |
| TCON_LCD0 | `0x05501000` | `tcon0@5501000` | LCD timing (LVDS/RGB/DSI dual) |
| **TCON_LCD1** | `0x05502000` | `tcon1@5502000` | **our path** — DSI single-link timing |
| MIPI-DSI0 | `0x05506000` | `dsi0@5506000` (disabled) | DSI host 0 |
| COMBOPHY_DSI0 | `0x05507000` | `phy@5507000` | combo D-PHY 0 |
| **MIPI-DSI1** | `0x05508000` | `dsi1@5508000` (okay) | **our DSI host** |
| **COMBOPHY_DSI1** | `0x05509000` | `phy@5509000` | **our combo D-PHY** |

UM facts:
- **MIPI DSI**: compliant with **MIPI DSI v1.02** + **D-PHY v1.1**, up to
  **1.5 Gbit/s/lane**, 4-lane "up to 1280×720@60 and 1920×1200@60" — our 720×1280
  panel is squarely in spec. Pixel formats RGB888/RGB666/RGB666-LP/RGB565. Video
  modes: non-burst sync-pulse / sync-event / burst. (UM §6.2 p757)
- **TCON_LCD1** explicitly "supports MIPI DSI interface with **single link**, up to
  1920×1200@60" — matches dsi1 single-link. (UM §6.3 p758)
- **Sequencing note (UM §6.3.3.12)**: in MIPI-DSI mode the **TCON data clock must
  be started first**, before DSI. Programming-guideline section is written only for
  TCON_LCD0 but applies structurally.

### CCU clock map (UM §2.5, CCU base `0x02001000`)

These are the **register offsets** that gate/mux our pipeline. The in-tree
`allwinner,sun55iw3-ccu` driver must export equivalents; verify each is present.

| Clock | CCU offset | Source mux (SRC_SEL) | Divider |
|---|---|---|---|
| DE_CLK | `0x0600` | (DE muxes; vendor parents DE off PLL via idx) | /M |
| DE_BGR (gate+reset) | `0x060C` | — | bit: DE gate + rst_bus_de |
| DISPLAY0_TOP_BGR | `0x0ABC` | — | top glue gate+reset |
| **DSI1_CLK** | **`0x0B28`** | 000 HOSC / 001 PERI0_200M / 010 PERI0_150M | bit31 gate, M=FACTOR_M+1 (0..31) |
| DSI0_CLK | `0x0B24` | same as DSI1 | |
| **DSI_BGR** | **`0x0B4C`** | — | bit1 DSI1_GATING, bit17 DSI1_RST (bit0/16 = DSI0) |
| TCONLCD0_CLK | `0x0B60` | 000 VID0PLL4X..110 VID1PLL3X (PLL_VIDEO + PERI0PLL2X) | M=FACTOR_M+1 (0..31) |
| **TCONLCD1_CLK** | **`0x0B64`** | same PLL_VIDEO mux as TCONLCD0 | bit31 gate, M=FACTOR_M+1 (**0..15**) |
| COMBOPHY_DSI0_CLK | `0x0B6C` | PLL_VIDEO mux | M=FACTOR_M+1 (0..31) |
| **COMBOPHY_DSI1_CLK** | **`0x0B70`** | PLL_VIDEO mux (000 VID0PLL4X..) | bit31 gate, M=FACTOR_M+1 (0..31) |
| **TCONLCD_BGR** | **`0x0B7C`** | — | bit1 TCONLCD1_GATING, bit17 TCONLCD1_RST (bit0/16 = LCD0) |
| PLL_VIDEO0..3_CTRL | `0x0040/0x0048/0x0050/0x0068` | source PLLs (1.26–2.52 GHz VCO) | feed TCON/PHY muxes |

**Key clock-topology takeaways**
- DSI1 host functional clock comes from **HOSC or PERI0 200/150 MHz** — NOT a
  video PLL. (The DSI controller core clock is low-speed; the high-speed lane
  clock is generated in the combo PHY.)
- TCON_LCD1 pixel clock and COMBOPHY_DSI1 byte/lane clock both come from
  **PLL_VIDEOx** (×4/×3 taps) — this is where the **62 MHz pixel** and the
  **lane bit clock** are derived. For RGB888 4-lane: lane rate ≈ pixel × 24 / 4 =
  62e6 × 6 ≈ **372 Mbit/s/lane** (well under the 1.5 Gbit/s ceiling). Pick a
  PLL_VIDEO rate that divides cleanly to both 62 MHz (TCON) and the DSI byte clock.

### Vendor DT clock wiring (for cross-reference; BSP indices, not mainline)

`dsi1@5508000`:
```
clocks      = <&ccu 0x9b  &ccu 0x9c  &combophy1 2  &combophy1 1>;
clock-names = "dsi_clk", "dsi_gating_clk", "displl_hs", "displl_ls";
resets      = <&ccu 0x40>;                 reset-names = "dsi_rst_clk";
assigned-clocks        = <&ccu 0x9b>;      /* DSI1_CLK */
assigned-clock-parents = <&ccu 0x0c>;      /* a PERI0/HOSC parent index */
phys = <&combophy1>;  phy-names = "combophy";
```
`phy@5509000` (`allwinner,sunxi-dsi-combo-phy1`):
```
clocks = <&ccu 0x9c>;  clock-names = "phy_gating_clk";
resets = <&ccu 0x40>;  reset-names = "phy_rst_clk";
#clock-cells = <1>;  #phy-cells = <0>;
```
`de@5000000` (`allwinner,display-engine-v350`):
```
clocks = <&ccu 0x34  &ccu 0x35>;  clock-names = "clk_de", "clk_bus_de";
resets = <&ccu 0x02>;  reset-names = "rst_bus_de";
iommus = <&iommu 5 1>;  power-domains = <&pd 5>;  /* DE power domain */
```
The combo PHY exports two clocks back to the DSI host (`displl_hs` = combophy
index 2, `displl_ls` = index 1) — i.e. the PHY is itself a clock provider for the
high-speed/low-speed lane clocks. A mainline port must reproduce that PHY→host
clock relationship (or fold it into the PHY driver).

Pipeline graph (vendor `port`/`endpoint`): `de (port@0) → tcon1 → dsi1 → panel`.
TCON1's only out endpoint goes to DSI1; DSI1 port@1 → panel.

---

## 4. Bring-up plan (ordered)

Goal: get a lit, correctly-timed 720×1280 image on MIPI-DSI1. Phased so each step
is independently testable.

**Phase 0 — prerequisites (not display, but blocking)**
1. Confirm PMIC: is the chip really programmable as AXP717 or genuine AXP2202?
   The panel rails (cldo1/cldo4) and their voltages depend on a correct PMIC
   driver. Read live cldo1/cldo4 voltages from vendor FW. (Cross-stream with PMIC
   work.)
2. Confirm a working in-tree **PWM driver** for the sun55i PWM (`pwm0@2000c00`,
   PD23). Without it, `pwm-backlight` won't probe. Check whether sun55iw3 PWM is
   covered by `pwm-sun20i`/`pwm-sunxi` upstream; if not, that's a small port.

**Phase 1 — clocks & PHY (the hard, novel part)**
3. Verify/add CCU exports in `allwinner,sun55iw3-ccu` for: DSI1 gate+reset
   (0x0B4C bit1/bit17), DSI1_CLK (0x0B28), TCONLCD1 gate+reset+clk
   (0x0B7C bit1/17, 0x0B64), COMBOPHY_DSI1_CLK (0x0B70), DE gate/reset (0x060C),
   DISPLAY0_TOP gate (0x0ABC). Add missing ones.
4. Write the **combo D-PHY driver** for `allwinner,sunxi-dsi-combo-phy1`
   (base 0x05509000). This is greenfield for sun55i. Use the D1/T113 combo-PHY
   LVDS series and the BSP `phy@5509000` as references. It must (a) configure DSI
   D-PHY lane timing for ~372 Mbit/s, (b) act as clock provider for displl_hs/ls.

**Phase 2 — DSI host**
5. Port/extend the **MIPI-DSI host** for `dsi1@5508000` (base 0x05508000,
   DSI v1.02). Start from `sun6i-mipi-dsi` (A31/A64) — register layout is the
   closest existing match. Implement: host attach (4 lanes, RGB888, video mode),
   LP command transport for the init blob, video-mode timing programming, and the
   "start TCON data clk first" ordering from UM §6.3.3.12.

**Phase 3 — TCON-LCD**
6. Extend `sun4i_tcon` with a **sun55i tcon-lcd compatible** for
   `tcon1@5502000` (DSI single-link mode), feeding the DSI host. Wire the
   PLL_VIDEO → TCONLCD1 pixel clock (62 MHz) and DISPLAY0_TOP glue.

**Phase 4 — DE3.5 mixer**
7. Port the **DE3.5 mixer** for `de@5000000` (`display-engine-v350`). Base on the
   in-flight DE33 mixer series; diff the register layout against the A523 UM DE
   chapter and the BSP. Hook IOMMU (`<&iommu 5 1>`) and the DE power domain.

**Phase 5 — panel + integration**
8. Panel: either a tiny dedicated `panel-trimui-dsi` driver carrying the decoded
   init/exit blob + timings + reset (PD22, 120 ms) + the two regulators, or reuse
   a generic "DSI panel with init-sequence" mechanism. Backlight via existing
   `pwm-backlight`.
9. Assemble the OF graph (de → tcon1 → dsi1 → panel), enable, and validate:
   modetest shows 720×1280, image is stable, backlight tracks brightness.

**Recommended first milestone:** clock+PHY+DSI host+TCON far enough to push a
solid color test pattern via the TCON, *before* the full DE mixer — that isolates
the novel DSI/PHY work from the larger DE port.

---

## Citations

- linux-sunxi A523 wiki — SoC/display status, v6.15 base, DE33 H616/H700/T507
  scope, Kalashnikov display work: https://linux-sunxi.org/A523
- linux-sunxi mainlining effort: https://linux-sunxi.org/Linux_mainlining_effort
- DE33 series (LWN overview): https://lwn.net/Articles/977570/
- DE33 RFC v1 (patchwork): https://patchwork.kernel.org/project/linux-arm-kernel/cover/20240607110227.49848-1-ryan@testtoast.com/
- DE33 later revisions (v8/v12, dri-devel): https://lists.freedesktop.org/archives/dri-devel/2025-May/507950.html
- Combo D-PHY LVDS series (D1/T113), reference for combo PHY:
  https://lwn.net/Articles/1046783/
- sun6i-mipi-dsi + DSI mod clock discussion (D1/T113):
  https://www.mail-archive.com/linux-sunxi@googlegroups.com/msg35633.html
- A527/T527 SDK + datasheet/UM release (CNX):
  https://www.cnx-software.com/2025/07/07/allwinner-a527-t527-and-a733-datasheets-user-manuals-and-linux-sdk-released/
- A523 User Manual V1.1 (local, Confidential): §2.1 memory map, §2.5 CCU
  (clock-register offsets), §5.1 DE p722, §6.2 MIPI DSI p757, §6.3 TCON LCD p758.
- Board DTB: `trimui_smart_pro_source.dts` (panel_0@0, backlight0, dsi1@5508000,
  phy@5509000, de@5000000, tcon1@5502000, AXP2202 regulators, pinctrl groups).
