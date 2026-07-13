<!-- SPDX-License-Identifier: (GPL-2.0-only OR MIT) -->
<!-- Copyright (C) 2026 Midgy BALON -->

# DisplayPort Alt Mode output (A523 / Trimui Smart Pro S) — planning notes

Planning doc for **external video-out over USB-C DisplayPort Alt Mode** (e.g. a
USB-C→HDMI adapter driving a TV/monitor).

**Status: NOT STARTED — Phase 6+.** It sits *behind* the internal MIPI-DSI panel
bring-up (shared display engine), which itself isn't confirmed lit on mainline yet.
No upstream A523 DP work exists by anyone — this would be the only effort, same as
the DSI/DE stack. **Spec-unblocked:** register map is in **UM v1.4 "eDP 1.3"**
section + the tina5.0_aiot BSP `sunxi_drm` eDP source; so it's "a lot of driver
work", not "blocked on missing info".

Facts below are from the vendor DTB (`vendor/trimui_smart_pro_source.dts`); mainline
status checked against v7.2-rc. Related: [USB-TYPEC-NOTES](USB-TYPEC-NOTES.md),
[DISPLAY-PORT-STATUS](DISPLAY-PORT-STATUS.md), [DISPLAY-NOTES](DISPLAY-NOTES.md).

---

## Port map (which port does what)

The board has **two USB-C ports but only ONE Type-C controller** (`husb311@4e`):

| Port (silk) | USB data | Charging (PD) | Video-out | Parts |
| :--- | :--- | :--- | :--- | :--- |
| **"usb/dp"** (multifunction) | USB2 OTG (`usbc0`, MUSB) | ✅ **yes — the only charge port** (`husb311` dual power-role, `sink-pdos`+`source-pdos`) | ✅ **DP Alt Mode** (SVID `0xff01`) | `husb311@4e` TCPC + `ps8743@11` USB3/DP mux + `drm_edp@5720000` DP source (via `tcon3@5504000`) |
| **"USB host"** | USB3 host (`dwc3@0x4d00000`, super-speed) | ❌ **no** — only *drives VBUS out* (`drvvbus-supply`, `aw,vbus-shared-quirk`); no TCPC/PD | ❌ none | `snps,dwc3` + **GMA340** SS redriver (sel=PH7, oe=PH8) |

**Key consequence:** the **only charging port is also the only video-out port**
(usb/dp). So "charge on one port + external display on the other" is impossible;
doing both at once requires a **USB-C dock** that provides DP-alt video **and** PD
charge-passthrough on the single usb/dp port (plus a hub for keyboard/mouse).

---

## Pipeline to bring up

```
DE3.5 (mixer/CRTC) ──► TCON3 ──► A523 eDP/DP source ──► ps8743 mux ──► USB-C ──► HDMI adapter ──► TV
  (shared with          (TV      (drm_edp@5720000)      (USB3/DP      (husb311 TCPC negotiates DP
   the internal          TCON,    allwinner,drm-dp)      orientation   Alt Mode SVID 0xff01 + PD)
   DSI panel)            @5504000)                        + redrive)
```

---

## Components & status

| # | Piece | Role | Mainline | Model / source | Effort |
| :-: | :--- | :--- | :--- | :--- | :--- |
| 0 | **husb311** TCPC (`@4e`) | PD + DP alt-mode negotiation | ✅ upstream (RT1711H fallback) | — (free) | — |
| 1 | **A523 eDP/DP source** (`drm_edp@5720000`, `edp0@5720000`) | DP transmitter: AUX (EDID), **link training**, DP encoder | ❌ greenfield — no sunxi DP/eDP driver upstream | BSP `sunxi_drm` eDP driver; UM v1.4 "eDP 1.3" regs | **Large** (long pole) |
| 2 | **TCON3** (`tcon3@5504000`, TV TCON) | Feeds pixels/timing from DE → DP source | ⚠️ `sun4i_tcon` exists; needs a sun55i **TCON3 (TV)** variant | Same pattern as our TCON-LCD1 work, TV path | Medium |
| 3 | **DE3.5 second output** | Mixer drives **two** outputs (DSI + TCON3/DP) as separate CRTCs | ⚠️ depends on DE3.5 bring-up done first | Extends our DE3.5 mixer cfg | Medium |
| 4 | **ps8743 mux** (`parade,ps8743`, `@11`) | Route/orient SS lanes to USB3 **or** DP by cable orientation | ❌ no driver | ~200–300 lines, model on `typec/mux/ps883x.c` | Small–medium |
| 5 | **Type-C DP alt-mode glue** | TCPM enters DP mode → set mux orientation + **pin assignment (C/D/E)** → hand **HPD** to the DRM connector | ⚠️ framework exists (`typec/altmodes/displayport.c`, `typec_mux`, DRM HPD); the **cross-subsystem wiring** is the fiddly bit | DP-over-Type-C pattern (some Qualcomm / RK3399 boards) | Medium |
| 6 | **DT wiring** | connector `altmodes` (SVID `0xff01`), mux, OF graph TCON3→edp→connector | ❌ to-do | straightforward once 1–5 exist | Small |

---

## Critical path / ordering

1. **Internal panel + DE3.5 must be lit first** — DP-out shares the display engine;
   no point until DE→TCON→DRM is a working device. (This is why DP-out sits behind
   the internal screen.)
2. **eDP/DP source driver (#1)** — the long pole: probe + AUX + link training vs a
   real sink.
3. **TCON3 + DE second CRTC (#2, #3)** — so the DE can feed the DP source.
4. **ps8743 mux (#4) + Type-C DP alt-mode glue (#5)** — so a plugged cable routes
   lanes + delivers HPD.
5. **DT (#6)** ties it together.

---

## Milestone ladder (how you know each step works)

1. Plug DP cable → `dmesg` shows the TCPM **enter DP Alt Mode** (SVID `0xff01`) —
   works today-ish (husb311 upstream).
2. **Mux switches** orientation on plug (ps8743 driver bound).
3. DP source gets **HPD**, reads **EDID** over AUX → monitor modes appear.
4. **Link training** succeeds → a `DP-1`/`eDP` connector goes `connected` in
   `/sys/class/drm/`.
5. `modetest` a solid pattern to the DP connector → **pixels on the TV**.
6. Compositor / RetroArch outputs the game to the external connector (GPU/Panfrost
   already upstream → once the connector exists this is userspace config).

---

## Scope (honest)

**Large — comparable to the whole DSI/DE internal-display bring-up**, and arguably
harder because of the USB-Type-C ↔ DRM alt-mode glue (#5), the part with the most
cross-subsystem sharp edges. No upstream A523 DP effort exists, so we'd be the only
one — but we are spec-unblocked (UM v1.4 eDP registers + BSP source).

`recon.sh` §7B captures the live Type-C alt-mode state on the stock OS
(`/sys/class/typec` SVIDs, husb311/ps8743 on i2c, DRM connectors, tcpm/dp dmesg) —
useful ground truth before starting.
