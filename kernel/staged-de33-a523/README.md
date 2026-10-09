<!-- SPDX-License-Identifier: (GPL-2.0-only OR MIT) -->
<!-- Copyright (C) 2026 Midgy BALON -->

# Staged A523 DE33 patch set (against Jernej's rework)

Clean, upstream-shaped A523 Display-Engine patches drafted **offline**, following the analysis
in [`../harvest-sun55i-de/DE33-ALIGNMENT.md`](../harvest-sun55i-de/DE33-ALIGNMENT.md). They add
A523 (sun55iw3) DE3.5 support to mainline's DE33 in the **new shared-`planes` model**.

## ⚠ Base, status, and what is / isn't verified (read first)

- **Base:** mainline **+ Jernej Škrabec's `[PATCH v2 0/8] drm/sun4i: update DE33 support`
  (patches 1–8) applied.** That rework is **NOT merged yet** (as of 2026-10-09 — see
  [[de33-upstream-jernej-v2]]), so these patches **cannot be `git apply`-verified** against any
  tree that exists today. They target files the rework *creates/rewrites*
  (`sun50i_planes.c`, the minimal DE33 mixer cfg, the split bindings).
- **Driver patches (0001, 0002):** the *content* (structs, table/enum entries) is correct and
  modelled exactly on the H616 entries in the rework. The **hunk line offsets are UNVERIFIED** —
  regenerate and `git apply --check` them against the merged rework before sending upstream.
- **DT (0003):** **DRAFT.** The DE3.5 register split (`display`/`top`/planes/syscon bases+sizes)
  and the channel `def_map` are **best-estimates from the BSP + our captures**, flagged inline
  with `VERIFY`. They need BSP (`lowlevel_v35x/de_top.c`) + on-HW confirmation.
- This set covers the **plane/mixer/mux/DT**. The HW test below resolved the one open gate
  (direct-AHB works → no separate RTMX/RCQ latch driver needed), so this is ~the whole A523 DE port.

## ★ The gate — RESOLVED on hardware (2026-10-09): direct-AHB works

The open question was *"is RCQ mandatory on A523, or does direct-AHB commit work like H616?"*
(DE33-ALIGNMENT §5.1). **Tested on HW: direct-AHB works.** Booting `use_rcq=0` lit the panel
(white) and dmesg showed the AHB path writing the *active* layer registers with no RCQ kick
(`RCQ-RB … kicked=0 use_rcq=0`, `hw_attctl` reads back enabled). So:

- **This set is essentially the whole A523 DE port** — `sun55i_de.c` (the BSP-derived RCQ engine)
  is **dropped entirely**; A523 is "H616 DE33 + these patches."
- **No net-new RCQ driver needed.**

Caveats: `use_rcq=0` showed slight flicker (our crude path lacks a vsync double-buffer; H616's
proper AHB commit is flicker-free — expected fixable). The *definitive* confirm is running
Jernej's H616 mixer path on A523, but the RCQ risk is now low. **Heat remains a separate blocker**
(the IC overheated → fractal within ~2 min; DSI HS-clock duty, not the commit path) and gates
sustained display use.

## Contents / apply order

1. `0001-drm-sun4i-planes-add-sun55i-a523-de33-planes.patch`
   — A523 `sun50i_planes_quirks` (def_map + cfg) + `allwinner,sun55i-a523-de33-planes` compatible
   (driver + planes binding).
2. `0002-drm-sun4i-mixer-add-sun55i-a523-de33-mixer.patch`
   — minimal A523 `sun8i_mixer_cfg` + `allwinner,sun55i-a523-de33-mixer-0` compatible
   (driver + mixer binding, new split form). This is our old `patch 0008` re-shaped.
3. `0003-dt-sun55i-a523-de33-new-model.dtsi`
   — the A523 DT display-engine nodes in the new 3-node form (syscon + planes + mixer),
   replacing the disabled `de@5000000` skeleton from our `kernel/patches/0003`.

## Open items (carried from the analysis)

- **def_map** — `{0,6,7}` here (UI primary, mirrors H616). Stock routes `{0,1,6,7,8}`; BSP has 6
  channels. Final set/order = HW decision.
- **DT reg split** — exact `display`/`top`/planes/syscon bases+sizes; and whether A523 needs a
  separate `de33-clk` CCU node (H616 has one) or the de-top `0x5008000` serves as the
  plane-mapping syscon. Also: mux-write ordering (planes probe writes the mux before the mixer
  enables the DE clock — confirm the de-top is reachable then).
- **Colour / CSC** — check whether the rework's DE33 CSC (patch 3) subsumes our device-output
  (DCSC) bypass, or the DCSC is a separate unit A523 must still program.
- **RCQ** — ✅ resolved (§5.1): direct-AHB works on HW, `sun55i_de.c` dropped, no RCQ driver.

## When the rework merges

Re-base these onto it, `git apply --check` each, run `make dt_binding_check` on the two yamls and
`make dtbs`, then (pending the RCQ gate + HW bring-up) post as a small per-subsystem series,
coordinating with Jernej (A523 as a first real DE33 user) and ut-slayer (RCQ).
