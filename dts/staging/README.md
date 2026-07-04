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
| `trimui-cpu-opp.dtsi` | Per-cluster CPU DVFS (`cpufreq-dt` OPP tables) + turbo rows (commented) | (1) A523 CPU clock not exposed in mainline CCU (no `CLK_CPUX`); (2) big-cluster `tcs4838@0x41` regulator has no driver. (Real vendor freq/voltage points now filled in from the stock DTB — little 408–1416 MHz, big 408–1800 MHz.) | the CPU clock macro lands upstream **and** the `tcs4838` regulator is drivable |
| `trimui-gpu-opp.dtsi` | Mali-G57 GPU DVFS (`operating-points-v2`, full vendor ladder 150–888 MHz) | **Nothing upstream** — compiles today; only HW-unverified | anytime; keep a conservative default ceiling (~600 MHz) and validate the top steps on HW. Optional, off the critical path |
| `trimui-thermal.dtsi` | Active-cooling map: binds the board `pwm_fan` to the CPU thermal zones (the upstream zones only passive-throttle cpufreq) | A523 THS series not upstream (Kalashnikov v5, based on 7.2-rc1, near-merge) — needs its zone/trip labels | the THS series merges; then enable + tune trips/governor on HW. See `docs/THERMAL-DVFS-NOTES.md` |

Each file's header documents what is verified vs. what must be calibrated /
relabelled on hardware or against the final upstream nodes.
