<!-- SPDX-License-Identifier: (GPL-2.0-only OR MIT) -->
<!-- Copyright (C) 2026 Midgy BALON -->

# A523 DE → mainline DE33 alignment analysis

**Goal:** map our working A523 DE-v35x (DE3.5) display-engine work onto mainline's DE33
support *and* Jernej Škrabec's in-flight DE33 rework, so that (a) we know which of our DE
changes are real upstream contributions vs. BSP throwaway, and (b) we have a concrete,
upstream-shaped port plan ready for when the rework lands.

**This is offline analysis** — no hardware, no waiting on the rework to merge. It is grounded
in actual code, not memory:

| Source | What | As of |
|---|---|---|
| `torvalds/linux` master `sun8i_mixer.{c,h}` | current mainline DE33 | verified 2026-10-09 (raw.githubusercontent) |
| `docs/upstream-de33-jernej-v2/patch-{3,5,6,7,8}.txt` | Jernej's `[PATCH v2 0/8] update DE33 support` | pulled 2026-09-06; **not merged** (see status below) |
| `kernel/patches/0008-…-DE3.5-DE33-mix.patch` | our A523 mixer cfg | our tree |
| `kernel/harvest-sun55i-de/sun55i_de.{c,h}` | our BSP-derived RCQ backend | harvested 2026-08-18 |

**Upstream status (2026-10-09, verified):** mainline has the *incomplete, mixer-only* DE33
(`SUN8I_MIXER_DE33` + `allwinner,sun50i-h616-de33-mixer-0`). Jernej's rework (CSC + **planes
driver** + shared-planes **binding split**) is **NOT merged** — the drm/sun4i Makefile has no
`sun50i_planes.o`, and the contentious binding patch 7/8 was still under review through
2026-09-15. The follow-up "H616 display pipeline" series is gated on this rework merging.

---

## 1. Three DE generations, three plane models

| | layers / planes | mux (port→channel) | latch |
|---|---|---|---|
| **DE2 / DE3** | per-mixer, hardwired | n/a | direct AHB |
| **H616 DE3.3** (mainline) | **shared** resource, separate block | syscon regs `0x24/0x28/0x2c` in the clock/DE-top space | direct AHB |
| **A523 DE3.5** (ours) | shared (same as DE3.3) | **same** `0x24/0x28/0x2c` | **RTMX + RCQ** (register-command-queue DMA, frame-sync latch) |

The A523 shares DE3.3's **plane/mux model** but adds the DE3.5 **RTMX/RCQ** datapath
(`0x5100000` mixer + `0x5008000` "de-top" glue with GLB_CTL/STS/OUT_SIZE/RCQ at `0x8100+`).
That RCQ layer is the one thing H616 doesn't have.

## 2. Mainline DE33 — today vs. after Jernej's rework

**Today (master):** `struct sun8i_mixer_cfg` still carries `lay_cfg` + `ui_num/vi_num/map[6]`;
the DE33 mixer maps three regmaps (`layers`/`top`/`display`); the port→channel mux is
hardcoded in `ccu-sun8i-de2.c` at clock probe.

**After the rework (patches 5–8, in review)** — the model our DTS and drivers must target:

- **`struct sun8i_mixer_cfg` loses `map[6]`** and the DE33 cfg becomes **minimal**:
  ```c
  static const struct sun8i_mixer_cfg sun50i_h616_mixer0_cfg = {
          .de_type  = SUN8I_MIXER_DE33,
          .mod_rate = 600000000,
  };               /* lay_cfg / ui_num / vi_num / map all gone */
  ```
- **New `sun50i-planes` platform driver** (`sun50i_planes.c`, patch 6) owns the shared planes:
  ```c
  struct sun50i_planes_quirks {
          struct default_map   def_map[MAX_DISP];  /* per-display channel list */
          struct sun8i_layer_cfg cfg;              /* de_type, scaler_mask, scanline_yuv */
  };
  ```
  It programs the mux **generically** from `def_map` in `sun50i_planes_init_mapping()`
  (`CHN2CORE 0x24`, `PORT02CHN 0x28`, `PORT12CHN 0x2c`), via a syscon regmap obtained from the
  mixer/planes DT (`allwinner,plane-mapping` phandle). It exports `sun50i_planes_setup()`.
