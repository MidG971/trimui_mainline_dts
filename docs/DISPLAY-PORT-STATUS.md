<!-- SPDX-License-Identifier: (GPL-2.0-only OR MIT) -->
<!-- Copyright (C) 2026 Midgy BALON -->

# Display driver port — status & plan

Live status of the MIPI-DSI display bring-up for the Trimui Smart Pro S (A523 / sun55iw3).
Background and the decoded panel spec are in [`DISPLAY-NOTES.md`](DISPLAY-NOTES.md);
this file tracks the *driver port* itself.

## BSP source of truth (found)

The vendor display stack is the official Allwinner AIOT SDK (public, **no NDA**):

| Repo | Use |
|---|---|
| `gitlab.com/tina5.0_aiot/lichee/bsp` (branch `product-aiot-stable`) | **vendor display drivers** (`sunxi-drm` framework) |
| `gitlab.com/tina5.0_aiot/lichee/linux-5.15` (branch `product-t527-linux`) | vendor kernel (no display in-tree; bsp overlays it) |
| `gitlab.com/tina5.0_aiot/product/docs` | datasheets / user manuals |

The BSP uses the **new `sunxi-drm`** framework (not legacy `disp2`). Key files
(`drivers/drm/` in the bsp repo):

| BSP file | Role |
|---|---|
| `sunxi_drm_dsi.c` (1664 lines) | MIPI-DSI host |
| `sunxi_device/hardware/lowlevel_lcd/dsi_v1.{c,h}`, `dsi_v1_type.h` | DSI register layer (**"dsi_v1"** IP) |
| `phy/sunxi_dsi_combophy.c` (1011), `sunxi_dsi_combophy_reg.{c,h}` | **combo D-PHY** (the new bit) |
| `sunxi_device/sunxi_tcon.c`, `sunxi_tcon_top.c` | TCON-LCD + TCON-TOP |
| `panel/panel-dsi.c` | generic DSI panel (init-sequence driver) |

(Checked out on the build host at `compiler-rock3b:/root/trimui-display/aw-bsp-drivers`.)
Trimui does not publish kernel GPL source (only a toolchain SDK + firmware images),
and the community board trees — radxa/avaota/yuzukiHD — are mainline-style with no
vendor display stack. So this Allwinner BSP is the register/sequence reference.

## Architecture decision

**Extend the mainline `drm/sun4i` stack** (upstreamable) using the BSP + A523 User
Manual as register/sequence references — do *not* port the BSP `sunxi-drm` framework
wholesale (it is not upstreamable).

### Already in mainline 6.19 (big de-risk)

- **CCU** `ccu-sun55i-a523` already exports every display clock: `mipi-dsi1` (0xb28),
  `bus-mipi-dsi1` (0xb4c), `tcon-lcd1`, `combophy-dsi1` (0xb70), `bus-de` (0x60c).
