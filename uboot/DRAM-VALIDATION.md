<!-- SPDX-License-Identifier: (GPL-2.0-only OR MIT) -->
<!-- Copyright (C) 2026 Midgy BALON -->

# DRAM / electrical validation — Trimui Smart Pro S (A523, sun55iw3p1)

Validation of the U-Boot DRAM retarget and DTS DRAM supply rail against the
authoritative Allwinner A523 electrical specs. Date: 2026-06-13.

Sources:
- **A523 Datasheet** `docs/a523_trm.pdf`, Ch.6 "Electrical Characteristics"
  (printed p79+, PDF page = printed+1). Key tables: 6-1 (p80/81), **6-2
  Recommended Operating Conditions (printed p81-83 / PDF p82-83)**, 6-3 DC
  (p84), 6-4/6-5 SMHC (p85), 6-9..6-13 clocks (p88+).
- **A523 User Manual** `/tmp/a523_um.txt` (PDF page = printed+1). DRAMC §3.2,
  features list lines 387-396 / 24277-24283.
- Vendor DTB `trimui_smart_pro_source.dts` (board "A523-PRO2-AXP717C").
- Our retarget: `uboot-a523/DRAM-PARAMS.md`, `uboot-a523/trimui-tg5050_defconfig`.
- Our DTS: `dts/sun55i-a523-trimui-smart-pro-s.dts`.

---

## TL;DR verdicts

| Item | Status | Correct value |
|---|---|---|
| (a) `vdd-dram` = 1.16 V | **WRONG (too high)** | **1.10 V** (datasheet typ); range 1.06–1.17 V |
| (b) LPDDR4 vs LPDDR4x | **LPDDR4** (type=8), *not* 4x | VDDQ = 1.1 V, NOT 0.6 V |
| (c) DRAM clock 1200 MHz (2400 MT/s) | **VALID** | at the rated max (1200 MHz) |
| (d) tpr/odt/dri extracted values | Plausible | no electrical contradiction found |

---

## (a) DRAM supply voltage — `vdd-dram` is set too high

**Datasheet Table 6-2 (Recommended Operating Conditions, PDF p82), VCC-DRAM:**

| DRAM type | Min | **Typ** | Max | Unit |
|---|---|---|---|---|
| DDR3 | 1.425 | 1.5 | 1.575 | V |
| DDR3L | 1.283 | 1.35 | 1.45 | V |
| LPDDR3 | 1.14 | 1.2 | 1.30 | V |
| DDR4 | 1.14 | 1.2 | 1.26 | V |
| **LPDDR4 and LPDDR4x** | **1.06** | **1.1** | **1.17** | V |

Our DTS `reg_dcdc3` (`vdd-dram`) is pinned min=max=**1.16 V** (1160000 µV).

- 1.16 V *is* inside the LPDDR4 recommended window (1.06–1.17 V), so it will not
  instantly damage anything, but it sits **+60 mV above the 1.1 V typical and
  only 10 mV under the absolute recommended max (1.17 V)**. That is the wrong
  target: it leaves essentially no margin to the top of the spec, wastes power,
  and runs the DRAM IO hotter than designed.
- **Correct target: 1.10 V (1100000 µV)**, the LPDDR4 typical. The AXP2202
  DCDC3 step is 10–20 mV, so 1.10 V is directly settable.

**Where 1.16 V came from:** it is the CPU-rail value. Note `reg_dcdc1`
(`vdd-cpu`) max is also 1160000, and `CONFIG_AXP_DCDC3_VOLT=1160` in the
defconfig. 1.16 V is a plausible CPU/DSU voltage, **not** an LPDDR4 DRAM
voltage. This looks like a copy/placeholder carried over while the rail was
marked VERIFY.

### Which AXP rail actually feeds DRAM
Vendor PMIC is **AXP2202** (silk "AXP717C"), `pmu@34` on r_i2c0
(`trimui_smart_pro_source.dts:4670`). Its regulator block (lines 4802-4831)
declares only min/max **ranges**, not fixed operating voltages — the runtime
voltage is programmed by boot0 / the PMIC driver, which is exactly why the
DRAM rail must be confirmed on-device.

- `axp2202-dcdc3` range = 0x7a120..0x1c1380 = **0.50–1.84 V** (line 4824). That
  wide low-voltage range is the classic DRAM/low-rail DCDC; on AXP2202 + A523/
  T527 designs **DCDC3 is the DRAM rail**. Our DTS already maps dcdc3 →
  vdd-dram, which is consistent.
- For contrast, dcdc2 ranges to 3.4 V and dcdc4 to 3.7 V (IO rails); dcdc1 to
  1.54 V (CPU). So dcdc3 is the only DCDC whose range is centred on ~1 V — the
  DRAM rail assignment is sound.
- The static vendor DTS contains **no explicit 1.1 V / 1.16 V literal for
  dcdc3** (grep found none); the only ~1.1 V literals are CPU OPP floors
  (`0x10c8e0` = 1100000 in the cluster opp tables). So the vendor's actual DRAM
  voltage isn't pinned in the DTS — it is set by boot0. Read it on hardware via
  i2c (AXP2202 DCDC3 voltage register) to get the vendor's exact value; expect
  ~1.10 V.

