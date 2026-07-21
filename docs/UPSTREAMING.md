<!-- SPDX-License-Identifier: (GPL-2.0-only OR MIT) -->
<!-- Copyright (C) 2026 Midgy BALON -->

# Upstreaming plan

The order and routing for getting this port into mainline Linux, once the device
boots mainline and each feature is verified on hardware. This is the *execution*
plan; [`UPSTREAM-READINESS.md`](UPSTREAM-READINESS.md) is the per-item scorecard.

## The one rule: hardware-tested first

sunxi/upstream culture is **tested-on-hardware-first**. Almost nothing lands before
the board boots mainline and the feature is verified on the device — maintainers
(Andre Przywara, who leads A523 mainlining; Chen-Yu Tsai; Jernej Skrabec; Samuel
Holland) will ask "tested on HW?" first. So the whole [Bring-Up Runbook](HARDWARE-BRINGUP.md)
comes **before** any submission. Every patch states how it was tested.

## Maintainer guidance (from linux-sunxi, 2026-07-21)

Andre Przywara (leads A523 mainlining) reviewed the ut-slayer / OrangePi-4A
series on `linux-sunxi`. His guidance — most of which **validates the approach
already taken here** — is now binding on our submissions:

- **Base on the latest kernel, never an LTS.** Patches must apply on the latest
  `-rc1` (currently **v7.2-rc**) or even `linux-next`, *not* 6.18-LTS. (We already
  forward-ported the whole series to v7.2-rc3 — this is our main structural
  advantage over the 6.18.38-based ut-slayer tree.)
- **Submit as a proper git branch + `git send-email`,** not patch files staged in
  a repo. Our `kernel/patches/*.patch` is a *working/staging* format only; the
  actual submission is git commits (English messages, `Signed-off-by:`) routed by
  `scripts/get_maintainer.pl`.
