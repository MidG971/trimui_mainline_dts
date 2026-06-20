<!-- SPDX-License-Identifier: (GPL-2.0-only OR MIT) -->
<!-- Copyright (C) 2026 Midgy BALON -->

# Upstream-readiness assessment

Status of the display/PWM series toward mainline submission. Checked on the
build host against v7.1 with `checkpatch.pl` and `dt_binding_check`.
The work itself builds and dt-validates; the gaps below are about **submission
form** and **missing binding docs**, not driver correctness.

## Scorecard

| Artifact | checkpatch | dt_binding_check | Notes |
|----------|-----------|------------------|-------|
| `phy-sun55i-dsi-combo.c` | clean | — | |
| `pwm-sun20i.c` | clean | — | greenfield (no D1 PWM-v2 in mainline yet) |
| `panel-trimui-smart-pro-s.c` | clean | — | |
| `allwinner,sun55i-a523-dsi-combo-phy.yaml` | — | clean | |
| `trimui,smart-pro-s-panel.yaml` | — | clean | |
| `allwinner,sun20i-d1-pwm.yaml` (new) | — | clean | added this pass — closes the pwm "no schema" gap |
| patches 0001–0008 | **3 errors each** | — | structural (see below), not code |

## checkpatch: the patch series

All three drivers are **clean**. Every patch in `kernel/patches/` trips the same
three structural errors, because the files are hand-written `patch -p1` patches,
not `git format-patch` output:

1. **"Avoid using diff content in the commit message"** + **"Invalid commit
   separator"** — there is no `---` separator between the message and the diff,
   and no `From `/`Subject:`/diffstat envelope. Fixed by regenerating the series
   with `git format-patch` once the changes are committed to a branch.
2. **"Missing Signed-off-by:"** — a DCO certification. **This is the author's to
   add** (`git commit -s`, Midgy BALON); it must not be added on their behalf.

Warnings worth fixing before posting:
- **Bindings belong in their own patch** (0001 DSI, 0004 TCON, 0008 mixer mix a
  binding change with the driver/dts change). Split each binding hunk out.
- Wrap commit descriptions at ≤75 cols (0001, 0003, 0004).
- 0004: drop commit-log lines starting with `#` (git treats them as comments).
- 0007: give the Kconfig symbol a ≥4-line help paragraph.

## Binding coverage

- combo-phy, panel and the new pwm binding all pass `dt_binding_check`.
- These bindings live in `kernel/bindings/` and are **not** applied to the kernel
  `Documentation/` in our build flow, which is why a plain board `dt-validate`
  still prints benign "no schema" lines for `combo-phy`, `panel` and (until now)
  `pwm`. For upstream each ships as its own "dt-bindings:" patch.
- The board top-level compatible (`trimui,smart-pro-s`) would be added to
  `arm/sunxi.yaml` at submission time.

## Boot chain — TF-A

Re-checked TF-A master: **still no A523/sun55i platform** (`plat/allwinner` has
only a64, h6, h616, r329). We continue to use the **H616 BL31 stand-in** (fine
for console/boot; revisit SMP/PSCI specifics on HW). No upstream change.

## Out-of-tree: AIC8800 (WiFi/BT)

Module build against v7.1 not yet exercised on the host (Radxa package V5.0 was
noted to build on 6.19). TODO: clone + build vs v7.1, stage as DKMS. Tracked
here so it isn't lost; not on the critical path.

## Action checklist

Mechanical (can be done without HW):
- [x] Add the PWM binding (`allwinner,sun20i-d1-pwm.yaml`).
- [ ] Split binding hunks out of 0001/0004/0008 into dedicated patches.
- [ ] Fix commit-message wrapping / `#` lines / Kconfig help text.

Author's call (legal / workflow decisions — not automatable):
- [ ] `Signed-off-by` (DCO) on every patch.
- [ ] Regenerate the series with `git format-patch` for posting (vs. the current
      `patch -p1` form the repo keeps for convenience).
- [ ] Decide submission grouping/order and CC the sunxi + DRM maintainers.

HW-gated (later):
- [ ] AIC8800 module build/stage; TF-A A523 BL31 (when upstream gains a plat).
