<!-- SPDX-License-Identifier: (GPL-2.0-only OR MIT) -->
<!-- Copyright (C) 2026 Midgy BALON -->

# DE3.5 (DE-v35x) display — adoption plan (ut-slayer → our DSI panel)

The DE3.5 mixer/CRTC ("lit pixel") was our biggest display blocker. The
**ut-slayer / OrangePi-4A** effort has a **complete, HW-proven DE-v35x driver**
(`sun55i_de.c`) — but for **HDMI-out** (TCON-TV + DW-HDMI). We reuse the **shared
DE core** and wire it to **our internal MIPI-DSI panel** (TCON-LCD + our DSI host
/ combo-PHY / panel). Source cloned on `compiler-rock3b:/root/opi4a`.

**This is a deliberate integration, not a clean add** — their display series
*overlaps and partially supersedes* ours. Bulk-applying it over our `0001–0008`
would conflict on `sun8i_mixer.c`, `sun4i_tcon.c`, and the SoC dtsi. And it is
**HW-gated** — a lit pixel can only be confirmed on silicon
([[hardware-testing-prevails]]).

## Patch triage (ut-slayer numbering)

### Adopt — the shared DE-v35x core + A523-generic deps
| Group | Patches | Notes |
| :--- | :--- | :--- |
| **DE-v35x driver** | 0030, 0033, 0034, 0046, 0051, 0052, 0053, 0054, 0055, 0059 | `sun55i_de.c/.h` (RCQ backend, VSU, blender/formatter), `sun4i_crtc.c`, layers |
| **Mixer cfg** | 0040 | `sun55i_a523_mixer0_cfg` (`DE33, vi_num=1, ui_num=3, mod_rate=600M`) — **supersedes our patch 0008** |
| **display-engine glue** | 0024 | `sun4i_drv.c` — `allwinner,sun50i-h6-display-engine` reuse |
| **IOMMU** (scanout) | 0025, 0029, 0047, 0049, 0050 | `sun50i-iommu.c` PHYS_OFFSET/PTE fixes — A523-generic |
| **CCU display clocks** | 0026, 0027, 0028 | `ccu-sun55i-a523.c` DE/TCON clocks + resets |
| **SoC DE dtsi** | 0036, 0037, 0056 | `de: display-engine` + `bus@5000000` + `mixer@100000` + `tcon-top@5500000` — **reconcile with our 0003; rewire the tcon-top output to our TCON-LCD, not their TCON-TV** |

### Skip — HDMI-specific (we use DSI, not HDMI)
`sun8i_hdmi_phy.c` (0023/31/35/39/41), `sun8i_dw_hdmi` (0031), TCON-TV bits of
0022, and all their `sun55i-t527-orangepi-4a.dts` board patches. (Their config
`0016`/`0040`.config — we use `kernel/trimui.config`.)

## Reconciliation with our existing display work
| Ours | Decision |
| :--- | :--- |
| `0001` DSI host, `0002` combo-PHY, `0007` panel | **KEEP** — DSI-only, no overlap with their HDMI path |
| `0004` TCON-LCD | **KEEP, reconcile** — their `0022/0032` also touch `sun4i_tcon.c` (TCON-TV); merge so both TCON-LCD (ours, → DSI) and their tcon-top coexist |
| `0008` mixer cfg | **DROP** — replaced by their `0040` + `sun55i_de.c` |
| `0003` SoC display pipeline dtsi | **RECONCILE** — keep our DSI/TCON-LCD nodes; take their `de`/`bus`/`mixer`/`tcon-top` nodes; wire `tcon_top` → our `tcon_lcd1` → our `dsi1` → panel (instead of their tcon-tv → HDMI) |

## Execution order (when we do it)
1. Fresh v7.2-rc3 + our non-display patches (CPU-CCU etc.).
2. Apply the DE core + IOMMU + CCU-display + `sun4i_drv` groups above.
3. Re-apply our DSI stack (`0001/0002/0004/0007`), dropping `0008`, reconciling `0004`.
4. Build the SoC DT: their `de`/`bus`/`mixer`/`tcon-top` + our `tcon-lcd`/`dsi`/`panel`,
   with the OF-graph routed **tcon-top → tcon-lcd → dsi → panel**.
5. `make dtbs` + `dt-validate` + compile the DE/mixer objects.
6. **On hardware:** `modetest` a test pattern → the internal panel lights.

## Scope
Comparable to a full display bring-up (~15 patches to adopt + reconcile + the
HDMI→DSI rewire), and the payoff (a lit pixel) is only verifiable on silicon.
Best done as a focused pass, ideally with the device in the loop. Their THS
series (`0086–0089`) is an independent, easier adoption if a smaller win is
wanted first.

**Credits:** ut-slayer (Juan Manuel Lopez Carrillo), building on minimyth2 /
Justin Suess (H728 display) and Jernej Škrabec (H616). GPL-2.0.