- **The DE33 mixer** gets a new `sun50i_engine_ops` whose `.layers_init` → `sun50i_layers_init`
  → `sun50i_planes_setup(mixer->planes_dev, …)`. At bind it resolves an **`allwinner,planes`**
  phandle (EPROBE_DEFER until the planes driver is up) and stores `mixer->planes_dev`. It drops
  the separate `disp_regs` (blend writes go to `engine.regs`).
- **New DT shape** (bindings patch 5 + 7):
  ```
  de_syscon:  the clock/DE-top register space (holds mux 0x24/0x28/0x2c)   // syscon
  planes@…  { compatible = "…-de33-planes";  reg = <layer-fetch block>;
              allwinner,plane-mapping = <&de_syscon>; }
  mixer@…   { compatible = "…-de33-mixer-0"; reg = <display>,<top>;   // 2 entries, was 3
              reg-names = "display","top"; allwinner,planes = <&planes>; }
  ```

## 3. Our current A523 DE stack

- **`patch 0008`** adds `sun55i_a523_mixer0_cfg` with `.lay_cfg{…}` + `.ui_num=3 .vi_num=3`
  + `.map={0,1,2,6,7,8}` and a compatible on the **old** mixer yaml. ⇒ shaped for the
  *current-master* model; **obsoleted by the rework** (see §4).
- **`sun55i_de.c`** (BSP-derived, from ut-slayer/OrangePi-4A): the RTMX/RCQ engine. Hand-RMWs
  the mux (`SUN55I_DETOP_UCH2CORE_MUX 0x24`, `PORT2CHN_MUX 0x28`), drives the RCQ
  arm/accept/finish latch, programs the detop RTMX-global regs, and bypasses the device-output
  CSC (DCSC). Integrated into `sun8i_mixer` via a `uses_rcq` hook that delegates commit/layer.

## 4. Keepers vs. throwaway vs. net-new

**THROWAWAY** — superseded by Jernej's generic code; do **not** upstream these:
- `sun55i_de.c`'s **hand mux RMW** (`0x24/0x28`) → `sun50i_planes_init_mapping()` does it from
  `def_map`.
- `patch 0008`'s **cfg shape** (`lay_cfg`/`ui_num`/`vi_num`/`map`) → DE33 mixer cfg is now
  minimal; the map moves to the planes `def_map`.
- `patch 0008`'s edit to the **old** `…de2-mixer.yaml` reg form → must follow the new split.
- Our `uses_rcq` delegation hook in `sun8i_mixer` → replaced by `sun50i_engine_ops`.

**KEEPERS** — real A523 knowledge that feeds the clean port:
- **The A523 channel map.** Decoded from stock `PORT02CHN 0x28 = 0x000A9810`
  (nibble *i* = phys channel on blender port *i*, UI channels carry +2):
  `def_map disp0 = {0, 1, 6, 7, 8}` = **2 VI + 3 UI**, `num_ch = 5`.
  ⚠ This differs from `patch 0008`'s `{0,1,2,6,7,8}` (3 VI + 3 UI) — the HW has 6 channels
  per the BSP `de350 chn_id_lut`, but **stock routes only 5** to disp0. A single-DSI handheld
  needs just the primary; match stock or trim. **Decide/verify on HW.**
- **A523 reg bases:** mixer `display`+`top` blend space, the planes layer-fetch block
  (`0x5100000`), the DE-top syscon holding the mux (`0x5008000`, i.e. mux at `0x5008024/28/2c`).
- `mod_rate = 600 MHz`; scaler_mask / scanline_yuv (verify — Jernej sets `scaler_mask=0` for
  H616 with a "needs driver work" TODO).

