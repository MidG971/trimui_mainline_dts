<!-- SPDX-License-Identifier: (GPL-2.0-only OR MIT) -->
<!-- Copyright (C) 2026 Midgy BALON -->

# Contributing to the Trimui Smart Pro S mainline port

Thanks for wanting to help bring up the **Trimui Smart Pro S** (Allwinner A523,
board `A523-PRO2-AXP717C`, model TG5050) on mainline Linux.

This is an **experimental, kernel-mainline project**, and the goal goes beyond
first boot. We care about three things: **(1)** getting the device onto mainline
Linux, **(2)** making it a good **daily driver** — power, thermal, performance,
battery life, and usability tuning — and **(3)** keeping it **maintained against
future kernels** as upstream moves. Contributions toward any of the three are
welcome.

The bar for changes is the same as the bar this project holds itself to: a change
has to be **tested** and **reproducible**, and it has to be **honest** about what
is and isn't verified. Please read this whole file before opening a pull request —
it is short, and it will save us both a round trip.

Start with [`README.md`](README.md) (status table) and
[`PORTING-NOTES.md`](PORTING-NOTES.md) (the hardware truth table + phased
roadmap) so you know what already works, what is blocked on missing upstream
drivers, and what is deliberately *not* done yet.

---

## The two non-negotiables

Every contribution — code, device tree, bindings, docs, or hardware findings —
must be:

### 1. Tested

You ran it, and you say exactly how. **A clean `dtc` compile is not a test** —
`dtc` only checks syntax and phandle resolution; it does not validate register
addresses, `compatible` strings, or that any driver exists or binds. This has
bitten this project repeatedly, so "it compiles" alone will be sent back.

