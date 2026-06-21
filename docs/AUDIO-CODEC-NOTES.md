<!-- SPDX-License-Identifier: (GPL-2.0-only OR MIT) -->
<!-- Copyright (C) 2026 Midgy BALON -->

# Audio codec — port plan (A523 / sun55iw3)

Plan for a **new mainline ASoC driver** for the A523 internal audio codec. This is
the largest greenfield item left (BSP driver `snd_sun55iw3_codec.c` is ~3115 lines).
**Status: planned, not started** — coding begins next session.

## 1. Hardware

- Block: `codec@0x07110000`, register window **0x348**, one IRQ, one reset.
- Self-contained **digital + analog** codec (analog regs live in the same window at
  `0x300+`, unlike H6/H616 which split analog into PRCM). So this is **one driver**,
  not codec-digital + separate analog component.
- Path: **DAC L/R → line-out + headphone**; **3 ADCs** ← mics/line-in. Headphone
  charge-pump + ramp (`SUNXI_RAMP@0x31c`) for pop/click suppression. Jack/HP detect.
- External **AVCC** supply (`avcc-supply`, `avcc-external`).

### Register map (from `snd_sun55iw3_codec.h`)
| Group | Offsets |
|-------|---------|
| DAC digital | `DAC_DPC@0x00`, `DAC_VOL_CTL@0x04`, `DAC_FIFO_CTL@0x10`, FIFO_STA/CNT/DEBUG |
| ADC digital | `ADC_FIFO_CTL@0x30`?, `ADC_VOL_CTL1@0x34`, `ADC_FIFO_STA@0x38`, `ADC_RXDATA@0x40`, `ADC_DIG_CTL@0x50` |
| DAP / DRC / HPF | `DAC_DAP_CTL@0xF0`, `ADC_DAP_CTL@0xF8`, `DAC_DRC_CTL@0x108`, `ADC_DRC_CTL@0x208` |
| Analog | `ADC1/2/3_AN_CTL@0x300/0x304/0x308`, `DAC_AN@0x310`, `DAC2_AN@0x314`, `RAMP@0x31c`, `HP_AN_CTL@0x324` |

## 2. BSP structure inventory (what to reproduce)

- **Registration:** platform driver → `devm_regmap_init_mmio` (`sunxi_regmap_config`)
  → register `snd_soc_component_driver sunxi_codec_component_dev` (`.probe =
  sunxi_codec_component_probe`) + `snd_soc_dai_driver sunxi_codec_dai`.
- **DAI** (`sunxi_codec_dai`, internal — `#sound-dai-cells = 0`, no external bus):
  - Playback: `SNDRV_PCM_RATE_8000_192000 | KNOT`, S16/S24/S32_LE, stereo.
  - Capture: `SNDRV_PCM_RATE_8000_48000 | KNOT`, up to 3 ADC channels.
  - `.ops = sunxi_codec_dai_ops` (hw_params / trigger / set_sysclk).
- **DAPM (~40 widgets):** DAC L/R, ADC1/2/3, `AIF_IN/AIF_OUT`, line-out (L/N pins),
  HP, SPK, 3× MIC, 2× LINE inputs, OUTPUT/INPUT pins, plus `POST_PMU/POST_PMD/
  PRE_PMU/PRE_PMD` event widgets for the HP charge-pump + ramp sequencing.
- **kcontrols:** 11 `SOC_SINGLE_TLV` (DAC / DACL / DACR Volume; ADC1/2/3 Volume;
  LINEOUT / HPOUT Gain; ADC1/2/3 Gain) + enums (DAC/ADC DRC & HPF mode, DACL/DACR &
  ADCn swaps, ADDA loopback, tx-hub / rx-sync). The vendor DT tuning props map 1:1:
  `dac-vol`/`dacl-vol`/`dacr-vol`/`adcN-vol`/`lineout-gain`/`hpout-gain`/`adcN-gain`.
- **Routes:** ~14 DAPM route entries (DAC→mixer→LINEOUT/HP; MIC/LINE→ADC).

## 3. Mainline mapping decision

