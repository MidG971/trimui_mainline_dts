<!-- SPDX-License-Identifier: (GPL-2.0-only OR MIT) -->
<!-- Copyright (C) 2026 Midgy BALON -->

# TCS4838 CPU regulator — driver notes (the big-cluster DVFS keystone)

The **big cluster (cpu4–7) CPU supply** is a **TCS4838** buck at **0x41 on r_i2c0**
(vendor compatible `ext,tcs4838`, rail `tcs4838-dcdc0`). Without a mainline driver
for it, the big cores run at the **fixed bootloader voltage** — cpufreq/DVFS on the
big cluster can't scale voltage (wasted power, no boost). So this driver is the
keystone for the vendor OPP ladder in
[`dts/staging/trimui-cpu-opp.dtsi`](../dts/staging/trimui-cpu-opp.dtsi).

## Approach: a FAN53555-family variant
TCS4838 is a **FAN53555-family digitally-programmable TinyBuck** (TCS = a fabless
clone house). So the mainline path is a **new variant in
`drivers/regulator/fan53555.c`**, not a new driver.

## What we know (from the vendor DTB)
| Property | Value |
| :--- | :--- |
| i2c addr | `0x41` on **r_i2c0** (`s_twi0`, PL0/PL1) |
| Rail | `tcs4838-dcdc0` → cluster1 `cpu-supply` |
| Voltage range | **0.7125 V – 1.5 V** (`regulator-min/max-microvolt` = `0xadf34`/`0x16e360`) |
| ramp-delay | 520 µV/µs |
| enable-ramp-delay | 1000 µs |
| always-on / boot-on | yes (it's the live CPU rail) |

FAN53555-family register model (for reference): `VSEL0`/`VSEL1` at `0x00`/`0x01`
(Fairchild/Silergy) **or** `0x10`/`0x11` (TCS4525/4526 layout), `CONTROL` `0x02`,
`ID1` `0x03`, `ID2` `0x04`; TCS parts add `TIME` `0x13`, `COMMAND` `0x14`.

## RESOLVED — driver written (patch `kernel/patches/0014`, compile-verified)
There is no public tcs4838 datasheet, but the register map + voltage table were
**mined from the Allwinner `tina5.0_aiot` BSP** (`drivers/power/regulator/
pmu-ext-regulator.c` + `include/power/pmu-ext.h`):
- **standard FAN53555 register layout** — `VSEL0` `0x00`, `VSEL1` `0x01`, `CTRL`
  `0x02`, `ID1` `0x03`, `ID2` `0x04` (NOT the TCS4525 `0x10/0x11` layout);
- **voltage table** — linear range **0.7125 V, 12.5 mV step, 64 selectors
  (0x00–0x3F)** → 0.7125–1.5 V, matching the vendor DTB constraint exactly;
- **enable** = `VSEL_BUCK_EN` BIT(7), **mode** = `VSEL_MODE` BIT(6), sel mask
  `GENMASK(5,0)`; **matched by compatible** (`tcs,tcs4838`), no chip-ID auto-detect.

This is **electrically identical to the Silergy SYR82x/SYR83x** the driver already
supports, so patch 0014 adds a dedicated `FAN53555_VENDOR_TCS_4838` (voltage table
fixed by compatible, not by a chip-ID switch) + the `tcs,tcs4838` binding.
**`fan53555.o` cross-compiles clean** (aarch64, on `compiler-rock3b`, v7.1 tree).
Not silicon-verified. The `i2cdump 0x41` capture below is now a **cross-check**
(confirm ID1/ID2 + that the live VSEL sits in-range) rather than the source of truth.

## How to get the missing values
1. **On-device (~30 s), the reliable path** — `recon.sh` §3 and `hw-verify.sh`'s
   `pmic` test now **`i2cdump 0x41`** and decode `ID1(0x03)`/`ID2(0x04)` + the VSEL
   regs. `ID1`/`ID2` identify the exact die (→ its register map + table); the live
   `VSEL` + `regulator_summary` cross-check the base/step. Day-1 task.
2. **BSP mine** — the Allwinner `tina5.0_aiot` BSP has the tcs4838 register handling
   in its power code; if it carries a clean VSEL table we can fill the variant
   no-hardware. (In progress.)

## The variant (patch 0014, for reference)
```c
enum fan53555_vendor { …, FAN53555_VENDOR_TCS_4838, };
{ .compatible = "tcs,tcs4838", .data = (void *)FAN53555_VENDOR_TCS_4838 },

static int fan53555_voltages_setup_tcs4838(struct fan53555_device_info *di) {
    di->vsel_min = 712500; di->vsel_step = 12500;
    di->vsel_count = FAN53555_NVOLTAGES;               /* 64 */
    di->slew_reg = FAN53555_CONTROL; di->slew_mask = CTL_SLEW_MASK;
    di->ramp_delay_table = slew_rates; di->n_ramp_values = ARRAY_SIZE(slew_rates);
    di->enable_time = 400;
    return 0;
}
/* grouped with FAN53555_VENDOR_SILERGY in the vsel-reg + mode switches
 * (standard VSEL0/1 + CONTROL layout); enable BIT(7)/mode BIT(6) of VSEL. */
```

## Remaining wiring once the driver lands
1. Add the **tcs4838 regulator node** to the board DTS on `&r_i2c0` (`@0x41`, the
   `dcdc0` rail, constraints above).
2. **Uncomment** `cluster1` `cpu-supply = <&tcs4838_dcdc0>` in
   `dts/staging/trimui-cpu-opp.dtsi` and promote it.
3. Needs the A523 **CPU clock** (`CLK_CPUX`) too — the *other* cpufreq blocker
   (tracked separately).

**Safety:** never ship guessed VSEL/table values for this rail. Build-verify the
driver, but the values are HW/BSP-confirmed only.
