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
      [`../kernel/patches/0001-drm-sun4i-dsi-sun55i-a523-host-variant.patch`](../kernel/patches/0001-drm-sun4i-dsi-sun55i-a523-host-variant.patch).
- [ ] **Combo-D-PHY driver** (new) — `allwinner,sun55i-a523-dsi-combo-phy`, base
      0x05509000. Port from BSP `phy/sunxi_dsi_combophy.c`; it also provides the
      high-speed lane clock to the host. *This is the critical path.*
- [ ] **TCON-LCD** sun55i compat in `sun4i_tcon.c` (DSI single-link), BSP ref
      `sunxi_device/sunxi_tcon.c`; wire TCON-TOP (`vo0@5500000`).
- [ ] **Panel** — small DSI panel driver carrying the decoded init/exit blob from
      [`../dts/trimui-panel.dtsi`](../dts/trimui-panel.dtsi) (or panel-mipi-dsi + blob).
- [ ] **DE3.5 mixer** (`display-engine-v350`, 0x05000000) — largest piece; base on
      the in-flight DE33 series, diff register layout for v350.
- [ ] DT: assemble OF graph `de → tcon1 → dsi1 → panel`; `modetest` 720×1280.

**Recommended milestone order:** combo-PHY → TCON → push a solid-colour test pattern
through TCON before the full DE mixer (isolates the novel DSI/PHY work).

## Build / test

Mainline tree on `compiler-rock3b:/root/rock3b-build/linux-mainline` (6.19);
`CONFIG_DRM_SUN4I/SUN6I_DSI/PHY_SUN6I_MIPI_DPHY=m`. Compile-check a single TU:
`make ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- drivers/gpu/drm/sun4i/sun6i_mipi_dsi.o`.
The host can't probe until the combo-PHY + DT land (it requires its `phys=<&...>`).
