<!-- SPDX-License-Identifier: (GPL-2.0-only OR MIT) -->
<!-- Copyright (C) 2026 Midgy BALON -->

# Thermal + CPU/GPU DVFS notes (A523 / Trimui Smart Pro S)

Plan and findings for making the device throttle, cool, and scale frequency
like a sane daily driver. Splits into three tracks: **thermal sensors (THS)**,
**active cooling (pwm-fan)**, and **CPU/GPU DVFS (cpufreq/devfreq)**. All three
are HW-gated for final tuning; the data here is from the vendor DTB + stock
firmware (v1.0.1 == v1.0.2 vendor DTB, byte-identical) and upstream patch review.

## 1. Thermal sensors (THS) — adopt the upstream series

There is an in-flight mainline series that fully covers the A523 sensors:
**Mikhail Kalashnikov, "Allwinner: A523: add support for A523 THS0/1 controllers"**,
latest **v5** (based on 7.2-rc1, posted 2026-07-04, `Reviewed-by: Chen-Yu Tsai`
on 3/5 patches — close to merge but **not** in v7.2-rc1). We do **not** write our
own THS driver; we adopt this when it merges.

What it provides (authoritative sensor/zone map):

| Controller | Base | IRQ | mod clock | Sensors |
|---|---|---|---|---|
| `ths1` | `0x02009400` | GIC_SPI 62 | `CLK_GPADC1` | idx0 = big cores, idx1 = little cores, idx2 = GPU |
| `ths0` | `0x0200a000` | GIC_SPI 39 | `CLK_GPADC0` | single = DRAM |

- Shared bus clock `CLK_BUS_THS`, shared reset `RST_BUS_THS`
  (`devm_reset_control_get_shared_deasserted`).
- Calibration = **two** SID eFuse nvmem cells: `ths_calibration0@38` (0x38, 8 B) +
  `ths_calibration1@44` (0x44, 8 B), merged by the driver.
- A 4th (NPU) sensor exists in the datasheet but has no usable registers; ignored.
- Zones created: `cpu0_thermal` (`<&ths1 1>`), `cpu4_thermal` (`<&ths1 0>`),
  `gpu-thermal` (`<&ths1 2>`), `ddr-thermal` (`<&ths0>`). CPU trips 70/90 C passive
  + 110 C critical; GPU 60/90 + 110; DDR 110 critical. `sustainable-power`
  cpu0=1200, cpu4=1600, gpu=2400 mW. Every `cpu@` gets `#cooling-cells = <2>`.

**Known bugs in v5 (flagged in list review — adopt the *merged* version, not v5):**
- DTS patch: the gpu zone's trips node is named `gpu-trips` instead of `trips`, so
  the thermal-OF core silently ignores **all** GPU trips (including 110 C critical).
- Driver: `if (!caldata[0])` aborts calibration when the (unused) eFuse bits 0-15
  are zero; plus NULL-deref / OOB paths if `nvmem-cell-names` is missing/short.

If we ever carry the series before it merges, fix `gpu-trips` -> `trips` first.

## 2. Active cooling — the pwm-fan (our contribution)

The upstream zones only **passive-throttle cpufreq**; they bind no fan. The board
has a real fan the series does not use:

- `pwm_fan` (compatible `pwm-fan`) on **PWM0 channel 10 / PB6**, 40 us period,
  inverted; 33 `cooling-levels` (states 0..32, duty 0..255), `#cooling-cells = <2>`.
  Already in the board DTS; usable manually today.

Vendor behavior (from stock `thermald` + the Fn "Fan Level" widget):
- Fan is exposed as `.../soc@3000000:pwm_fan/hwmon/hwmon0` **and** as thermal
  `cooling_device0`. `thermald` reads `thermal_zone0/1/2/temp` and writes
  `cooling_device0/cur_state` (`set fanlevel %d, maxtemp %d`); stops the fan on suspend.
- Manual levels (Fn menu) -1..6 map to cooling `cur_state` `(auto, 0, 20, 22, 24, 26,
  28, 30)` — i.e. a small band near the top of the fan's range; `-1` hands control
  back to thermald (auto).

Our plan: `dts/staging/trimui-thermal.dtsi` adds the fan as an **active cooling
device** to `cpu0_thermal` + `cpu4_thermal` cooling-maps (gpu once its trips are
fixed upstream). Gated on the THS merge (needs the zone/trip labels). Governor
caveat: the upstream zones set `sustainable-power` (selects IPA); a plain pwm-fan
isn't a power actor, so on HW either add `dynamic-power-coefficient` to the CPUs or
drop `sustainable-power` to use step_wise for the fan. Tune on-device.

## 3. CPU / GPU DVFS

### CPU (cpufreq)
Real per-cluster OPP ladders are transcribed in `dts/staging/trimui-cpu-opp.dtsi`
(from the vendor `cluster{0,1}-opp-table`):
- **little** cpu0-3 (`reg_dcdc1`): 408 -> **1416 MHz**, 0.90 -> 1.15 V
- **big** cpu4-7 (`tcs4838`): 408 -> **1800 MHz** nominal (0.90 -> 1.15 V);
  turbo bins 1992 @1.22 V / 2088 @1.24 V / 2160 @1.28 V (better silicon only).

Two cpufreq domains confirmed on the stock OS: `cpufreq/policy0` (little) and
`policy4` (big). **Still blocked** on: (1) no A523 CPU clock exposed in the mainline
CCU (`CLK_CPUX` absent) so `cpufreq-dt` has nothing to scale; (2) big-cluster
`tcs4838@0x41` regulator has no mainline driver. Both must land before CPU DVFS works.

Vendor DVFS presets (map cleanly to mainline governors):
- **Performance**: `scaling_governor = performance`, min = max pinned high.
- **Normal**: `scaling_governor = ondemand`, floor ~1008 MHz.
- Vendor also has BSP-only `cpufreq/policy{0,4}/force_{min,max}_freq` clamps
  (reset to 0 = unclamped at boot) — not a mainline interface; use standard
  scaling_min/max + governors instead.

### GPU (devfreq / Panfrost)
Full vendor ladder 150 / 200 / 300 / 400 / 600 / 648 / 696 / 744 / 840 / 888 MHz
in `dts/staging/trimui-gpu-opp.dtsi` (clock-only; shared always-on mali rail). This
one compiles today (the `gpu@1800000` node + `CLK_GPU` are upstream); it is only
HW-unverified. Keep a conservative default ceiling (~600 MHz) and validate the top
steps on the device. See [GPU-NOTES.md](GPU-NOTES.md).

## Ordering (once hardware + THS are available)
1. THS series merges -> rebase, sensors read temperature.
2. Enable `trimui-thermal.dtsi` (fan cooling-maps); tune trips/governor vs measured temps.
3. CPU clock + tcs4838 land -> enable `trimui-cpu-opp.dtsi` (cpufreq-dt), pick governor.
4. GPU OPP (`trimui-gpu-opp.dtsi`) any time; validate ceiling.
