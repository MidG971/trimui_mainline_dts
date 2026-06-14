<!-- SPDX-License-Identifier: (GPL-2.0-only OR MIT) -->
<!-- Copyright (C) 2026 Midgy BALON -->

# DE3.5 (display engine) port plan — sun55i-a523

The last piece for a lit panel: a CRTC/scanout source feeding TCON-LCD1.

## Key finding (big de-risk)

Linux **v7.1-rc7 already has DE33 support** in `drivers/gpu/drm/sun4i/sun8i_mixer.c`
(`SUN8I_MIXER_DE33` type, `sun50i-h616-de33-mixer-0` cfg, the top/disp regmap split,
the blender `map[]`). The A523 **"DE3.5" is the same DE3.x generation as the H616
DE33** — so this is a **mixer-cfg + DT extension**, NOT a greenfield mixer/CRTC
driver. Same pattern as the DSI host (A100 variant), TCON (D1 variant), PWM (D1 IP).

(The agent-era "greenfield, port from BSP" framing is obsolete — DE33 landed upstream
since then.)

## What the DE33 model needs

`sun8i_mixer_cfg` (sun8i_mixer.h) — e.g. the H616 one:
```c
static const struct sun8i_mixer_cfg sun50i_h616_mixer0_cfg = {
	.lay_cfg = { .de_type = SUN8I_MIXER_DE33, .scaler_mask = 0xf, .scanline_yuv = 4096 },
	.de_type = SUN8I_MIXER_DE33, .mod_rate = 600000000,
	.ui_num = 3, .vi_num = 1, .map = {0, 6, 7, 8},
};
```
DT (per the de2-mixer binding, DE33 branch): the mixer node needs **3 reg ranges**
`reg-names = "layers", "top", "display"`, clocks `bus`+`mod`, a reset, an OF graph
out to the TCON; plus a `display-engine` aggregator node (`allwinner,*-display-engine`,
`allwinner,pipelines = <&mixer0>`). NOTE: **no in-tree board uses de33-mixer yet**, so
the DT must be built from the binding + DE2/DE3 DT patterns (no copy-paste example).

## A523 DE3.5 topology (from BSP `lowlevel_v35x/de35x/de350_feat.c`)

- **6 channels = 3 VI + 3 UI** (`num_chns=6`, `num_vi_chns=3`), 4 layers/channel.
  → cfg likely `.vi_num = 3, .ui_num = 3`; `map[6]` (channel→blender phy_index) must be
  read from the BSP de_rtmx/de_bld mapping (H616 used `{0,6,7,8}`; A523 needs 6 entries).
- `mod_rate`: confirm from the CCU DE clock (vendor parents DE off a PLL; H616 used 600 MHz).

## Implementation steps (next focused chunk)

1. Extract the exact `map[]` (channel→blender) + reg sub-range offsets for layers/top/
   display within `de@0x05000000` (4 MB) from BSP `de_rtmx.c`/`de_top.c` + the A523 UM
   §5.1. (mainline regmap sizes: top max 0x3c, disp max 0x20000, layers max 0xffffc.)
2. Add `sun55i_a523_mixer0_cfg` (DE33, vi_num=3, ui_num=3, map, mod_rate) +
   `allwinner,sun55i-a523-de33-mixer-0` to `sun8i_mixer.c` of_table + the binding.
3. Restructure `de@5000000` in `sun55i-a523.dtsi`: real compatible, 3 reg ranges
   (layers/top/display) + reg-names, CLK_BUS_DE/CLK_DE, RST_BUS_DE, PD_DE; add a
   `display-engine` node (`allwinner,sun55i-a523-display-engine`, pipelines=<&mixer0>);
   keep the OF graph de→tcon1→dsi1.
4. Confirm the sun4i_drv aggregator binds the sun55i display-engine compatible (add it to
   `sun4i_drv.c` of_table if it gates on compatible).
5. Build: `sun8i_mixer.o`, `sun4i-drm.ko` set, `dtbs`, and the board+panel DTB. Then a
   full `make` of drm/sun4i to catch link issues.

## Reality check
Even compiling + binding, a lit pixel needs HW iteration (clock rates, the map, RCQ/
register-control-queue if DE33 uses it). Goal of the next chunk: a **compiling DE33
mixer cfg + complete DT** so the whole pipeline (DE→TCON→DSI→panel) forms one DRM
device on v7.1-rc7; pixel bring-up is a HW task.

Build host: `compiler-rock3b:/root/trimui-display/linux-rc` (v7.1-rc7, canonical);
BSP DE source: `…/aw-bsp-drivers/drivers/video/sunxi/disp2/disp/de/lowlevel_v35x/`.

---

## Progress + resolved data (2026-06-14)

**DONE — mixer cfg + compatible (kernel/patches/0008, builds clean):**
`sun55i_a523_mixer0_cfg` in `sun8i_mixer.c` — DE33, `vi_num=3`, `ui_num=3`,
`map={0,1,2,6,7,8}`, `scaler_mask=0x3f` (BSP de350 `is_support_scale` = all 1),
`scanline_yuv=4096`, `mod_rate=600000000` (VERIFY) + compatible
`allwinner,sun55i-a523-de33-mixer-0` + binding enum. `sun8i_mixer.o` builds.

**Resolved register/DT data:**
- DE base `0x05000000` (4 MB). Sub-ranges (BSP `lowlevel_v35x` offsets):
  `top` = `0x05000000` (DE top), `layers` = `0x05100000` (RTMX, +0x100000),
  `display` = `0x05280000` (DISP0, +0x280000, size 0x20000). reg-names order in DT =
  **layers, top, display**.
- map / topology from BSP `de350_feat.c` `chn_id_lut = {0,1,2 video, 6,7,8,9 ui}`.
- DE clock: vendor DTB clocks DE off the **main CCU** (CLK_BUS_DE/CLK_DE, RST_BUS_DE).

**Mainline DE33 model (the DT to build):** mirrors `sun50i-h6.dtsi` DE3 block but DE33:
`display-engine` aggregator (needs `allwinner,sun55i-a523-display-engine` added to
`sun4i_drv.c` of_table + the display-engine binding) → `bus@5000000` (simple-bus +
ranges) → `display_clocks: clock@…` (reuse `allwinner,sun50i-h616-de33-clk`, which is in
`ccu-sun8i-de2.c` and does the DE33 magic writes at reg+0x24/0x28) → `mixer@100000`
(our compatible, 3 regs, clocks from display_clocks `CLK_BUS_MIXER0`/`CLK_MIXER0`,
RST_MIXER0, ports out→tcon1).

**Open unknowns for the DT chunk (pioneering — even H616 has NO in-tree DE33 DT):**
1. The `bus@` compatible — does a `allwinner,sun50i-h616-de33` (or similar) bus binding
   exist, or must one be added? (h6 used `allwinner,sun50i-h6-de3`.)
2. The `display_clocks` (de33-clk) reg offset within the DE block + its reg size, and
   whether it needs an SRAM phandle (h6-de3 did: `allwinner,sram`).
3. Whether `sun4i_drv` needs the new `display-engine` compatible (yes — list ends at h6/
   d1/a64; no h616/de33 entry).
These need confirming from the H616 DE33 patch series / BSP before writing the DT, so the
`de@5000000` node stays a disabled skeleton until then.
