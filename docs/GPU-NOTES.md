<!-- SPDX-License-Identifier: (GPL-2.0-only OR MIT) -->
<!-- Copyright (C) 2026 Midgy BALON -->

# GPU — Mali-G57 / Panfrost (decision + bring-up notes)

## TL;DR
The A523 GPU is an **ARM Mali-G57** (*Valhall*, **Job-Manager** based). The driver is
**Panfrost** (`drm/panfrost` kernel + Mesa **Panfrost** GLES / **PanVK** Vulkan) — and it is
**already fully supported upstream** (verified on v7.1): the SoC GPU node, the DT binding,
and Panfrost's Valhall/G57 support are all in mainline. For the Trimui board this is **not a
porting effort** — it's a two-line board enable (`status = "okay"` + `mali-supply`).

## Decision: Panfrost. NOT Panthor, NOT the vendor blob.

| Option   | Verdict | Why |
|----------|---------|-----|
| **Panfrost** | ✅ use this | Open, in mainline, supports Valhall **JM** (G57/G68/G77/G78). Probes the exact model from the `GPU_ID` register at runtime. The A523 node + binding + driver are **already upstream** (v7.1). Renders into dma-buf that our `sun4i` KMS scans out. |
| Panthor  | ❌ wrong chip | Panthor only drives **CSF** GPUs (Mali-G310/510/610/710, e.g. RK3588). The G57 has a Job Manager, so Panthor cannot bind it. |
| Vendor Mali blob (kbase / libmali) | ❌ not for this port | Proprietary DDK, pinned to the vendor 5.15 kernel, closed userspace, not upstreamable. Defeats the mainline effort. |

### How we know it's Job-Manager (→ Panfrost, not Panthor)
Vendor DTB `gpu@1800000`: `compatible = "arm,mali-valhall"`, `interrupt-names = "JOB",
"MMU", "GPU"` (IRQs 117/118/119). The **3-IRQ JOB/MMU/GPU** layout is the
Midgard/Bifrost/Valhall-JM signature; CSF parts have a different layout and need Panthor.

## Upstream status (verified v7.1 on the build host)
**All three pieces are already mainline — nothing to write:**
- **Binding:** `Documentation/devicetree/bindings/gpu/arm,mali-bifrost.yaml` lists
  `allwinner,sun55i-a523-mali` with fallback `arm,mali-valhall-jm`. (Valhall-JM lives in the
  *bifrost* yaml; only CSF got its own `arm,mali-valhall-csf.yaml`.)
- **Driver:** `drivers/gpu/drm/panfrost/` has Valhall/G57 support.
- **SoC DT node:** `arch/arm64/boot/dts/allwinner/sun55i-a523.dtsi` already contains:
  ```dts
  gpu: gpu@1800000 {
      compatible = "allwinner,sun55i-a523-mali", "arm,mali-valhall-jm";
      reg = <0x1800000 0x10000>;
      interrupts = <GIC_SPI 117 ...>, <GIC_SPI 118 ...>, <GIC_SPI 119 ...>;
      interrupt-names = "job", "mmu", "gpu";
      clocks = <&ccu CLK_GPU>, <&ccu CLK_BUS_GPU>;   /* 51, 52 */
      clock-names = "core", "bus";
      power-domains = <&pck600 PD_GPU>;
      resets = <&ccu RST_BUS_GPU>;                    /* 6 */
      status = "disabled";                            /* boards enable it */
  };
  ```
- Sibling A523-family boards already enable it: `sun55i-a527-cubie-a5e.dts`,
  `sun55i-t527-avaota-a1.dts`, `sun55i-t527-orangepi-4a.dts` — all with the same pattern.

