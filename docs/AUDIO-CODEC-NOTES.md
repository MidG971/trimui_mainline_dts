<!-- SPDX-License-Identifier: (GPL-2.0-only OR MIT) -->
<!-- Copyright (C) 2026 Midgy BALON -->

# Audio codec — A523 / sun55iw3

## Status: SWITCHED to Chen-Yu Tsai's upstream driver (2026-07-21)

We originally wrote a **standalone** `sun55i-codec.c` (~1022 lines, its own compatible
`allwinner,sun55i-a523-codec`). That is now **dropped** — the sunxi maintainer
**Chen-Yu Tsai** is writing the A523 codec upstream himself, by **extending the existing
`sound/soc/sunxi/sun4i-codec.c`** family driver under the *same* compatible. A parallel
driver can't share a compatible and never wins against the maintainer extending the family
driver, so per the linux-sunxi "adopt in-flight, don't duplicate" guidance we adopted his.

**What we carry now (build-verified on v7.2-rc3, 2026-07-21):**
- **Patches 0026-0031** = Chen-Yu's 6 codec commits, cherry-picked from his `wens/linux.git`
  `sunxi-wip` branch, authorship preserved (H616 `playback_only`; sort includes; split
  DAC/ADC clocks; **A523 playback**; **binding**; **[WIP] A523 capture**). They add sun55i
  regs (0x310/0x318/0x324/0x348), DAPM widgets/routes, `sun55i_a523_codec_quirks`
  (`has_reset` + `has_split_clks`) and the `of_match` entry to `sun4i-codec.c`.
  `CONFIG_SND_SUN4I_CODEC=m` (no separate driver symbol anymore).
- **Patch 0010** = our board-side SoC node `audio-codec@7110000` in `sun55i-a523.dtsi`,
  reshaped to HIS binding: **clock-names `apb`/`dac`/`adc`** (his `has_split_clks`; NOT the
  old `bus`+pll names), reg span **0x400** (covers his `max_register` 0x348), mcu_dma DRQ 7,
  IRQ 190, disabled by default.
- **Board `&codec`** (in the board DTS): enables it and wires the card via his GENERIC
  property names **`widgets`** + **`audio-routing`** (Speaker←LINEOUT, Headphone←HP,
  MIC1←Mic), `avcc = aldo4`.

**Build status:** `sun4i-codec.o` compiles and the board DTB builds; the decompile shows the
codec node with `apb/dac/adc` + the card props + the amp hog resolving. Playback is
merge-quality upstream; **capture is his `[WIP]`**. HW-UNVERIFIED (no audio without the device).

### Two real gaps found (upstream feedback material)
1. **His card has NO speaker-amp (PA) support.** Our handheld needs the **PH6** (active-high)
   power-amp enable, but his A523 `create_card` only parses `widgets`/`audio-routing` — no
   `pa-gpios`, no `simple-audio-amplifier` aux-dev. **Interim:** the board hogs **PH6 high**
   (`&pio spk_amp_hog`) so the speaker plays. Proper fix = add `pa-gpios`/aux-dev support to
   his codec card (so it's a DAPM-gated, power-aware enable — ties into the fan/power-profile
   plan), then drop the hog.
2. **Driver/binding property-name mismatch in his WIP:** the driver reads `"audio-routing"`
   but the binding (patch 0030) documents `allwinner,audio-routing`. Our board node follows
   the *driver* (what runs). Worth flagging to him.

**Switch-over when his codec merges upstream:** drop patches 0026-0031 (they'll be in
mainline), and if he also adds the SoC `audio-codec@7110000` node to `sun55i-a523.dtsi`
(as he did for the THS zones), slim our 0010 to a board `&codec` override.

## Hardware reference (still valid)

- Block: `codec@0x07110000`, register window through **0x348**, one IRQ, one reset.
- Self-contained **digital + analog** codec (analog regs in the same window at `0x300+`,
  unlike H6/H616 which split analog into PRCM) — hence a `sun4i-codec.c` variant, not a
  digital+analog split.
- Path: **DAC L/R → line-out + headphone**; **3 ADCs** ← MIC1/2/3. Headphone charge-pump +
  ramp for pop/click suppression. External **AVCC** analog supply (board: AXP2202 aldo4).
- Clocks (his driver): `apb` (`CLK_BUS_MCU_AUDIO_CODEC`), `dac` (`CLK_MCU_AUDIO_CODEC_DAC`),
  `adc` (`CLK_MCU_AUDIO_CODEC_ADC`) from the **MCU CCU** (`mcu_ccu@7102000`, already mainline);
  reset `RST_BUS_MCU_AUDIO_CODEC`. The pll-audio parents are handled by the CCU, not listed
  on the node.

### Register map (from BSP `snd_sun55iw3_codec.h`, for capture/DAP work later)
| Group | Offsets |
|-------|---------|
| DAC digital | `DAC_DPC@0x00`, `DAC_VOL_CTL@0x04`, `DAC_FIFO_CTL@0x10`, FIFO_STA/CNT/DEBUG |
| ADC digital | `ADC_FIFO_CTL@0x30`, `ADC_VOL_CTL1@0x34`, `ADC_FIFO_STA@0x38`, `ADC_RXDATA@0x40`, `ADC_DIG_CTL@0x50` |
| DAP / DRC / HPF | `DAC_DAP_CTL@0xF0`, `ADC_DAP_CTL@0xF8`, `DAC_DRC_CTL@0x108`, `ADC_DRC_CTL@0x208` |
| Analog | `ADC1/2/3_AN_CTL@0x300/0x304/0x308`, `DAC_AN@0x310`, `MICBIAS@0x318`, `RAMP@0x31c`, `HP@0x324`, `POWER@0x348` |

## HW-gated follow-ups (once the device is in hand)
- Verify the DAPM graph + routing directions on hardware; confirm the speaker (via the PH6
  amp), headphone, and MIC1 paths.
- Capture is `[WIP]` upstream — validate the 3-ADC mic path, or wait for his non-WIP version.
- DT `*-vol`/`*-gain` defaults, jack/HMIC detect, SID-efuse bias calibration, suspend/resume —
  all still deferred; his driver is a minimal playback-first variant.
- BSP regmap reference on the build host: `aw-bsp-drivers/.../snd_sun55iw3_codec.{c,h}`; UM ch 4.1.