What "tested" means depends on what you touched — see
[How to validate your change](#how-to-validate-your-change) below. At minimum
your change must **build and pass the relevant validator** on the project's
baseline (mainline Linux **v7.1**). If your change affects hardware behavior and
you have a device, test it on the device. If you do **not** have a device, that
is fine — say so explicitly and state precisely what you *did* validate (build /
`dt-validate` / checkpatch). Marking work **"unverified on silicon"** is normal
and welcome here; silently implying it works is not.

### 2. Reproducible

Someone else must be able to repeat what you did from your description alone.
That means:

- **Exact commands and baseline.** State the kernel tree/tag (e.g. `v7.1`), the
  repo commit you branched from, the toolchain, and the literal commands you
  ran. "I built it" is not reproducible; the command that built it is.
- **Provenance for every hardware fact.** Do not invent nodes, addresses, GPIOs,
  compatibles, or magic numbers. Each hardware claim must be traceable to a
  source: the decompiled vendor DTB (cite the line/property), the A523 User
  Manual / datasheet (cite the page or register), an upstream sibling board, or
  a reading taken from the device (cite the command and paste the output).
  Fabricated-but-plausible values are the single most common defect in earlier
  attempts (see `PORTING-NOTES.md` §2). If you can't cite it, mark it `VERIFY`
  rather than presenting it as fact.

These two are **not negotiable**. A change that can't be reproduced or wasn't
tested won't be merged, however good the idea.

---

## Using AI assistants

**AI assistance is allowed and welcome** on this project — for code, device
tree, bindings, research, or writing. There are exactly two conditions, and they
are not optional:

1. **Disclose it.** Say in the pull request that AI was involved, and ideally
   which tool/model and for what (e.g. "drafted the panel driver with an LLM,
   then hand-reviewed and tested"). A trailer in the commit message is great:

   ```
   Assisted-by: <tool / model name>
   ```

2. **It meets the same two non-negotiables above.** AI output is held to the
   *exact same* tested + reproducible bar — no exceptions, and realistically a
   bit more scrutiny, because LLMs are very good at producing plausible,
   confident, **wrong** hardware facts (invented register addresses, GPIO pins,
   `compatible` strings). You are responsible for everything you submit. Verify
   every fact against a real source (see *Provenance* above), build it, and run
   the validators **before** you open the PR. Do not paste AI-generated hardware
   values you haven't checked.

Undisclosed AI use, or AI output that wasn't tested/verified, is the one thing
that will get a PR closed on principle rather than on its merits.

---

## What contributions are most useful

- **Hardware findings from a real device** — the project is largely pre-hardware
  bring-up, so anything observed on actual silicon is gold. Run
  [`recon.sh`](recon.sh) (read-only) on the stock OS over `adb`, or capture
  serial/`dmesg`/`i2cdetect`/`evtest` output, and file it (see the
  *Hardware bring-up* issue template). Cite how you captured it.
- **Verifying things currently tagged 🟡 / `VERIFY`** — confirming or correcting
  a rail, GPIO, address, or battery value against real hardware.
- **Driver / device-tree work** for the still-blocked subsystems (display
  DE3.5, audio, GPADC sticks, USB3) — coordinate via an issue first, these are
  large and several track in-flight upstream series.
- **Daily-driver optimization** — once a subsystem works, making it *good*:
  CPU/GPU DVFS + OPP tuning, thermal/fan curves, charging behaviour and battery
  life, suspend/resume, audio quality, input latency, boot time. Back claims with
  before/after numbers and how you measured them (the same reproducibility bar
  applies to performance work).
- **Future-kernel maintenance** — rebasing the patch series onto a newer release,
  fixing API churn, and tracking the in-flight upstream series we depend on
  (display, GPADC, THS thermal, USB3) so we can drop out-of-tree carries as they
  land. Note which kernel version you validated against.
- **Docs and provenance** — improving `PORTING-NOTES.md` / `docs/` with cited
  facts. The [project wiki](https://github.com/MidG971/trimui_mainline_dts/wiki)
  summarises these for readers; if you edit it, see
  [`docs/WIKI-MAINTENANCE.md`](docs/WIKI-MAINTENANCE.md) for how it's structured
  and how to check a page actually renders.

---

## How to validate your change

Pick the rows that apply. Run them on a mainline **v7.1** tree (the project
baseline) before opening the PR, and paste the relevant output into the PR.

### Device tree (`dts/`)

- Local syntax/phandle check (fast, **not** a test on its own):
  ```sh
  ./compile.sh
  ```
- Real validation against the kernel schema — copy the board DTS (and
  `dts/trimui-panel.dtsi`) into a v7.1 tree's
  `arch/arm64/boot/dts/allwinner/`, add it to that Makefile, then:
  ```sh
  make ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- CHECK_DTBS=y dtbs
  ```
  Note: build the *board* DTB target via the dir's `dtb-y +=` entry; don't pass
  a full path to `make <path>.dtb` (kbuild doubles the path).
- `dt-validate` should report no real errors. "no schema" lines are expected
  only for compatibles whose bindings live in this repo (panel / combo-phy /
  pwm / codec) and aren't applied to the tree — note that if it's the case.

### Kernel drivers / patch series (`kernel/`)

- Build clean, with warnings on (`W=1`), against v7.1:
  ```sh
  ./kernel/build-trimui-kernel.sh <path-to-v7.1-kernel-src>
  # internally: make ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- W=1 ...
  ```
- Patches must be `git format-patch` form with a `Signed-off-by` line and pass
  checkpatch:
  ```sh
  scripts/checkpatch.pl --strict kernel/patches/00XX-*.patch
  ```
- One logical change per patch; follow kernel coding style and commit-message
  conventions (`subsystem: short imperative summary`).

### DT bindings (`kernel/bindings/`)

```sh
make ARCH=arm64 dt_binding_check DT_SCHEMA_FILES=path/to/your.yaml
dt-doc-validate path/to/your.yaml
```
Both must pass. (A clean `make dtbs` does **not** validate bindings — this has
also bitten the project; validate the binding explicitly.)

---

## Hard rules — things that must never land in the repo

These are enforced by `.gitignore` and by review. Don't try to work around them:

- **No Allwinner Confidential documents.** The A523 datasheet / User Manual
  PDFs are confidential — keep them local, never commit (`*.pdf` is git-ignored).
  Cite them by page/section in docs instead.
- **No binaries.** No `*.bin` / `*.img` / `*.awimg` / `*.fex`, no built DTBs, no
  U-Boot/firmware blobs. The repo is source-only; binaries ship via GitHub
  Releases once HW-validated.
- **No vendor firmware or the decompiled vendor device tree.** Cite *facts*
  observed from it (re-expressed in original form); don't commit the vendor
  artifact itself.
- **No private network info.** Never commit IP addresses (especially Tailscale /
  internal hosts) — refer to the build host only by hostname. History has had to
  be rewritten once already to scrub a leaked IP; don't repeat it.
- **No private personal data** of anyone, yours or others'.

## Licensing, SPDX, and sign-off

- The project is dual-licensed **`(GPL-2.0-only OR MIT)`**; kernel driver sources
  are **`GPL-2.0-only`** (kernel convention). See [`LICENSE`](LICENSE) and
  [`NOTICE`](NOTICE).
- **Every new file needs an SPDX header** matching the surrounding convention:
  - C sources: `// SPDX-License-Identifier: GPL-2.0-only` (or `GPL-2.0`)
  - Docs / YAML / shell: a leading `SPDX-License-Identifier: (GPL-2.0-only OR MIT)`
    comment, plus `Copyright (C) <year> <you>`.
- By contributing you agree your contribution is licensed under the project's
  terms (inbound = outbound), and you certify the
  **[Developer Certificate of Origin](https://developercertificate.org/)** by
  signing off your commits:
  ```sh
  git commit -s
  ```
  which adds `Signed-off-by: Your Name <your@email>`. This is required on kernel
  patches anyway, so it's the same workflow throughout.

## Pull request workflow

1. Open an **issue** first for anything non-trivial (especially the big blocked
   subsystems) so we don't duplicate in-flight work.
2. Fork, branch from `main`, make **one focused change** per PR.
3. Run the validators above; keep the device tree honest (`VERIFY` tags, cited
   facts, no fabricated nodes).
4. Fill in the **pull request template** completely — the testing, reproduction,
   and AI-disclosure boxes are the ones that matter most.
5. Be patient and kind in review (see the [Code of Conduct](CODE_OF_CONDUCT.md)).
   Expect questions about provenance and reproduction — that's the whole point.

Thank you for helping :).