**Action:** change `reg_dcdc3` in
`dts/sun55i-a523-trimui-smart-pro-s.dts` from 1160000 to **1100000**
(min=max=1100000), and `CONFIG_AXP_DCDC3_VOLT` in
`uboot-a523/trimui-tg5050_defconfig` from `1160` to **`1100`**. Then confirm
against the on-device AXP2202 readback before declaring final.

---

## (b) DRAM type — LPDDR4, NOT LPDDR4x (VDDQ matters)

- Our extracted `type = 8` = **LPDDR4** (`DRAM-PARAMS.md`; mainline
  `SUNXI_DRAM_A523_LPDDR4`). Avaota-A1 reference is the same. This is plain
  LPDDR4, not LPDDR4x.
- The electrical difference between the two is **VDDQ (DRAM IO rail)**:
  - Datasheet Table 6-2 footnote (1) (PDF p82): "**VCC-DRAML is 0.6 V only when
    LPDDR4x is used. When DDR3/DDR3L/LPDDR3/DDR4/LPDDR4 is selected, the voltage
    of VCC-DRAM and VCC-DRAML are the same.**"
  - VCC-DRAML row: 0.57 / **0.6** / 0.65 V — this 0.6 V applies **only** to
    LPDDR4x.
- Therefore, because this board is **LPDDR4 (type=8)**: VCC-DRAML must track
  VCC-DRAM at **1.1 V**, NOT 0.6 V. If anyone later "optimises" by dropping the
  IO rail to 0.6 V (the LPDDR4x value), it would be wrong for this part and
  could corrupt training/operation.
- Net: a single ~1.1 V DRAM rail (VDD2/VDDQ = VCC-DRAM = VCC-DRAML = 1.1 V) is
  correct here. (VDD1 ≈ 1.8 V is a separate internal-pad rail — see VDD18-DRAM
  in Tables 6-1/6-2, recommended 1.71/1.8/1.89 V — supplied independently and
  not the dcdc3 concern.)

---

## (c) DRAM clock 1200 MHz / 2400 MT/s — valid

- User Manual DRAMC feature list (lines 396 and 24283): "**Clock frequency up
  to 1200 MHz for DDR4, LPDDR4, and LPDDR4x.**"
- Our `clk = 1200` (MHz) → **2400 MT/s**. This is **exactly at the rated
  ceiling**, and matches both the A523 family default and the Avaota-A1
  reference. Valid — no down-clock required.
- Note this is the *max*; it leaves zero headroom above 1200 MHz, so do not
  attempt to raise it. The datasheet Ch.6 clock section (Tables 6-9..6-13) only
  specs the 24 MHz / 32.768 kHz reference oscillators; it does not impose a
  tighter DRAM-clock electrical limit than the 1200 MHz DRAMC figure.

---

## (d) ODT / drive-strength / tpr plausibility

The datasheet electrical chapter does **not** publish per-pin LPDDR4 ODT/drive
ohm targets (those live in the DRAMC registers / MR settings, recomputed by the
init driver). So the datasheet cannot prove the exact tpr/odt/dri numbers — it
can only bound them. Cross-checks performed:

- `dx_odt = 0x07070707`, `dx_dri = 0x0d0d0d0d`, `ca_dri = 0x0e0e`,
  `odt_en = 0x84848484`, `tpr0 = 0x80808080` — all identical to the Avaota-A1
  (T527, same die) reference. These are family-default DQ/CA ODT & drive codes;
  nothing in Ch.6 contradicts them.
- The 5 board-specific deltas (`tpr2=0x1f090503`, `tpr6=0x3a000000`,
  `tpr10=0x862f3333`, `tpr11=0xc0c0bbbf`, `tpr12=0x35352f31`) are timing/training
  words, not electrical-rail values — out of scope for the datasheet to confirm;
  they came from the vendor boot0 and are the best available pre-hardware values.
- **One consistency note:** ODT/drive codes are calibrated to a specific IO
  voltage. They were captured from a vendor boot0 that runs the DRAM rail at the
  vendor's (LPDDR4, ~1.1 V) voltage. They are therefore self-consistent with a
  **1.1 V** DRAM rail — which is a second, independent reason the rail should be
  1.10 V, not 1.16 V. Training at 1.16 V with codes tuned for 1.1 V is a further
  argument against the current DTS value.

No electrical red flags in the extracted DRAM init parameters beyond the supply
voltage issue in (a).

---

## Required corrections (summary)

1. **`dts/sun55i-a523-trimui-smart-pro-s.dts`**: `reg_dcdc3` `vdd-dram`
   1160000 → **1100000** (min=max). [Datasheet Table 6-2, PDF p82: LPDDR4 typ
   1.1 V.]
2. **`uboot-a523/trimui-tg5050_defconfig`**: `CONFIG_AXP_DCDC3_VOLT=1160` →
   **`1100`**.
3. Keep DRAM type LPDDR4 and clk=1200 as-is — both validated.
4. Do **not** set any DRAM rail to 0.6 V — that VCC-DRAML value is LPDDR4x-only
   (footnote 1, p82); this board is LPDDR4.
5. On first hardware access, read AXP2202 DCDC3 voltage over i2c to capture the
   vendor's exact DRAM voltage and confirm 1.10 V (`recon.sh` §3).