## What the Trimui board needs (the *only* GPU work — HW-gated)
A board override, mirroring the siblings:
```dts
&gpu {
    mali-supply = <&reg_dcdc2>;   /* AXP2202 DCDC2 — resolved from vendor DTB */
    status = "okay";
};
```
- **GPU rail resolved = AXP2202 `dcdc2`** (`regulator-name = "axp2202-dcdc2"`, vendor phandle
  `0x20`). This matches what the upstream siblings use (`cubie-a5e`, `avaota-a1`:
  `mali-supply = <&reg_dcdc2>`). Vendor DCDC map: dcdc1 → CPU cluster0; **dcdc2 → GPU+VE**;
  dcdc3 → DRAM (1.10 V); CPU cluster1 → external tcs4838.
- It is a **shared GPU/VE rail** (`regulator-always-on`): vendor phandle `0x20` is referenced
  by `mali-supply` (GPU), `ve-supply` (VPU) and `vdd-edp-supply` (eDP, unused here). So treat
  it as a static system rail — don't expect per-GPU voltage DVFS on it (siblings ship no GPU
  OPP table either). OPP voltage sits at 0.9 V; the node's 0.5–3.4 V is a loose vendor range.
- **Only residual unknown = the regulator *label*, not the rail:** the board PMIC is the
  long-standing axp2202-vs-axp717 question (mainline has the axp717 driver). DCDC2 is
  definitive; `reg_dcdc2` is whatever our board PMIC node names it.
- `power-domains = <&pck600 PD_GPU>`, clocks and reset are already wired in the SoC dtsi —
  nothing to add there.
- Kernel: `CONFIG_DRM_PANFROST=m`. Userspace: Mesa with Panfrost + PanVK.
- **Optional later:** an `operating-points-v2` GPU OPP table for DVFS (vendor table:
  150/200/300/400/600 MHz, `dts/staging/trimui-gpu-opp.dtsi`). Not required for basic accel;
  the siblings ship without one — and see the clock-model gate below before enabling it.

## GPU clock model — DVFS is gated on an upstream fix (tracking)
Basic Panfrost accel runs the GPU at the boot clock and needs none of this. But **GPU
DVFS (an OPP table) is gated on a clock-driver fix**, discovered/posted upstream 2026-07-19
by Juan Manuel López Carrillo (ut-slayer) and confirmed by Chen-Yu Tsai:

- The A523/T527 **GPU mod clock (`0x670`) is a cycle-masking / fractional divider**
  (`rate = source * (16 - M) / 16`, T527 UM v0.92 §2.7.6.58), **not** the linear `M+1`
  divider mainline currently models it as.
- With the linear model **every OPP that needs `M>0` silently overclocks** — measured on an
  OrangePi 4A: "150 MHz"→~487, "200"→648, "300"→560, "400"→**750** (25% over the 600 MHz
  vendor ceiling; *throttling to "400 MHz" raises the clock*). Only `M=0` rates
  (200/300/400/600, exact from a periph output) are safe under the current model.
- Fix in review: series **"clk: sunxi-ng: fix the A523/T527 GPU clock model, enable GPU
  DVFS"** — a new `ccu_maskdiv` clock type, switch the A523 `gpu_clk` to it, drop the
  `pll-periph0-800M` parent (BSP: "GPU job fault"), drop `CLK_SET_RATE_PARENT`, and a mux
  notifier parking the GPU on `pll-periph0-600M` while `pll-gpu` retunes. Prep patch
  `clk: sunxi-ng: div: implement set_rate_and_parent` already has Chen-Yu's Reviewed-by.
- **Our stance:** *track, don't carry.* It's mainline-bound (generic `ccu_maskdiv` + the
  A523 CCU), still churning (v1; Sashiko AI flagged issues → expect v2+), and we can't
  runtime-verify without the device — so it flows in for free on a kernel rebase once merged,
  rather than as an out-of-tree patch. `dts/staging/trimui-gpu-opp.dtsi` is now gated on it.
  The turbo/speed-bin rows (648–888 MHz) run from `pll-gpu` and are **doubly** gated (they
  need the deferred follow-up too). This also *validates our clock-only OPP choice*: Juan's
  OPPs sit at a fixed 920 mV (the AXP `dcdc2` rail), exactly our shared-rail reasoning.

