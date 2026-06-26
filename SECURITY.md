<!-- SPDX-License-Identifier: (GPL-2.0-only OR MIT) -->
<!-- Copyright (C) 2026 Midgy BALON -->

# Security Policy

This is an **experimental, hobby bring-up project** for the Trimui Smart Pro S
(Allwinner A523). It ships device trees, kernel driver sources/patches, and a
U-Boot configuration — low-level code that, if wrong, **can permanently brick a
device** (see the disclaimer in [`README.md`](README.md) and [`NOTICE`](NOTICE)).
We take that seriously and welcome reports.

## Supported versions

Only the latest `main` is maintained. The tree tracks mainline Linux (current
baseline **v7.1**) and is rebased forward; older commits/tags receive no fixes.

| Version            | Supported |
| ------------------ | :-------: |
| `main` (latest)    | ✅        |
| Older tags/commits | ❌        |

## What counts as a security issue here

Because this is firmware/kernel-level work, "security" is broader than the usual
web sense. Please report:

- A change that can **brick, corrupt, or physically damage** hardware beyond the
  inherent, documented experimental risk — e.g. out-of-spec DRAM timings or
  regulator voltages, or a flash/partition path that can destroy the bootloader.
- A **secret or private detail accidentally committed** — keys, tokens, IP
  addresses (including Tailscale / internal hosts), or personal data (see the
  hard rules in [`CONTRIBUTING.md`](CONTRIBUTING.md)).
- A genuine memory-safety or privilege bug in the **driver code under
  [`kernel/`](kernel/)**.
- A **supply-chain** concern with anything the project pulls in (e.g. the
  out-of-tree AIC8800 WiFi/BT module, the build scripts).

Out of scope here (report elsewhere):

- Bugs in the device's **stock vendor firmware/OS** → report to Trimui / Allwinner.
- Bugs in **upstream Linux or U-Boot** themselves → report to those projects.
- The documented, accepted risk that flashing or FEL-booting experimental
  firmware can brick the device — that is the project's standing disclaimer, not
  a vulnerability.

## How to report

**Please report privately — do not open a public issue for a security matter.**

1. **Preferred — GitHub Private Vulnerability Reporting:** go to this repository's
   **Security** tab → **Report a vulnerability**. This keeps the report private
   to the maintainer until a fix is ready.
2. **Fallback — email:** **midgy971@gmail.com** with `SECURITY` in the subject.

Please include, as far as you can:

- The affected file(s) and commit (`git rev-parse --short HEAD`).
- Kernel / U-Boot version and the hardware involved (if any).
- What the issue is, its impact, and how to reproduce it (cited like any other
  change — see [`CONTRIBUTING.md`](CONTRIBUTING.md)).
- Any suggested fix.

## What to expect

This is a single-maintainer hobby project, so there is **no formal SLA** — but
reports are taken seriously. Expect a **best-effort acknowledgement**, a fix or
mitigation on `main` once confirmed, and credit in the commit/advisory unless you
prefer to remain anonymous. Please allow a reasonable chance to fix before public
disclosure (coordinated disclosure).

Thank you for helping keep people's devices safe.
