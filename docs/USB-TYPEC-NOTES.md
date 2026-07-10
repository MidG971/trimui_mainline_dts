<!-- SPDX-License-Identifier: (GPL-2.0-only OR MIT) -->
<!-- Copyright (C) 2026 Midgy BALON -->

# USB-C / Type-C notes (A523 / Trimui Smart Pro S)

The board has **two USB-C ports**. Facts below are from the vendor DTB
(`vendor/trimui_smart_pro_source.dts`); mainline driver status was checked against
v7.2-rc1. Live state is captured by `recon.sh` §7B on the stock OS.

| Port (silk) | Role | Key parts |
| :--- | :--- | :--- |
| USB host | USB3 host (super-speed) | `dwc3@0x04d00000` (`snps,dwc3`, `dr_mode=host`) + **GMA340** SuperSpeed redriver (sel=PH7, oe=PH8) |
| **"usb/dp"** | USB data + **DisplayPort alt-mode** + USB-PD | `husb311` TCPC + `ps8743` USB3/DP mux + `drm_edp` DP source (via TCON3) |

## The "usb/dp" port — DisplayPort Alt Mode chain

It is **not** power-only. The vendor DTB wires a full DP-over-Type-C path:

```
TCON3 -> drm_edp@5720000 (allwinner,drm-dp, 4-lane)     DisplayPort source
      -> husb311@4e  (usb-c-connector, altmode svid=0xff01 = DisplayPort, PD sink/source PDOs)
      -> ps8743@11   (parade,ps8743, orientation-switch, svid=0xff01)   USB3/DP mux
      -> USB-C connector
```

`svid = 0xff01` is the VESA DisplayPort SVID, so the connector genuinely advertises
DP Alt Mode. `data-role`/`power-role` are both `dual` with PD PDOs → the port does
USB data, DP video-out, and USB-PD charging.

## Mainline driver status (checked v7.2-rc1)

| Component | Role | Mainline? | Action |
| :--- | :--- | :--- | :--- |
| **husb311** (Hynetek) | Type-C port controller (PD, alt-mode negotiation) | ✅ **Yes** — a rebrand of **Richtek RT1711H**, pin/register compatible; handled by `tcpci_rt1711h.c` (binding `richtek,rt1711h.yaml` documents `hynetek,husb311`) | Just DT — use the **fallback compatible** below |
| **ps8743** (Parade) | USB3.1/DP mux + redriver (orientation) | ❌ **No** — absent from the tree. Closest sibling is `drivers/usb/typec/mux/ps883x.c` (Parade PS8830) | Write a small `typec_mux`/`switch`/`retimer` driver modeled on `ps883x.c` |
| **drm_edp** (A523 DP/eDP source) | DisplayPort source (fed by TCON3) | ❌ **No** — greenfield, same class as the DSI/DE display work | Port from BSP; rides on the [display bring-up](DISPLAY-PORT-STATUS.md) |
| **dwc3** (USB3 host) + USB3/PCIe combo-PHY | Super-speed host | ⏳ In-flight (Kalashnikov "a523: add USB3.0 support", needs v2; not in v7.1/v7.2) | Adopt when merged; see `dts/staging/trimui-usb3.dtsi` |

### DT: husb311 compatible

The vendor DTB uses `compatible = "hynetek,husb311"` alone. Mainline binds it via the
RT1711H fallback, so the mainline node must be:

```dts
usb-pd@4e {
	compatible = "hynetek,husb311", "richtek,rt1711h";
	/* reg, interrupt, connector { altmodes { ... svid = <0xff01>; } } */
};
```

## Staging / order of work

1. **Now (no display needed):** USB2 data + USB-PD charging on the "usb/dp" port work
   with only the husb311/RT1711H TCPC — upstream, no driver to write.
2. **USB3 host:** adopt the upstream A523 USB3 series when it merges (`trimui-usb3.dtsi`).
3. **DP-out (Phase 6+):** gated on two of our own pieces — the **ps8743 mux driver** and
   the **A523 DP/eDP source** driver — plus the Type-C/DP-altmode glue. It rides on the
   display bring-up; the internal MIPI-DSI panel stays the priority.

`recon.sh` §7B captures the live Type-C alt-mode state (`/sys/class/typec`), the
husb311/ps8743 i2c devices, DRM connectors, and `tcpm`/`dp` dmesg to confirm all this on
the real device.