## Framing
- The GPU is **orthogonal to lighting the panel.** Panfrost gives 3D accel; the panel is
  brought up by the DE3.5/DSI/TCON work. You can have a KMS framebuffer with no GPU, and
  Panfrost does nothing to scan out pixels. → enable it whenever; it's off the critical path.
- Precedent: H616/H700 handhelds run Panfrost on mainline (Mali-G31). Same driver family.

## Measuring the *real* GPU clock on hardware (the method that caught the maskdiv bug)
The maskdiv bug above is invisible to `clk_summary`, which only shows the *programmed*
rate — i.e. the kernel's wrong belief. Juan (ut-slayer) caught it by measuring the
**actual** GPU frequency with the Mali's hardware **cycle counter** and comparing. We
want the same method in our bring-up kit to independently confirm the fix (and GPU DVFS
generally) on *our* silicon before trusting any GPU OPP. On mainline Panfrost:

- **Principle:** the Mali has a hardware cycle / `GPU_ACTIVE` counter that ticks at the
  *real* GPU clock. Run a sustained GPU load, read Δcycles over a known Δt →
  `real_MHz ≈ Δcycles / Δt`. Compare against `/sys/kernel/debug/clk/clk_summary` (the
  programmed rate). **A mismatch = clock mismodel** (exactly the linear-vs-masking bug —
  "400 MHz" programmed vs ~750 MHz measured).
- **Reading the counter — pick one:**
  - **fdinfo (simplest):** mainline Panfrost exposes `drm-cycles-fragment` /
    `drm-cycles-vertex-tiler` (plus `drm-curfreq-*` and `drm-maxfreq-*`) per engine in
    `/proc/<pid>/fdinfo/<fd>` of a running GPU client. `drm-curfreq` is the kernel's
    (possibly wrong) notion; the raw `drm-cycles-*` delta over wall-time is the hardware
    truth. Uses the "wire cycle counters + timestamp to userspace" Panfrost uAPI.
  - **Perfetto + gfx-pps** (Collabora): the Panfrost PPS producer samples Mali HW perf
    counters incl. GPU active-cycles. Needs Panfrost unstable ioctls
    (`panfrost.unstable_ioctls=1`). The heavier "proper profiler" path.
  - **Mesa timestamp queries:** `GL_ARB_shader_clock` / GL & Vulkan timestamp queries
    read the same wired-up counter in-shader; a known-cycle workload + wall time also
    yields real MHz.
- **Sweep each OPP like Juan did:** devfreq sysfs at `/sys/class/devfreq/1800000.gpu/` —
  set `governor` to `userspace` and write `set_freq`, or pin `min_freq`/`max_freq` — then
  read the cycle-counter real rate at each step and tabulate programmed-vs-measured.
- **Load generator:** `glmark2` or any sustained GLES/compute workload.

This belongs in the hardware bring-up kit (hardware testing prevails): it's how we
confirm the GPU clock is honest on the Trimui, independent of what the driver reports.

**Now wired into the kit:** `hw-verify.sh` has a **`gpu`** subsystem test that automates the
read-only state (Panfrost bind, `dmesg`, devfreq OPP ladder, `clk_summary` GPU rows) and
walks the guided programmed-vs-measured measurement above (pin an OPP on the `userspace`
governor → load with `glmark2` → sample `drm-cycles-*`/`drm-curfreq-*` from fdinfo twice
~2 s apart → `real_MHz ≈ Δcycles/Δt`). Its report row records the `programmed:measured`
pair and flags the maskdiv overclock if `measured > programmed`. Run: `sh hw-verify.sh gpu`.
That row is the concrete give-back to the ut-slayer / maskdiv effort — an independent
Mali-G57 confirmation of the fix.
