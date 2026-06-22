<!-- SPDX-License-Identifier: (GPL-2.0-only OR MIT) -->
<!-- Copyright (C) 2026 Midgy BALON -->

# Audio codec — port plan (A523 / sun55iw3)

Plan + status for the **new mainline ASoC driver** for the A523 internal audio codec
(BSP `snd_sun55iw3_codec.c` is ~3115 lines).

**Status: driver + DT integration done — `kernel/drivers/sun55i-codec.c`** (playback +
capture, 14 mixer controls, DAPM, clocking, init, ref-counted mic-bias) now a
**two-component model** (cpu DAI + codec DAI) that **self-registers its card** (like
sun4i-codec), parses `allwinner,audio-routing`, and drives the **speaker amp GPIO**
(`allwinner,pa-gpios`). SoC `audio-codec@7110000` node + board `&codec` enable +
routing (PH6 amp) all in tree. **Builds clean on v7.1; binding + board DTB validate
clean. NOT YET HW-verified.** Deferred: jack/HMIC detect, DAP DRC/HPF, SID-efuse bias
calibration, tx-hub/rx-sync, suspend/resume.

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
- **PREREQUISITE CLEARED (verified on v7.1):** the vendor's 2nd clock provider is the
  **MCU CCU** (`mcu_ccu@7102000`, already in mainline dtsi), which exposes everything:
  `CLK_BUS_MCU_AUDIO_CODEC`(21), `CLK_MCU_AUDIO_CODEC_DAC`(19),
  `CLK_MCU_AUDIO_CODEC_ADC`(20), `CLK_MCU_PLL_AUDIO1_DIV2/DIV5`(1/2),
  `RST_BUS_MCU_AUDIO_CODEC`(6); the 48 kHz-family PLL `CLK_PLL_AUDIO0_4X` is in the
  main CCU. No CCU change needed. The driver uses clock-names
  bus/dac/adc/pll-audio0-4x/pll-audio1-div5.

## 5. Step-ordered plan / progress

1. [x] **Prereq:** CCU clock IDs verified (§4 — all in the MCU CCU + main CCU).
2. [x] **Skeleton:** platform driver, regmap_mmio, clocks/reset, component + DAI,
   dmaengine PCM. Builds.
3. [x] **Playback:** DAC FIFO/DPC, DAC_AN line-out/HP analog, DAC→LINEOUT/HP routes,
   volume TLVs, line-out/HP/playback event widgets, init sequence.
4. [x] **Capture:** ADC FIFO/DIG_CTL + ADCn_AN_CTL, MIC→ADC routes, ADC gains, the
   ref-counted mic-bias settle (`msleep(240)`).
5. [x] **Binding** `allwinner,sun55i-a523-codec.yaml` + `dt_binding_check` (clean);
   Kconfig/Makefile patch 0009.
6. [x] **DT node (patch 0010):** `audio-codec@7110000` in `sun55i-a523.dtsi`
   (mcu_ccu bus/dac/adc clocks + audio PLLs, mcu_ccu reset, `mcu_dma` DRQ 7, IRQ 190),
   disabled; board `&codec` enable + `avcc = aldo4`. Board DTB dt-validates clean.
7. [x] **Sound card (driver-side):** refactored to the **two-component split** (cpu-DAI
   component owning the dmaengine PCM + the codec component owning analog DAPM/hw_params);
   `create_link`/`create_card` + `devm_snd_soc_register_card`, parses
   `allwinner,audio-routing`, **Speaker** card widget driving the amp GPIO
   (`allwinner,pa-gpios` = **PH6**). Board adds the routing (vendor map) + PH6. Builds +
   dt-validates clean.
8. [ ] **Then (HW-gated):** jack/HMIC detect (headphone + the media-key voltages), DT
   `*-vol`/`*-gain` defaults, DRC/HPF, SID-efuse bias cal, tx-hub/rx-sync, suspend/resume.
   And **verify the DAPM graph on hardware** — the routing/widget directions mirror the
   vendor BSP but the card's DAPM power-walk is unproven without the device.

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