- **New self-contained driver** `sound/soc/sunxi/sun55i-codec.c` + new compatible
  **`allwinner,sun55i-a523-codec`**. Model the *structure* on `sun4i-codec.c`
  (single component + DAI + dmaengine PCM, integrated analog), NOT on the
  digital/analog split of H616. Reuse mainline helpers: `snd_soc_set_dmaengine_pcm`
  / `devm_snd_dmaengine_pcm_register`, `regmap_mmio`, the standard TLV macros.
- It is **not** a variant of an existing in-tree compatible (register map differs
  from sun4i/sun8i/h616), so it cannot piggy-back on `sun4i-codec.c`’s `of_match`.
- **Machine card:** start with `simple-audio-card` (codec is self-contained, one DAI)
  — board DTS adds a `sound` node pointing at the codec. A dedicated machine driver
  is only needed if routing/jack handling can’t be expressed declaratively.

## 4. Clocks (trim the 9 vendor clocks)

Keep for a minimal mainline driver:
- `bus` (bus_audio gate), `dac` (audio_dac module), `adc` (audio_adc module), and an
  **audio PLL parent** for the 44.1k vs 48k families (`pll_audio0_4x` and/or
  `pll_audio1_div2`/`div5`). Drop the **DSP** clocks (`dsp_src`, `dsp_core`) and
  likely `pll_peri0_2x` — those serve the BSP DSP-offload path we won’t port.
- **DEPENDENCY TO CHECK FIRST:** confirm the mainline `sun55i-a523` CCU (and/or the
  audio CCU referenced as the second clock provider in the vendor node) actually
  exposes the codec DAC/ADC + audio-PLL clocks with usable IDs. If the CCU lacks
  them, that’s a prerequisite CCU change before the codec can probe. (Vendor node
  pulls clocks from two providers — main `&ccu` and a second one.)

## 5. Step-ordered plan (implement in this order)

1. **Prereq:** verify CCU clock IDs (DAC/ADC/bus/audio-PLL) + reset exist in mainline
   `sun55i-a523` headers; if not, note/stage the CCU addition.
2. **Skeleton:** platform driver, regmap_mmio, clocks/reset, component + DAI register,
   dmaengine PCM. No widgets yet → just probes.
3. **Playback:** DAC FIFO/DPC + DAC_AN/HP_AN analog, the DAC→LINEOUT/HP route,
   volume TLVs, ramp/pop event widgets. Target: `aplay` to line-out/HP.
4. **Capture:** ADC FIFO/DIG_CTL + ADCn_AN_CTL, MIC/LINE→ADC routes, ADC gains.
5. **Jack/HP detect** + the DT tuning props (apply `*-vol`/`*-gain` defaults).
6. **Niceties:** DRC/HPF mode enums, swaps, ADDA loopback, tx-hub/rx-sync.
7. **Binding** `allwinner,sun55i-a523-codec.yaml` + `dt_binding_check`; **board**
   `sound` (`simple-audio-card`) + `&codec` node; build clean + dt-validate.

## 6. How to tackle it (next session)

- Foreground, in chunks per §5 (driver ports as background agents get cut off mid-run
  — happened again this session at the account session limit). If budget allows,
  one analysis agent may pre-extract the exact DAPM widget/route/control reg-bit
  tables from `snd_sun55iw3_codec.c` to speed the skeleton — but the writing stays
  foreground.
- Reference on the build host: BSP `aw-bsp-drivers/drivers/sound/platform/
  snd_sun55iw3_codec.{c,h}` (authoritative regmap); mainline `sound/soc/sunxi/
  sun4i-codec.c` (structure to mirror). UM ch 4.1 = PDF p.546 (register details).

## 7. Risks / unknowns

- CCU codec-clock availability (§4) — the main gating risk.
- Analog path correctness (HP charge-pump enable order, ramp timing, anti-pop) is the
  fiddly part; lift the sequencing from the BSP `POST_PMU/PMD` event handlers.
- Real verification is **HW-gated** (no audio without the device); until then the
  driver can only be proven to build + bind + `dt_binding_check`/`dt-validate` clean.
