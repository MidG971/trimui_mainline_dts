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
  150/200/300/400/600 MHz). Not required for basic accel; the siblings ship without one.

## Framing
- The GPU is **orthogonal to lighting the panel.** Panfrost gives 3D accel; the panel is
  brought up by the DE3.5/DSI/TCON work. You can have a KMS framebuffer with no GPU, and
  Panfrost does nothing to scan out pixels. → enable it whenever; it's off the critical path.
- Precedent: H616/H700 handhelds run Panfrost on mainline (Mali-G31). Same driver family.