- **DSI host** `sun6i_mipi_dsi.c` supports A31/A64/**A100** — same IP as the A523.
- **D-PHY** `phy-sun6i-mipi-dphy.c` has an A100 variant (reference for the combo-PHY).
- **TCON** `sun4i_tcon.c` goes up to `sun20i-d1-tcon-lcd` (DSI-capable) — extend it.

### Key finding (verified)

The A523 DSI host (`dsi_v1`) is **register-compatible** with the mainline driver:
same instruction-based command engine (`DSI_INST_ID_LP11/TBA/HSC/HSD/LPDT/HSCEXIT/
NOP/DLY/END`) and identical `dsi_basic_ctl` bitfields (`video_mode_burst`/`hsa_hse_dis`/
`hbp_dis`/`trail_fill`/`trail_inv`) ↔ `SUN6I_DSI_BASIC_CTL` BIT(0..3)+TRAIL_INV. So the
host port is a **variant add**, not a rewrite. The novel work is the **combo-PHY**.

## Progress

- [x] BSP source located + cloned; DSI host IP confirmed == mainline.
- [x] **DSI host: `allwinner,sun55i-a523-mipi-dsi` variant added** to `sun6i_mipi_dsi.c`
      + DT binding — compiles clean (linux 6.19). See
      [`../kernel/patches/0001-drm-sun4i-dsi-add-sun55i-a523-MIPI-DSI-host-variant.patch`](../kernel/patches/0001-drm-sun4i-dsi-add-sun55i-a523-MIPI-DSI-host-variant.patch).
- [x] **Combo-D-PHY driver** — `allwinner,sun55i-a523-dsi-combo-phy`, base
      0x05509000. New driver `kernel/drivers/phy-sun55i-dsi-combo.c` (ported from
      BSP `phy/sunxi_dsi_combophy.c`); integrated DISPLL PLL programmed from the
      `hs_clk_rate` passed via `phy_configure()` (so no separate clock provider).
      Kconfig/Makefile patch: `kernel/patches/0002-…`; binding:
      `kernel/bindings/allwinner,sun55i-a523-dsi-combo-phy.yaml`. Builds clean
      (W=1) as a module against 6.19. Clock = `CLK_COMBOPHY_DSI1`, reset
      `RST_BUS_MIPI_DSI1` (shared with the host). *Unverified until hardware:* PLL
      band math + analog trim are faithfully ported but untested on silicon.
- [x] **TCON-LCD** sun55i compat — `allwinner,sun55i-a523-tcon-lcd` added to
      `sun4i_tcon.c` (reuses the D1 LCD quirk: channel-0, `dclk_min_div=1`, r40
      mux) + binding. `kernel/patches/0004-…`. Builds clean.
- [x] **SoC dtsi nodes** — `dsi1`/`dsi1_combo_phy`/`tcon1` (real, default-disabled)
      + `de`/`display-top` (disabled skeletons, **no** compatible so they can't
      mis-bind until a DE driver exists) added to `sun55i-a523.dtsi`, OF graph
      de→tcon1→dsi1→panel. `kernel/patches/0003-…`. `make dtbs` clean; `dt-validate`
      passes. The board can now `#include trimui-panel.dtsi` (labels resolve).
- [x] **PWM** — `pwm-sun20i` driver (PWM v2 IP; ported from A. Shubin's D1 driver
      + `allwinner,sun55i-a523-pwm` compatible) in `kernel/drivers/pwm-sun20i.c`;
      `pwm0@2000c00` node added to the SoC dtsi (clocks bus/hosc/apb0 = CLK_BUS_PWM0/
      osc24M/CLK_APB0, reset RST_BUS_PWM0). `kernel/patches/0005` (node) + `0006`
      (Kconfig/Makefile). Builds clean on v7.1; the full **board+panel+pwm DTB
      now builds** (`make dtbs`, all of pwm/dsi/phy/panel nodes resolve).
- [x] **Panel** — `panel-trimui-smart-pro-s` DRM DSI driver
      (`kernel/drivers/panel-trimui-smart-pro-s.c`, compatible
      `trimui,smart-pro-s-panel`): 720x1280@60, 4-lane RGB888, reset PD22 (120 ms),
      power0/power1 supplies, backlight; replays the vendor init/exit DCS blobs in
      prepare()/unprepare() (modern `mipi_dsi_multi_context` API). Builds clean (W=1)
      on v7.1. `kernel/patches/0007` (Kconfig/Makefile) + binding. The DT panel
      node was slimmed to standard properties (driver owns mode/lanes/init) — this
      also dropped a stray `0x20` byte that had corrupted the init blob in the DTS.
- [ ] **DE3.5 mixer/CRTC** (`display-engine-v350`, 0x05000000) — the remaining
      blocker for any lit pixel (DSI+PHY+TCON probe, but nothing feeds the TCON).
      **De-risked:** v7.1 mainline now has DE33 support in `sun8i_mixer.c`
      (`SUN8I_MIXER_DE33` + the H616 cfg), and the A523 DE3.5 is DE33-class — so this
      is a **mixer-cfg + DT extension**, not a greenfield port.
      **Mixer cfg DONE** (`kernel/patches/0008`, builds clean on v7.1):
      `sun55i_a523_mixer0_cfg` (DE33, 3 VI + 3 UI, map `{0,1,2,6,7,8}`, scaler 0x3f) +
      `allwinner,sun55i-a523-de33-mixer-0` + binding; reg sub-ranges resolved
      (layers `0x05100000` / top `0x05000000` / display `0x05280000`).
      **Remaining = the DT assembly** (display-engine + bus@ + display_clocks reusing
      `sun50i-h616-de33-clk` + mixer node) — genuinely pioneering since *even H616 has no
      in-tree DE33 DT*; open unknowns (bus compatible, display_clocks offset/SRAM,
      `display-engine` compatible) tracked in
      [`../kernel/DE35-NOTES.md`](../kernel/DE35-NOTES.md). Until then `de@5000000`
      stays a disabled skeleton; pixel bring-up is a HW task.

**Status:** the DSI host, combo-PHY, TCON-LCD and the pipeline DT are done and
build clean. Next leverage is PWM (small) then the DE3.5 mixer (large) — nothing
scans out until the mixer/CRTC exists.

## Build / test

**Canonical tree: `compiler-rock3b:/root/trimui-display/linux-rc` = Linux v7.1.**
All four patches (`0001`–`0004`) + `phy-sun55i-dsi-combo.c` apply cleanly and build
clean (W=1) there with zero source changes — no 6.19→7.1 API churn.
`CONFIG_DRM_SUN4I/SUN6I_DSI/PHY_SUN6I_MIPI_DPHY/PHY_SUN55I_DSI_COMBO=m`. Checks:
`make ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- drivers/gpu/drm/sun4i/sun6i_mipi_dsi.o`,
`… drivers/phy/allwinner/phy-sun55i-dsi-combo.o`, `… drivers/gpu/drm/sun4i/sun4i_tcon.o`,
and `… dtbs`. (Older tree `/root/rock3b-build/linux-mainline` = 6.19-rc5, kept as ref.)
The board+panel combined DTB additionally needs a `&pwm0` node (see PWM item).