- **Group into small, per-subsystem series.** A 100+-patch dump "makes no friends"
  (there's a review bottleneck). pinctrl / clocks / thermal / display each go
  separately.
- **BSP = register lookup only.** Reading undocumented registers from the Allwinner
  BSP is fine; BSP *code / approaches* are "far from mainline quality" — don't port
  them. (Our tcs4838 = BSP register layout, rewritten as a standard `fan53555`
  variant = the acceptable case.)
- **Framing:** "add Trimui Smart Pro S support" / "improve A523 support", not
  "mainline bring-up" (the A523 SoC is already supported; board work is mostly
  enabling peripherals in DT + the genuinely-new handheld drivers).
- **Display is not urgent and is sequenced:** the **H616 DE33 support must merge
  upstream first**, then A523 DE follows as a clean extension. BSP-derived DE/RCQ
  code is **not upstreamable**. → our DE-v35x adoption is a **local screen-lighting
  path only**; see [DE35-ADOPTION-NOTES.md](DE35-ADOPTION-NOTES.md).
- **Coordinate with the ut-slayer effort — do NOT duplicate.** Andre asked *Juan*
  to break out the generic fixes (pinctrl, watchdog restart-priority, AXP717
  poweroff, the RESET_GPIO/mmc-pwrseq `#gpio-cells=3` bug, `ccu_div`
  set_rate_and_parent). Several of those are the ones we **adopted** from his tree
  (our 0024/0025 + the RESET_GPIO config) — so they are **his to upstream**, not
  ours. Our lane = the handheld-specific parts + acting as a second sun55iw3
  tester. Track his fixes; when they merge, they flow into our rebase.
- **Join `#linux-sunxi` on OFTC** for quick direction before starting work.

## Routing (who gets what)

Run `scripts/get_maintainer.pl` on each patch; the table is the shape of it.

| What | List(s) / tree | Maintainers |
| :--- | :--- | :--- |
| Board DTS + SoC peripheral nodes (`sun55i-a523.dtsi`) | `linux-sunxi@lists.linux.dev`, `devicetree@`, `linux-arm-kernel@` → sunxi tree | Chen-Yu Tsai, Jernej Skrabec, Andre Przywara |
| Display drivers (DSI host, combo-PHY, TCON, panel, DE3.5) | `dri-devel@`, drm-misc | DRM/sun4i + panel maintainers |
| Audio codec (`sun55i-codec`) | `alsa-devel@` | Mark Brown (ASoC) |
| PWM driver (`pwm-sun20i`) | `linux-pwm@` | Uwe Kleine-König |
| PHY drivers | `linux-phy@` | Vinod Koul |

## Submission order (smallest reviewable units first)

1. **Board DTS — minimal boot.** CPU, DRAM, UART console, MMC (microSD + eMMC),
   PMIC + regulators, USB2. Plus the board compatible in
   `Documentation/devicetree/bindings/arm/sunxi.yaml`. This is the anchor everything
   else hangs off, and the first thing to land.
2. **Easy peripherals**, each its own small patch once verified on HW:
   - **LRADC keys** — driver already upstream (`sun50i-r329-lradc` variant); needs
     the 3 button voltages measured on the device. Likely the quickest win. (Add an
     `allwinner,sun55i-a523-lradc` compatible + `sun50i-r329-lradc` fallback.)
   - **LEDC** RGB array, **PWM fan + vibrator**, **GPADC sticks** (rebase onto the
     v7.2 gpadc), **WiFi/BT power sequencing** (the DT hooks only — see out-of-tree below).
3. **Adopt in-flight series** rather than duplicating (you're on `linux-sunxi` — track them):
   - **THS thermal** — the existing series is
     [iuncuim/Kalashnikov, lore 20260704](https://lore.kernel.org/linux-sunxi/20260704171411.1413349-1-iuncuim@gmail.com/)
     (Andre pointed here explicitly). **Review/respond on the list; do NOT submit our
     own** THS driver. Our contribution is only the `pwm-fan` cooling-map + board zones
     on top, once the driver merges. (We carry ut-slayer's THS as our 0021-0023 for the
     *local* build only.)
   - **USB3** (Kalashnikov) — enable `&combophy`/`&dwc3` + the Trimui GMA340 mux on top.
4. **The big new drivers** — their own series to their subsystems:
   - **Display stack** — **NOT urgent, and sequenced behind H616.** Upstream wants the
     **H616 DE33 support merged first**, then A523 DE follows as a clean extension —
     *not* a BSP-derived RCQ port. So upstream = {DSI host variant → combo-PHY → TCON →
     panel} can go early to DRM, but the **DE3.5 mixer/CRTC waits on H616 DE** landing.
     Largest, most novel, longest review — budget for many revisions.
   - **Audio codec** → ASoC. **PWM driver** → linux-pwm.

## Per-submission checklist

- `scripts/checkpatch.pl --strict` — 0 errors.
- `make dt_binding_check DT_SCHEMA_FILES=…` + `make CHECK_DTBS=y dtbs` — clean.
- One logical change per patch; cover letter for a series; **changelog between versions**.
- Send with `b4` / `git send-email`; `Signed-off-by` on every patch; CC from get_maintainer.
- Say **how you tested it on hardware** — that is the currency of review.
- Expect v2/v3+ (the THS series we tracked reached v5). Address each comment, resend.

## Stays out-of-tree (not upstreamable)

- **AIC8800 WiFi/BT** — vendor driver, not upstreamable. Only its DT hooks (`&mmc1`
  pwrseq, `&uart1`) ride in with the board; the modules stay in `kernel/aic8800/`.

## Out of scope — unused on this board

These SoC blocks exist but are **disabled / unwired** on the Trimui, so there's
nothing to submit (see [[Hardware Overview]] / Roadmap "Out of scope"):

- **NPU**, **E906 RISC-V**, **HiFi4 DSP** — present but dormant, no runtime, no mainline driver.
- **PCIe** (`pcie@4800000`, `allwinner,sunxi-pcie-v210-rc`, 1-lane Gen2) — **`disabled`**
  in the vendor DTB. The Trimui's WiFi is **SDIO** (AIC8800), not PCIe, and nothing else
  is wired to it. It shares the `phy@4f00000` Innosilicon combo-PHY with **USB3** — and
  the board uses the *USB3* half, not the PCIe half. So PCIe is not addressed. (If a future
  board ever needed it, both the `sunxi-pcie-v210-rc` controller and the PCIe side of the
  combo-PHY would need mainline drivers — greenfield; Kalashnikov's USB3 combo-PHY work is
  the natural starting point.)

## Reality

Upstreaming is a **months-long, piece-by-piece loop**: rebase → verify on HW → submit
→ address review → resend, plus forward-porting as new kernels release. Board DTS,
LRADC, and LEDC land fast; the display stack is a long haul; AIC8800 rides out-of-tree
indefinitely.
