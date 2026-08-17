# A523 DE-v35x RCQ backend harvest (display scanout fix) — WIP, for next session

## Why
The A523 display engine (DE-v35x / "DE3.5") commits its pipeline via an **RCQ**
(Register Config Queue — per-frame register-block DMA). Our current `sun8i_mixer`
uses **direct MMIO** writes for the A523 (`de_type == SUN8I_MIXER_DE33`), which —
in ut-slayer's words — *"land in a shadow that does not latch."* Result: the DSI
video engine gets exactly one line then starves (`curr_line=0->1`), so the panel
lights the backlight but shows **no image**. Everything else in the pipeline is
proven working (combo-PHY locks, TCON HV/continuous mode, DSI attached, fb0 up,
no flip_done timeouts). **The RCQ commit is the entire remaining fix.**
(The IOMMU is **not** involved — our kernel has `CONFIG_SUN50I_IOMMU` unset, so
the DE runs on CMA/direct addressing; ut-slayer's IOMMU PTE patches are N/A here.)

## What's here
`sun55i_de.c` (1889L), `sun55i_de.h`, `sun55i_de_scaler.h` — the **final** RCQ
backend, reconstructed from ut-slayer/orangepi-4a-mainline (applied its 25
`sun55i_de`-touching patches in isolation). This is the authoritative code to port.

## The port (into our drivers/gpu/drm/sun4i/)
1. Copy the 3 files into the tree; add `sun55i_de.o` to `sun8i-mixer-y` in the Makefile.
2. `sun8i_mixer.h`: add `bool uses_rcq;` to `struct sun8i_mixer_cfg`; add
   `struct sun55i_de *de;` + `struct regmap *detop_regs;` to `struct sun8i_mixer`;
   `#include "sun55i_de.h"`. Set `.uses_rcq = true` on the **sun55i-a523** mixer cfg.
3. `sun8i_mixer.c`:
   - map **`detop_regs`** = the DE-top block at **DE base 0x5000000** (regs: RESET@0x00,
     CLK@0x04 [+CLK_KEY BIT(16), vendor ORs on every write], MBUS_CLK@0x08, DE2TCON_MUX,
     ASYNC_BRIDGE, BUF_DEPTH=0x6000, UCH2CORE_MUX). ut-slayer ioremaps the whole DE
     `devm_ioremap(dev, 0x5000000, 0x400000)`.
   - map the **RCQ head regs** on `top_regs` (RCQ_HEAD_LADDR/HADDR/LEN).
   - call `sun55i_de_init(mixer)` in probe when `uses_rcq` (it sets `mixer->de`,
     allocates the coherent RCQ pool via `dmam_alloc_coherent`, wants a reserved-mem /
     low-CMA region — try default CMA first).
   - **delegate the commit** when `uses_rcq`: the RCQ commit is a **two-pass, lock-held**
     flow — pass 1 `sun55i_de_hold_fb(old_fb)` (keep replaced fbs mapped until the RCQ arm
     retires them, else the DE faults → **wedged MBUS port → black**); pass 2 under
     `sun55i_de_stage_lock/unlock` do `sun55i_de_layer_update()` per plane then
     `sun55i_de_commit()`. Also delegate `layer_update`/`layer_disable`.
4. DT: a reg range for the DE MMIO (and optionally a low-CMA `memory-region` for the RCQ pool).

## Reference + test loop
- **v6.18 + ut-slayer applied** reference tree on the build server:
  `compiler-rock3b:/root/linux-6.18-de-ref` — **diff its `sun8i_mixer.c/.h` vs ours** for
  the exact, coherent integration delta (raw ut-slayer patches `git apply`-conflict on our
  7.2-rc3 tree, so this is a manual merge). ut-slayer patch series: `/root/ut-slayer/patches`
  (key: 0030 base, 0032/0033 RCQ-reload, 0045 CMA pool, 0066 neutralize-legacy-MMIO, 0070 races).
- **Test loop is remote**: rebuild `sun8i_mixer.ko` (with `sun55i_de` linked in), install to
  `/lib/modules`, **reboot** (a fresh boot re-inits the display cleanly — do **NOT** `rmmod`
  the display stack, it OOPSES). Watch `SUN6I-DSI-VID curr_line` climb past 1 + a lit image.

Coordinate with ut-slayer rather than duplicating (per the sunxi maintainer guidance).