**NET-NEW, DE3.5-only, NOT covered by the H616 series** (the hard part):
- **The RTMX/RCQ latch engine.** H616 DE3.3 commits via direct AHB; our A523 path uses RCQ
  (`use_rcq=Y arm_at_target=Y`). If RCQ is *mandatory* on A523, this is a whole new subsystem
  with no upstream equivalent, and the BSP-derived `sun55i_de.c` is **not upstreamable as-is**
  → needs a clean reimplementation (coordinate with ut-slayer, who have the HW-proven A523 RCQ
  for their HDMI path). **See the open question in §5 — this is the fork that sizes the whole job.**
- **The DCSC (device-output CSC) handling** — our bypass fix. May overlap Jernej's patch 3
  ("Add support for DE33 CSC", per-channel CCSC); check whether his CSC path subsumes our
  device-output-CSC bypass or they're distinct units.
- The detop RTMX-global regs (GLB_CTL/STS/OUT_SIZE/RCQ at `0x8100+`) — DE3.5-only.

## 5. Open questions that gate the clean port (HW-resolvable)

1. **★ Is RCQ mandatory on A523, or can it be driven by direct AHB like H616?** Our debugging
   concluded "AHB writes the shadow but never latches → black" and the RCQ arm-at-target was the
   fix — but that was mid-saga, with other bugs confounding it. **Re-test `use_rcq=N` now that the
   rest of the pipeline is correct (gate/trigger/clocks/reset).**
   - If **direct AHB works** → A523 ≈ H616 + planes; the clean series is *small* (planes quirks +
     minimal mixer cfg + DT + bindings), and `sun55i_de.c` is dropped entirely.
   - If **RCQ is required** → the net-new RCQ subsystem dominates the effort; plan a clean
     reimplementation + coordinate with ut-slayer.
   This single experiment sizes the entire upstream job — do it first.
2. **Exact `def_map`** — 5 (stock-routed) vs 6 (HW-present) channels.
3. **Colour** — does Jernej's patch-3 DE33 CSC subsume our DCSC bypass, or is the device-output
   CSC a separate unit we still must program? (ties to the open YUV↔RGB colour item).

## 6. The A523 clean-port plan (shaped against the rework)

Gated on Jernej's rework merging, but **preppable now**:

1. **A523 planes quirks** in `sun50i_planes.c`: `sun55i_a523_planes_quirks` (def_map from §4,
   `cfg` = DE33/scaler/scanline) + compatible `allwinner,sun55i-a523-de33-planes` in
   `sun50i_planes_of_table[]`.
2. **A523 minimal mixer cfg** in `sun8i_mixer.c`: `sun55i_a523_mixer0_cfg = { .de_type=DE33,
   .mod_rate=600000000 }` + compatible `allwinner,sun55i-a523-de33-mixer-0`. (= `patch 0008`
   **stripped** to the minimal form.)
3. **Bindings:** add the A523 compatibles to **both** the mixer yaml (new split form) and the
   planes yaml.
4. **A523 DT:** the DE-top syscon node, the `planes@…` node (reg = layer block,
   `allwinner,plane-mapping`), the `mixer@…` node (reg = display+top, `allwinner,planes`).
5. **RCQ/RTMX:** per §5.1 — either dropped (AHB works) or a clean new driver (RCQ required).
6. **CSC/colour:** align with patch 3.

## 7. Coordinate, don't fork

- **Jernej** (rework author): our A523 would be a real first *user* of the full DE33 chain
  (mixer+planes+TCON+DSI), and — being a single-display SoC that still uses the shared-planes
  model — a useful validation of his "planes are shared, not per-mixer" redesign. Engage once
  the rework is posted/merged; offer to test.
- **ut-slayer** (OrangePi-4A): origin of the A523 RCQ backend; the place to coordinate any clean
  RCQ reimplementation rather than duplicating.
- **leow149** (A133 Smart Pro, sister device): check whether the A133 DE is also DE3.5/RCQ — if
  so, the RCQ work is shared and should be done jointly.

## 8. Immediate, offline next step

Produce the **staged A523 patch set against the rework** (items 6.1–6.4) now, clearly marked
"applies on top of Jernej's DE33 rework, not current mainline," so it's ready to post the moment
the rework merges. The one thing it can't settle offline is §5.1 (AHB-vs-RCQ) — that HW
experiment should run before we commit to the RCQ half.
