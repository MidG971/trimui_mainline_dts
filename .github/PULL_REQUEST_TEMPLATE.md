<!-- SPDX-License-Identifier: (GPL-2.0-only OR MIT) -->
<!-- Copyright (C) 2026 Midgy BALON -->
<!--
  Please fill in every section. The Testing, Reproducibility and AI sections are
  the ones that get a PR merged or sent back. See CONTRIBUTING.md.
-->

## What this change does

<!-- One or two sentences. Link the issue it addresses, if any (e.g. Closes #12). -->

## What I touched

- [ ] Board / staging device tree (`dts/`)
- [ ] Kernel driver or patch series (`kernel/`)
- [ ] DT binding (`kernel/bindings/`)
- [ ] Optimization / tuning (power, thermal, performance, daily use)
- [ ] Rebase onto a newer kernel / upstream-tracking
- [ ] Docs / notes
- [ ] Other: <!-- describe -->

---

## ✅ Tested — *non-negotiable*

> A clean `dtc` compile is **not** a test. State exactly what you ran. See
> CONTRIBUTING.md → "How to validate your change".

**Baseline:** mainline Linux `vX.Y` (project baseline: **v7.1**) · repo commit `________`
· toolchain `________`

Validation I ran (tick what applies, paste output/links below):

- [ ] `./compile.sh` (DTS syntax/phandle — syntax only)
- [ ] `make CHECK_DTBS=y dtbs` → `dt-validate` clean (no real errors)
- [ ] `make dt_binding_check` + `dt-doc-validate` on the binding
- [ ] `checkpatch.pl --strict` clean on the patch(es)
- [ ] Kernel builds `W=1` with no new warnings (`build-trimui-kernel.sh`)
- [ ] Optimization/tuning change: before/after numbers + how I measured them

```text
<!-- paste the key command(s) + relevant output here -->
```

**On real hardware?**

- [ ] Tested on a physical Trimui Smart Pro S — board/fw: `________`, result: `________`
- [ ] **Not** tested on silicon — validated by build/dt-validate/checkpatch only
      (this is fine; just don't imply it works on hardware)

## 🔁 Reproducible — *non-negotiable*

- [ ] The commands above are sufficient for someone else to reproduce my result.
- [ ] **Every hardware fact is cited** (vendor DTB line / User Manual page / device
      reading + command / upstream sibling). No invented nodes, addresses, GPIOs,
      compatibles, or magic numbers. Unconfirmed values are tagged `VERIFY`.

Provenance for any new hardware facts:

<!-- e.g. "reg 0x34 on r_i2c0: vendor DTB pmu@34; confirmed by i2cdetect (output below)" -->

## 🤖 AI assistance

- [ ] No AI was used.
- [ ] AI was used — tool/model: `________`, for: `________`.
      I reviewed and **tested** all of it, and verified every hardware fact against
      a real source (AI output is held to the same tested + reproducible bar).

<!-- Optional commit trailer: Assisted-by: <tool/model> -->

## Housekeeping

- [ ] Commits are signed off (`git commit -s`, DCO).
- [ ] New files have the correct **SPDX header** + copyright (C `GPL-2.0-only`;
      docs/yaml/shell `(GPL-2.0-only OR MIT)`).
- [ ] No confidential PDFs, no binaries (`*.bin/.img/.dtb`), no vendor firmware /
      decompiled vendor DTB, **no IP addresses**, no private personal data.
- [ ] One focused change; kernel patches follow style + commit-message conventions.
