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

> **CORRECTED 2026-07-20 after an empirical apply/build pass on `compiler-rock3b`.**
> The earlier "adopt ~15 clean DE patches" triage below was **wrong**: it treated
> the DE-v35x core as a cherry-pickable subset. It is not. See "Reality check".

### Reality check — the DE-v35x core is an iterative HW-tuned branch, not a clean series
Enumerating every ut-slayer patch that touches a DE **driver** file
(`sun55i_de.c/.h`, `sun8i_mixer.c/.h`, `sun4i_crtc.c`, `sun8i_{ui,vi}_layer.c`)
shows the real span is **0030 → 0078 (~25 patches)**, not the 10-patch subset the
old table listed. It is Juan's *live debugging branch* (Spanish WIP messages),
including a **revert pair (0061 / 0062)**, `quiesce v4` (0054), `fix wedge v2`
(0063), "RCQ retries" (0065), "neutralise the MMIO path" (0066), "close the RCQ
races" (0070), and AFBC toggled off→on across 0067/0068/0069. Several are
**silicon-specific workarounds** ("the A523 RCQ DMA reports FINISH but doesn't
apply the BLD blocks" → the AHB/CPU write-in-blanking path in 0052) tuned on his
OrangePi-4A / T527. Cherry-picking fails immediately: applying the old subset
leaves `sun8i_mixer.c` with **no `quiesce` function** and `sun8i_mixer.h` with
**no RCQ_STATUS defines** because the prerequisite **0032** (page-flip tearing
fix that first wires `sun55i_de` into the mixer) was omitted; the later signature
/ AHB / AFBC hunks then reject. **The DE core must be taken as the whole
0030–0078 chain (squashed), forward-ported 6.18.38→v7.2 as one unit** — and its
timing/quiesce/RCQ hacks are only meaningful once validated on silicon
(hardware-testing-prevails). **Do not forward-port it blind.**

### Clean, HW-independent foundation — VERIFIED this session (apply + compile)
These apply with **zero rejects** on `v7.2-rc3 + our keep-set` and **compile
clean** (`sun50i-iommu.o` + `ccu-sun55i-a523.o`, exit 0). They are SoC-generic
and worth landing independent of the DE core:
| Group | Patches | Notes |
| :--- | :--- | :--- |
| **IOMMU (A523)** | 0025, 0029, 0047, 0049, 0050 | `sun50i-iommu.c` A523 support + PHYS_OFFSET/PTE fixes — needed for DE scanout, but standalone-correct |
| **CCU display / IOMMU clocks** | 0026, 0027, 0028 | `ccu-sun55i-a523.c` + clock/reset headers (TMDS is HDMI-only; IOMMU clk/reset are foundational) |
| **display-engine glue** | 0024 | `sun4i_drv.c` — `allwinner,sun50i-h6-display-engine` of_match reuse |

### DE-v35x core — the HW-tuned chain (adopt as a whole, WITH the device)
| Group | Patches | Notes |
| :--- | :--- | :--- |
| **DE-v35x driver + all fixes** | 0030, 0032, 0033, 0034, 0040, 0045, 0046, 0051–0055, 0059–0070, 0078 | `sun55i_de.c/.h` RCQ backend + the full iterative fix train; 0040 flattens `sun55i_a523_mixer0_cfg` (**supersedes our 0008**). Forward-port the squashed set; expect real hunk-resolution (base delta) + retarget the *output* HDMI→DSI. |
| **SoC DE dtsi** | 0036, 0037 | `de`/`bus@5000000`/`mixer@100000`/`tcon-top@5500000` + IOMMU nodes — **hand-reconcile with our 0003**; wire `tcon-top → our tcon1 (LCD) → dsi1 → panel`, **not** their `tcon-tv → HDMI`. (0056 = HDMI audio, skip.) |

### Skip — HDMI-specific (we use DSI, not HDMI)
`sun8i_hdmi_phy.c`, `sun8i_dw_hdmi`, TCON-TV bits of 0022, `sun8i_hdmi_*` and all
`sun55i-t527-orangepi-4a.dts` board patches; 0056 (HDMI audio). Their `.config`
hunks (0040 touches `.config`) — we use `kernel/trimui.config`.

## Reconciliation with our existing display work
| Ours | Decision |
| :--- | :--- |
| `0001` DSI host, `0002` combo-PHY, `0007` panel | **KEEP** — DSI-only, no overlap with their HDMI path |
| `0004` TCON-LCD | **KEEP, reconcile** — their `0022/0032` also touch `sun4i_tcon.c` (TCON-TV); merge so both TCON-LCD (ours, → DSI) and their tcon-top coexist (verified this session: our `0004` applies clean on rc3; theirs touch `sun4i_crtc.c`, not `sun4i_tcon.c`, so the driver-level overlap is smaller than feared) |
| `0008` mixer cfg | **DROP** — superseded by their `0040` (flattened `sun55i_a523_mixer0_cfg`), which is part of the DE-core chain |
| `0003` SoC display pipeline dtsi | **RECONCILE** — keep our `tcon1`/`dsi1`/`dsi1_combo_phy` nodes; **drop our skeleton `de@5000000` + `display-top@5500000`** (their `0036` provides the real `de`/`mixer`/`tcon-top`); rewire `tcon_top → our tcon1 (LCD) → dsi1 → panel`, not their `tcon-tv → HDMI`. (`0003` also had a stray committed `sun55i-a523.dtsi.orig` — **fixed 2026-07-20**.) |

## Execution order (revised 2026-07-20)
1. Fresh v7.2-rc3 + our keep-set (all of `0001–0025` **except** `0003`=reconcile,
   `0008`=drop). Apply with `-F3` fuzz (base-delta context drift is expected).
2. Apply the **clean foundation** (IOMMU `0025/29/47/49/50`, CCU `0026/27/28`,
   glue `0024`) — zero-reject, already compile-verified.
3. Forward-port the **whole DE-v35x chain `0030–0078` as a squashed unit**
   (not cherry-picked — 0032 is a hard prereq); resolve the 6.18.38→v7.2 hunks.
4. Re-apply/keep our DSI stack (`0001/0002/0004/0007`), `0008` stays dropped.
5. Hand-write the reconciled SoC DT: their `de`/`bus`/`mixer`/`tcon-top` + our
   reconciled `0003` `tcon1`/`dsi1`/`combo-phy`, OF-graph **tcon-top → tcon1 →
   dsi1 → panel**.
6. `make dtbs` + `dt-validate` + compile the DE/mixer objects.
7. **On hardware:** `modetest` a test pattern → the internal panel lights, then
   port over Juan's RCQ/quiesce/AHB workarounds against *our* silicon's behaviour.

## Scope (corrected)
Bigger than the old estimate: **~35 patches** (the ~25-patch iterative DE-v35x
core + IOMMU/CCU/glue foundation + 2 SoC-DT patches) plus the HDMI→DSI output
rewire. The DE core is a **live HW-debugging branch with reverts and `vN`
workarounds**, so its correctness is inseparable from silicon — a build-clean
forward-port proves nothing about a lit pixel. **Do it as a focused pass WITH the
device**, using Juan's board as the known-good HDMI reference before retargeting
to our DSI panel. The clean IOMMU/CCU/glue foundation (step 2) is the only part
safe to land pre-device. Their THS series (`0086–0089`) is already adopted
(our `0021–0023`); GPU OPP/maskdiv is tracked separately.

**Credits:** ut-slayer (Juan Manuel Lopez Carrillo), building on minimyth2 /
Justin Suess (H728 display) and Jernej Škrabec (H616). GPL-2.0.
