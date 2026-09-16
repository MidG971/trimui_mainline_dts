<!-- SPDX-License-Identifier: (GPL-2.0-only OR MIT) -->
<!-- Copyright (C) 2026 Midgy BALON -->

# Upstream-readiness assessment

Status of the display/PWM series toward mainline submission. Checked on the
build host against v7.1 with `checkpatch.pl` and `dt_binding_check`.
The work itself builds and dt-validates; the gaps below are about **submission
form** and **missing binding docs**, not driver correctness.

> **Update (2026-09):** this snapshot assessed the **display/PWM** series against a v7.1 base. The
> current first submissions are small standalone fixes (pinctrl / mmc-pwrseq / mfd-axp20x) on
> v7.3-rc3; the **display is not yet submission-ready** — colour (a YUV↔RGB conversion) and
> continuous-refresh robustness are still open. Read the scorecard below as the display/PWM
> readiness at the time, not the current submission plan.

## Scorecard

| Artifact | checkpatch | dt_binding_check | Notes |
|----------|-----------|------------------|-------|
| `phy-sun55i-dsi-combo.c` | clean | — | |
| `pwm-sun20i.c` | clean | — | greenfield (no D1 PWM-v2 in mainline yet) |
| `panel-trimui-smart-pro-s.c` | clean | — | |
| `allwinner,sun55i-a523-dsi-combo-phy.yaml` | — | clean | |
| `trimui,smart-pro-s-panel.yaml` | — | clean | |
| `allwinner,sun20i-d1-pwm.yaml` (new) | — | clean | added this pass — closes the pwm "no schema" gap |
| patches 0001–0008 | **0 errors** | — | regenerated as git format-patch + Signed-off-by; warnings remain (below) |

## checkpatch: the patch series

All three drivers are **clean**, and the **3 structural errors are now fixed**:
the series was regenerated through git (`format-patch`), so every patch now has a
proper `From`/`Date`/`Subject` envelope, a `---` separator + diffstat, real
`diff --git` hunks, and a `Signed-off-by: Midgy BALON` line. checkpatch now
reports **0 errors** on all 8 (verified on the host); they still apply cleanly
via `patch -p1` (8/8) so the build flow is unchanged.

Resolved this pass:
1. ~~"Avoid using diff content in the commit message" + "Invalid commit
   separator"~~ — fixed by the `git format-patch` envelope + `---` separator.
2. ~~"Missing Signed-off-by:"~~ — added (`Signed-off-by: Midgy BALON`, the repo
   author, at their direction).

Warnings still open (out of scope for the structural fix):
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
only a64, h6, h616, r329). We use a real **`sun55i_a523` BL31 from Jernej Škrabec's
`a523-v4` TF-A branch** — this cleared the earlier BL31 hang and is what boots on
hardware; it is not yet in TF-A mainline. Upstream (or switch to a merged A523 plat)
when one lands.
