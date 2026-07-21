<!-- SPDX-License-Identifier: (GPL-2.0-only OR MIT) -->
<!-- Copyright (C) 2026 Midgy BALON -->

# dts/staging — drafts gated on external upstream work

Most fragments here are **NOT** included by the board DTS and **do not build today**.
They capture the verified vendor-DTB facts and the intended mainline wiring for
features whose kernel support is still landing upstream, so they are quick to
finalise the moment the dependency is in our baseline. (Exception: `trimui-gpu-opp.dtsi`
already compiles on the v7.1 baseline — it is staged only because it is HW-unverified.)

| File | Provides | Blocked on | Drop-in when |
|------|----------|-----------|--------------|
| `trimui-usb3.dtsi` | USB3 host (dwc3) + GMA340 SuperSpeed mux | A523 USB3 CCU clocks + combo-PHY + dwc3 glue | Kalashnikov "[PATCH] a523: add USB3.0 support" series (needs a v2; not in v7.1/v7.2) |
| `trimui-cpu-opp.dtsi` | Per-cluster CPU DVFS (`cpufreq-dt` OPP tables, little 408–1416 / big 408–1800 MHz + commented turbo rows) + `cpu-supply` + dynamic-power-coeff | Nothing external — both former blockers resolved **locally**: CPU-CCU adopted from ut-slayer (`0015-0020`, `&cpu_ccu` CLK_CPUL/CPUB), `tcs4838` big-cluster buck driver written (`0014`). Builds today. Already board-only (no `-upstream` variant needed — see the file header). | in use now (opt-in `#include`); enable + tune voltages on HW. **When the CPU-CCU is upstreamed** (ut-slayer's to submit): drop `0015-0020` and use this file unchanged — *unless* the binding is renamed in review (watch CLK_CPUL/CPUB + compatible) |
| `trimui-gpu-opp.dtsi` | Mali-G57 GPU DVFS (`operating-points-v2`, full vendor ladder 150–888 MHz) | **Nothing upstream** — compiles today; only HW-unverified | anytime; keep a conservative default ceiling (~600 MHz) and validate the top steps on HW. Optional, off the critical path |
| `trimui-thermal.dtsi` | **LOCAL/now:** full THS via the ut-slayer driver (`0021-0023`, `sun55i-a523-ths`/`-ths-ddr`) — defines the ths sensors, single 20-byte calibration cell, all zones **and** the `pwm_fan` cooling-maps | nothing (works today with `0021-0023`) — it is the interim path until the upstream THS lands | in use now for bring-up; **retire** once the Kalashnikov series merges (see next row) |
| `trimui-thermal-upstream.dtsi` | **REBASED/post-merge:** just the `pwm_fan` cooling-map layered on the **upstream** `cpu0_thermal`/`cpu4_thermal` zones (fan at 70 °C, below upstream's 90 °C DVFS map) — the SoC ths0/ths1 + calibration + zones all come from mainline | Kalashnikov A523 THS series merging (v5, based on 7.2-rc1, near-merge; a v6 for binding nits expected) | the THS series is in your tree: drop `0021-0023`, `#include` this **instead of** `trimui-thermal.dtsi`. **Build-verified 2026-07-21** against K.'s v5 dtsi (DTB builds, fan map merges into both upstream CPU zones). See `docs/THERMAL-DVFS-NOTES.md` |

Each file's header documents what is verified vs. what must be calibrated /
relabelled on hardware or against the final upstream nodes.
