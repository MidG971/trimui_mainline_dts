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
| `trimui-gpadc-joystick.dtsi` | Analog sticks (`adc-joystick` on GPADC) | A523 GPADC driver + DT node | mainline **v7.2** (driver `sun20i-gpadc-iio`, node `adc@2009000` in `sunxi/dt-for-7.2`) |
| `trimui-usb3.dtsi` | USB3 host (dwc3) + GMA340 SuperSpeed mux | A523 USB3 CCU clocks + combo-PHY + dwc3 glue | Kalashnikov "[PATCH] a523: add USB3.0 support" series (needs a v2; not in v7.1/v7.2) |
| `trimui-cpu-opp.dtsi` | Per-cluster CPU DVFS (`cpufreq-dt` OPP tables) + overclock rows (commented) | (1) A523 CPU clock not exposed in mainline CCU (no `CLK_CPUX`); (2) big-cluster `tcs4838@0x41` regulator has no driver; (3) real vendor freq/voltage points | the CPU clock macro lands upstream **and** the vendor OPP table is read off HW |
| `trimui-gpu-opp.dtsi` | Mali-G57 GPU DVFS (`operating-points-v2`, 150–600 MHz) | **Nothing upstream** — compiles on v7.1 today; only HW-unverified | anytime; validate the 600 MHz ceiling on HW. Optional, off the critical path |

Each file's header documents what is verified vs. what must be calibrated /
relabelled on hardware or against the final upstream nodes.
