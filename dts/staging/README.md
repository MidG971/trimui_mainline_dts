<!-- SPDX-License-Identifier: (GPL-2.0-only OR MIT) -->
<!-- Copyright (C) 2026 Midgy BALON -->

# dts/staging — drafts gated on external upstream work

These fragments are **NOT** included by the board DTS and **do not build today**.
They capture the verified vendor-DTB facts and the intended mainline wiring for
features whose kernel support is still landing upstream, so they are quick to
finalise the moment the dependency is in our baseline.

| File | Provides | Blocked on | Drop-in when |
|------|----------|-----------|--------------|
| `trimui-gpadc-joystick.dtsi` | Analog sticks (`adc-joystick` on GPADC) | A523 GPADC driver + DT node | mainline **v7.2** (driver `sun20i-gpadc-iio`, node `adc@2009000` in `sunxi/dt-for-7.2`) |
| `trimui-usb3.dtsi` | USB3 host (dwc3) + GMA340 SuperSpeed mux | A523 USB3 CCU clocks + combo-PHY + dwc3 glue | Kalashnikov "[PATCH] a523: add USB3.0 support" series (needs a v2; not in v7.1/v7.2) |

Each file's header documents what is verified vs. what must be calibrated /
relabelled on hardware or against the final upstream nodes.
