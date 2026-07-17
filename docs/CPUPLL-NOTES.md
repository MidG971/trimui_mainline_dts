<!-- SPDX-License-Identifier: (GPL-2.0-only OR MIT) -->
<!-- Copyright (C) 2026 Midgy BALON -->

# A523 CPU-PLL clock controller — ADOPTED (CPU-DVFS unblocked)

CPU `cpufreq`/DVFS needs the A523 CPU clock, which mainline does not have. It was
greenfield — until the **ut-slayer / OrangePi-4A** effort
([github.com/ut-slayer/orangepi-4a-mainline](https://github.com/ut-slayer/orangepi-4a-mainline),
Juan Manuel Lopez Carrillo; T527 = same `sun55iw3` die as our A523) wrote a proper
mainline **CPU-CCU driver**. We **adopted it and forward-ported it to v7.2**.

**Status: ADOPTED + build-verified.** The 6 SoC-level patches apply cleanly to
v7.2-rc3 and the driver + framework objects **cross-compile clean** (aarch64, on
`compiler-rock3b`). Still HW-unverified on our board (it's boot-critical — the
frequencies/rate-change are silicon's call, per [[hardware-testing-prevails]]).

## What we carry (`kernel/patches/0015–0020`, authorship preserved)
Adopted verbatim from ut-slayer (GPL-2.0, `From: Juan Manuel Lopez Carrillo`),
renumbered into our series; they are the SoC-level CPU clock (not the OrangePi
board bits, which we replace with ours):

| ours | theirs | what |
| :--- | :--- | :--- |
| `0015` | 0090 | `drivers/clk/sunxi-ng/ccu-sun55i-a523-cpu.c` (`allwinner,sun55i-a523-cpu-ccu`) + header + Kconfig `SUN55I_A523_CPU_CCU` + Makefile |
| `0016` | 0091 | SoC dtsi node `cpu_ccu: clock-controller@8817000` in `sun55i-a523.dtsi` |
| `0017` | 0093 | reparent the clusters to a safe clock during PLL reprogram |
| `0018` | 0095 | don't touch the unused `pll-cpu0` |
| `0019` | 0097 | **the framework handshake** — adds `CCU_FEATURE_CLEAR_MOD` + a `clear` field to `ccu_common` + the `BIT(26)` update-write in `ccu_mult` (exactly the "mainline doesn't have this, must reimplement" gap this doc flagged) |
| `0020` | 0098 | commit `BIT(26)` in the PLL init sequence |

*Not adopted:* their `0092` (defconfig — we use `kernel/trimui.config`), `0094`
(OrangePi cpu wiring) and `0096` (OrangePi OPP — we have `trimui-cpu-opp.dtsi`).

## Clock model (matches the BSP mine)
`cpu_ccu` @ `0x08817000` provides: `CLK_PLL_CPUL`(0)/`CLK_PLL_CPUB`(1) — the little/big
cluster PLLs — `CLK_PLL_CPU0`(2), and the settable cluster clocks **`CLK_CPUL`(3)**
(cluster0, cpu0-3) / **`CLK_CPUB`(4)** (cluster1, cpu4-7) that `cpufreq-dt` scales.
Register map (PLL_CPU0@0x00, PLL_CPUB@0x0c, CPUB@0x64) matched the earlier BSP mine.

## Our board wiring (done)
- `kernel/trimui.config`: `CONFIG_SUN55I_A523_CPU_CCU=y`.
- `dts/staging/trimui-cpu-opp.dtsi`: `#include <dt-bindings/clock/sun55i-a523-cpu-ccu.h>`;
  cpu0-3 `clocks = <&cpu_ccu CLK_CPUL>`, cpu4-7 `clocks = <&cpu_ccu CLK_CPUB>` (was the
  `CLK_CPU_L/_B` placeholders); OPP tables + `dynamic-power-coefficient` already present.
- The `cpu_ccu` node itself comes from patch `0016` (SoC dtsi), so the board DTS needs
  no cpu-ccu node.

## Remaining
1. **Verify the full series co-applies** (our `0001–0020`) + the board DTB builds on
   v7.2 (the CPU-CCU dtsi node `0016` vs our other `sun55i-a523.dtsi` patches — expected
   fine, non-overlapping, but confirm in the build).
2. **Big-cluster `cpu-supply`** stays commented in `trimui-cpu-opp.dtsi` until the
   **tcs4838 regulator node** is added to the board DTS (the driver is done — see
   [TCS4838-NOTES](TCS4838-NOTES.md)). Little cluster uses `reg_dcdc1` (upstream axp717).
3. **On-hardware:** boot, confirm both cpufreq domains scale, tune the OPP voltages.

## Upstream status
ut-slayer's series is **posted to linux-sunxi for review/feedback, not submitted or
merged** — the author plans to split it for proper submission. So we **adopt + track**
(and are well placed to give feedback). Credits: Juan Manuel Lopez Carrillo (ut-slayer),
building on minimyth2 / Justin Suess (H728) and Jernej Škrabec (H616).
