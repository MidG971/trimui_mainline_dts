<!-- SPDX-License-Identifier: (GPL-2.0-only OR MIT) -->
<!-- Copyright (C) 2026 Midgy BALON -->

# A523 CPU-PLL clock controller — port plan (the last CPU-DVFS blocker)

CPU `cpufreq`/DVFS on the A523 is blocked on **two** pieces: the big-cluster
regulator ([tcs4838, DONE](TCS4838-NOTES.md)) and the **CPU clock**, which is
**greenfield** — not in v7.2-rc3, `sunxi/for-next`, or `linux-next`. This doc is
the mined structure + the port plan. **CPU DVFS is an optimisation, not a boot
requirement** — the CPU boots fine at the bootloader-set frequency without it.

Mined from the Allwinner BSP on `compiler-rock3b`:
`aw-bsp-drivers/drivers/clk/sunxi-ng/ccu-sun55iw3-displl.c` (317 lines, despite the
"displl" filename it is the `allwinner,sun55iw3-cpupll` driver).

## The hardware
A **dedicated CPU-PLL clock controller**, separate from the main CCU:

| | |
| :--- | :--- |
| Node | `clock@8817000`, `compatible = "allwinner,sun55iw3-cpupll"` |
| reg | `0x08817000`, size `0x4000`; `#clock-cells = <1>`, `#reset-cells = <1>` |
| Props | `pll_step`, `pll_ssc_scale`, `pll_ssc` (spread-spectrum config) |

### Register map (offsets from 0x08817000)
| Off | Clock |
| :--- | :--- |
| `0x0000` | PLL_CPU0 (`ccu_mult`, mult 8/8/12) |
| `0x0004` | PLL_CPU1 (`ccu_nkmp`, N@8, P in CPUA reg; lock BIT28, clear BIT26; SSC `0x0054`) |
| `0x0008` | PLL_CPU2 (`ccu_nkmp`; SSC `0x0058`) |
| `0x000c` | PLL_CPU3 (`ccu_nkmp`, enable BIT27, lock BIT28, N@8, P@`0x0064`; clear BIT26; SSC `0x005c`) |
| `0x0060` | CPUA mux + P-divider |
| `0x0064` | CPUB mux + P-divider |
| `0x006c` | DSU |

### Clock model
- **`pll-cpu1`** (parent `dcxo24M`) → **cluster0 / little (cpu0-3)** CPU PLL.
- **`pll-cpu3`** (parent `dcxo24M`) → **cluster1 / big (cpu4-7)** CPU PLL.
- `pll-cpu0` (mult) + `pll-cpu2` = reference / DSU helpers.
- **`cpua`** mux @0x0060 bits[26:24], parents: `dcxo24M, osc32k, iosc, pll-cpu1,
  pll-peri0-600m, pll-cpu0` → cluster0. **`cpub`** mux @0x0064 → cluster1
  (`…, pll-cpu3, pll-peri0-600m, …`). `pll-peri0-600m` is the **safe parent** to
  mux to while the CPU PLL is reprogrammed.
- IDs: `CLK_PLL_CPU0..3`, `CLK_PLL_CPUA`, `CLK_PLL_CPUB`.

### DT wiring (from the vendor DTB)
- `cpu@0` (cluster0): `clocks = <&cpu_pll CLK_PLL_CPU1>` (index 1)
- `cpu@400` (cluster1): `clocks = <&cpu_pll CLK_PLL_CPU3>` (index 3)
- both already carry `operating-points-v2` (our staged `trimui-cpu-opp.dtsi`),
  `cpu-supply` (cluster0 = `reg_dcdc1`, cluster1 = `tcs4838`), and
  `dynamic-power-coefficient` (little 0x11e=286, big 0x162=354 — for EAS/IPA).

### The glitch-free rate-change protocol (the crux)
The CPU can't run off an unlocked PLL, so a rate change must: mux the cluster to a
safe clock → reprogram + relock the PLL (with SSC) → mux back. The BSP does this
with a **`ccu_pll_nb` notifier** (`cpupll_notifier_cb`): on `PRE_RATE_CHANGE`
enable SSC (BIT31 of the SSC reg), on `POST_RATE_CHANGE` disable it, and each time
`wait_for_clear` on the PLL's `clear` bit (BIT26).

## Port plan (mainline)
1. **New driver** `drivers/clk/sunxi-ng/ccu-sun55i-a523-cpu.c` + header with the
   `CLK_PLL_CPU*` IDs and a `allwinner,sun55i-a523-cpu-pll` binding.
2. Define `pll_cpu0` (`ccu_mult`), `pll_cpu1/2/3` (`ccu_nkmp`), `cpua`/`cpub`
   (`SUNXI_CCU_MUX`) with the register map above.
3. **Reimplement the SSC/clear handling in a custom notifier** — mainline
   `ccu_common` has **no `.ssc_reg`/`.clear`** fields and mainline ccu has **no
   `CCU_FEATURE_CLAC_CACHED`/`CLEAR_MOD`/`TYPE_NKMP`** (BSP-only). Do the SSC
   enable/disable + wait-for-clear against raw registers in a `ccu_pll_nb`
   notifier (mainline **has** `ccu_pll_nb`), like the BSP.
4. Register a **`ccu_mux_notifier`** on `cpua`/`cpub` to switch to `pll-peri0-600m`
   during the PLL reprogram (standard mainline sunxi CPU-clk pattern).
5. Verify the BSP factor macros (`_SUNXI_CCU_MULT_OFFSET_MIN_MAX`, `_SUNXI_CCU_DIV`)
   have mainline equivalents; translate where they don't.
6. SoC DT: add `cpu_pll: clock@8817000`; wire `cpu@` `clocks` = idx 1 / idx 3;
   promote `trimui-cpu-opp.dtsi`.

## Status / risk
**Not started — this is a major greenfield CCU driver and it is boot-critical:**
a wrong PLL factor or a broken rate-change dance means the board does not boot or
runs unstable. So per [[hardware-testing-prevails]] it must be built **with the
device in the loop** — the structure can be compile-checked no-HW, but its
correctness cannot be trusted from a build alone. Depends on nothing else now
that [tcs4838](TCS4838-NOTES.md) is written. Full vendor source cached on the
build host (path above).
