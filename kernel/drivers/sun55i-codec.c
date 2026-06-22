// SPDX-License-Identifier: GPL-2.0-only
/*
 * Allwinner A523 (sun55iw3) internal audio codec.
 *
 * Copyright (C) 2026 Midgy BALON
 *
 * Self-contained digital + analog codec at 0x07110000 (reg window 0x348):
 * DAC L/R -> line-out + headphone, 3 ADCs <- mic/line, integrated analog at
 * 0x300+. New mainline ASoC driver for compatible "allwinner,sun55i-a523-codec";
 * structure modeled on sun4i-codec.c (single component + DAI + dmaengine PCM).
 * Clocks/reset come from the MCU CCU (already in mainline sun55i-a523.dtsi).
 *
 * Register map, DAPM/control inventory and the analog event sequencing were
 * extracted from the vendor BSP snd_sun55iw3_codec.{c,h}. See
 * docs/AUDIO-CODEC-NOTES.md. NOT YET HW-verified.
 *
 * Implemented: playback + capture path, mixer controls, DAPM incl. the mic-bias
 * reference-counted settle. TODO: jack/HMIC detect, DAP DRC/HPF, SID-efuse bias
 * calibration, tx-hub/rx-sync, suspend/resume.
 */

#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/input.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/regulator/consumer.h>
#include <linux/reset.h>

#include <sound/dmaengine_pcm.h>
#include <sound/jack.h>
#include <sound/pcm_params.h>
#include <sound/soc.h>
#include <sound/tlv.h>

/* DAC digital */
#define SUN55I_DAC_DPC			0x00
#define SUN55I_DAC_DPC_EN_DA			31
#define SUN55I_DAC_DPC_DVOL			12	/* 6-bit */
#define SUN55I_DAC_DPC_HUB_EN			0
#define SUN55I_DAC_VOL_CTL		0x04
#define SUN55I_DAC_VOL_SEL			16
#define SUN55I_DAC_VOL_L			8	/* 8-bit */
#define SUN55I_DAC_VOL_R			0	/* 8-bit */
#define SUN55I_DAC_FIFO_CTL		0x10
#define SUN55I_DAC_FS				29	/* 3-bit */
#define SUN55I_DAC_FIFO_MODE			24	/* 2-bit */
#define SUN55I_DAC_MONO_EN			6
#define SUN55I_DAC_TX_SAMPLE_BITS		5
#define SUN55I_DAC_DRQ_EN			4
#define SUN55I_DAC_FIFO_FLUSH			0
#define SUN55I_DAC_FIFO_STA		0x14
#define SUN55I_DAC_TXE_INT			3
#define SUN55I_DAC_TXU_INT			2
#define SUN55I_DAC_TXO_INT			1
#define SUN55I_DAC_TXDATA		0x20
#define SUN55I_DAC_CNT			0x24
#define SUN55I_DAC_DEBUG		0x28
#define SUN55I_DAC_DA_SWP			6

/* ADC digital */
#define SUN55I_ADC_FIFO_CTL		0x30
#define SUN55I_ADC_FS				29	/* 3-bit */
#define SUN55I_ADC_DIG_EN			28
#define SUN55I_ADC_FDT				26	/* 2-bit */
#define SUN55I_ADC_DFEN				25
#define SUN55I_ADC_RX_FIFO_MODE			24
#define SUN55I_ADC_RX_SAMPLE_BITS		16
#define SUN55I_ADC_DRQ_EN			3
#define SUN55I_ADC_FIFO_FLUSH			0
#define SUN55I_ADC_VOL_CTL1		0x34
#define SUN55I_ADC3_VOL				16	/* 8-bit */
#define SUN55I_ADC2_VOL				8	/* 8-bit */
#define SUN55I_ADC1_VOL				0	/* 8-bit */
#define SUN55I_ADC_FIFO_STA		0x38
#define SUN55I_ADC_RXA_INT			3
#define SUN55I_ADC_RXO_INT			1
#define SUN55I_ADC_RXDATA		0x40
#define SUN55I_ADC_CNT			0x44
#define SUN55I_ADC_DEBUG		0x4c
#define SUN55I_ADC_SWP1				24
#define SUN55I_ADC_SWP2				25
#define SUN55I_ADC_DIG_CTL		0x50
#define SUN55I_ADC3_VOL_EN			17
#define SUN55I_ADC1_2_VOL_EN			16

/* DAP */
#define SUN55I_DAC_DAP_CTL		0xf0
#define SUN55I_DDAP_EN				31
#define SUN55I_ADC_DAP_CTL		0xf8
#define SUN55I_ADAP0_EN				31
#define SUN55I_ADAP1_EN				27

/* Analog */
#define SUN55I_ADC1_AN_CTL		0x300
#define SUN55I_ADC2_AN_CTL		0x304
#define SUN55I_ADC3_AN_CTL		0x308
#define SUN55I_ADCn_EN				31
#define SUN55I_MICn_PGA_EN			30
#define SUN55I_ADCn_PGA_GAIN_CTL		8	/* 5-bit */
#define SUN55I_DAC_AN_REG		0x310
#define SUN55I_HEADPHONE_GAIN			28	/* 3-bit */
#define SUN55I_CPLDO_VOLTAGE			24	/* 2-bit */
#define SUN55I_DACL_EN				15
#define SUN55I_DACR_EN				14
#define SUN55I_LINEOUTL_EN			13
#define SUN55I_LMUTE				12
#define SUN55I_LINEOUTR_EN			11
#define SUN55I_RMUTE				10
#define SUN55I_CPLDO_EN				7
#define SUN55I_LINEOUT_GAIN			0	/* 5-bit */
#define SUN55I_MICBIAS_AN_CTL		0x318
#define SUN55I_SEL_DET_ADC_BF			24	/* 2-bit */
#define SUN55I_JACK_DET_EN			23
#define SUN55I_MIC_DET_ADC_EN			20
#define SUN55I_DET_MODE				18
#define SUN55I_AUTO_PULL_LOW_EN			17
#define SUN55I_HMIC_BIAS_EN			15
#define SUN55I_HMIC_BIAS_SEL			13	/* 2-bit */
#define SUN55I_MMIC_BIAS_EN			7
#define SUN55I_RAMP			0x31c
#define SUN55I_RAMP_EN				1
#define SUN55I_BIAS_AN_CTL		0x320
#define SUN55I_HP_AN_CTL		0x324
#define SUN55I_HPPA_EN				15
#define SUN55I_HMIC_CTL			0x328
#define SUN55I_MDATA_THRESHOLD			16	/* 5-bit */
#define SUN55I_HMIC_N				6	/* 4-bit debounce */
#define SUN55I_JACK_OUT_IRQ_EN			2
#define SUN55I_JACK_IN_IRQ_EN			1
#define SUN55I_MIC_DET_IRQ_EN			0
#define SUN55I_HMIC_STA			0x32c
#define SUN55I_HMIC_DATA			8	/* 5-bit ADC data */
#define SUN55I_JACK_OUT_IRQ_STA			4
#define SUN55I_JACK_IN_IRQ_STA			3
#define SUN55I_MIC_DET_IRQ_STA			0
#define SUN55I_POWER_AN_CTL		0x348
#define SUN55I_VRP_LDO_EN			24
#define SUN55I_BG_BUFFER_DISABLE		15

#define SUN55I_CODEC_MAX_REG		SUN55I_POWER_AN_CTL

struct sun55i_codec {
	struct device			*dev;
	struct regmap			*regmap;
	struct clk			*clk_bus;
	struct clk			*clk_dac;
	struct clk			*clk_adc;
	struct clk			*clk_pll_audio0_4x;	/* 48k family */
	struct clk			*clk_pll_audio1_div5;	/* 44.1k family */
	struct reset_control		*rst;
	struct gpio_desc		*gpio_pa;	/* speaker amp enable */
	int				irq;
	struct snd_soc_jack		jack;
	struct snd_dmaengine_dai_dma_data	playback_dma;
	struct snd_dmaengine_dai_dma_data	capture_dma;

	/* mic-bias / ADC-digital are shared across the three mic widgets */
	struct mutex			mic_lock;
	bool				mic_active[3];
};

#define SUN55I_JACK_BUTTONS	(SND_JACK_BTN_0 | SND_JACK_BTN_1 | \
				 SND_JACK_BTN_2 | SND_JACK_BTN_3)

/* ---- sample-rate -> FS field (DAC_FS / ADC_FS, 3-bit) ---- */
static const struct {
	unsigned int rate;
	unsigned int fs;
} sun55i_codec_rates[] = {
	{ 8000, 5 }, { 11025, 4 }, { 12000, 4 }, { 16000, 3 }, { 22050, 2 },
	{ 24000, 2 }, { 32000, 1 }, { 44100, 0 }, { 48000, 0 }, { 96000, 7 },
	{ 192000, 6 },
};

static int sun55i_codec_get_fs(unsigned int rate)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(sun55i_codec_rates); i++)
		if (sun55i_codec_rates[i].rate == rate)
			return sun55i_codec_rates[i].fs;
	return -EINVAL;
}

/*
 * Pick the audio-PLL family and program the DAC/ADC module clock.
 * 48k family (rate % 4000 == 0) -> pll-audio0-4x @ 196.608 MHz, module 24.576 MHz.
 * 44.1k family                  -> pll-audio1-div5,             module 22.5792 MHz.
 */
static int sun55i_codec_set_clk(struct sun55i_codec *scodec, struct clk *mod,
				unsigned int rate)
{
	struct clk *parent;
	unsigned long mod_rate;
	int ret;

	if (rate % 4000 == 0) {
		parent = scodec->clk_pll_audio0_4x;
		mod_rate = 24576000;
		clk_set_rate(scodec->clk_pll_audio0_4x, 196608000);
	} else {
		parent = scodec->clk_pll_audio1_div5;
		mod_rate = 22579200;
	}

	if (parent) {
		ret = clk_set_parent(mod, parent);
		if (ret)
			return ret;
	}
	return clk_set_rate(mod, mod_rate);
}

static int sun55i_codec_hw_params(struct snd_pcm_substream *substream,
				  struct snd_pcm_hw_params *params,
				  struct snd_soc_dai *dai)
{
	struct sun55i_codec *scodec = snd_soc_dai_get_drvdata(dai);
	bool playback = (substream->stream == SNDRV_PCM_STREAM_PLAYBACK);
	unsigned int fifo_mode, sample_bits;
	int fs, ret;

	fs = sun55i_codec_get_fs(params_rate(params));
	if (fs < 0)
		return fs;

	switch (params_format(params)) {
	case SNDRV_PCM_FORMAT_S16_LE:
		fifo_mode = playback ? 0x3 : 0x1;
		sample_bits = 0;
		break;
	case SNDRV_PCM_FORMAT_S24_LE:
	case SNDRV_PCM_FORMAT_S32_LE:
		fifo_mode = 0x0;
		sample_bits = 1;
		break;
	default:
		return -EINVAL;
	}

	ret = sun55i_codec_set_clk(scodec, playback ? scodec->clk_dac
						    : scodec->clk_adc,
				   params_rate(params));
	if (ret)
		return ret;

	if (playback) {
		regmap_update_bits(scodec->regmap, SUN55I_DAC_FIFO_CTL,
				   0x3 << SUN55I_DAC_FIFO_MODE,
				   fifo_mode << SUN55I_DAC_FIFO_MODE);
		regmap_update_bits(scodec->regmap, SUN55I_DAC_FIFO_CTL,
				   0x3 << SUN55I_DAC_TX_SAMPLE_BITS,
				   sample_bits << SUN55I_DAC_TX_SAMPLE_BITS);
		regmap_update_bits(scodec->regmap, SUN55I_DAC_FIFO_CTL,
				   0x7 << SUN55I_DAC_FS, fs << SUN55I_DAC_FS);
		regmap_update_bits(scodec->regmap, SUN55I_DAC_FIFO_CTL,
				   BIT(SUN55I_DAC_MONO_EN),
				   params_channels(params) == 1 ?
					BIT(SUN55I_DAC_MONO_EN) : 0);
	} else {
		regmap_update_bits(scodec->regmap, SUN55I_ADC_FIFO_CTL,
				   BIT(SUN55I_ADC_RX_FIFO_MODE),
				   fifo_mode << SUN55I_ADC_RX_FIFO_MODE);
		regmap_update_bits(scodec->regmap, SUN55I_ADC_FIFO_CTL,
				   0x3 << SUN55I_ADC_RX_SAMPLE_BITS,
				   sample_bits << SUN55I_ADC_RX_SAMPLE_BITS);
		regmap_update_bits(scodec->regmap, SUN55I_ADC_FIFO_CTL,
				   0x7 << SUN55I_ADC_FS, fs << SUN55I_ADC_FS);
	}

	return 0;
}

static int sun55i_codec_prepare(struct snd_pcm_substream *substream,
				struct snd_soc_dai *dai)
{
	struct sun55i_codec *scodec = snd_soc_dai_get_drvdata(dai);

	if (substream->stream == SNDRV_PCM_STREAM_PLAYBACK) {
		regmap_update_bits(scodec->regmap, SUN55I_DAC_FIFO_CTL,
				   BIT(SUN55I_DAC_FIFO_FLUSH),
				   BIT(SUN55I_DAC_FIFO_FLUSH));
		regmap_write(scodec->regmap, SUN55I_DAC_FIFO_STA,
			     BIT(SUN55I_DAC_TXE_INT) | BIT(SUN55I_DAC_TXU_INT) |
			     BIT(SUN55I_DAC_TXO_INT));
		regmap_write(scodec->regmap, SUN55I_DAC_CNT, 0);
	} else {
		regmap_update_bits(scodec->regmap, SUN55I_ADC_FIFO_CTL,
				   BIT(SUN55I_ADC_FIFO_FLUSH),
				   BIT(SUN55I_ADC_FIFO_FLUSH));
		regmap_write(scodec->regmap, SUN55I_ADC_FIFO_STA,
			     BIT(SUN55I_ADC_RXA_INT) | BIT(SUN55I_ADC_RXO_INT));
		regmap_write(scodec->regmap, SUN55I_ADC_CNT, 0);
	}

	return 0;
}

static int sun55i_codec_trigger(struct snd_pcm_substream *substream, int cmd,
				struct snd_soc_dai *dai)
{
	struct sun55i_codec *scodec = snd_soc_dai_get_drvdata(dai);
	bool playback = (substream->stream == SNDRV_PCM_STREAM_PLAYBACK);
	unsigned int reg = playback ? SUN55I_DAC_FIFO_CTL : SUN55I_ADC_FIFO_CTL;
	unsigned int drq = playback ? BIT(SUN55I_DAC_DRQ_EN)
				    : BIT(SUN55I_ADC_DRQ_EN);

	switch (cmd) {
	case SNDRV_PCM_TRIGGER_START:
	case SNDRV_PCM_TRIGGER_RESUME:
	case SNDRV_PCM_TRIGGER_PAUSE_RELEASE:
		regmap_update_bits(scodec->regmap, reg, drq, drq);
		return 0;
	case SNDRV_PCM_TRIGGER_STOP:
	case SNDRV_PCM_TRIGGER_SUSPEND:
	case SNDRV_PCM_TRIGGER_PAUSE_PUSH:
		regmap_update_bits(scodec->regmap, reg, drq, 0);
		return 0;
	default:
		return -EINVAL;
	}
}

static int sun55i_codec_dai_probe(struct snd_soc_dai *dai)
{
	struct sun55i_codec *scodec = snd_soc_dai_get_drvdata(dai);

	snd_soc_dai_init_dma_data(dai, &scodec->playback_dma,
				  &scodec->capture_dma);
	return 0;
}

#define SUN55I_FORMATS	(SNDRV_PCM_FMTBIT_S16_LE | SNDRV_PCM_FMTBIT_S24_LE | \
			 SNDRV_PCM_FMTBIT_S32_LE)

/*
 * Two-component model (as sun4i-codec): a "cpu" DAI that only wires up the
 * dmaengine PCM, and the "Codec" DAI that drives the registers. The cpu
 * component uses legacy DAI naming so the card can reference it by dev name.
 */
static const struct snd_soc_dai_ops sun55i_codec_cpu_dai_ops = {
	.probe		= sun55i_codec_dai_probe,
};

static struct snd_soc_dai_driver sun55i_codec_cpu_dai = {
	.name	= "sun55i-codec-cpu-dai",
	.playback = {
		.stream_name	= "Playback",
		.channels_min	= 1,
		.channels_max	= 2,
		.rates		= SNDRV_PCM_RATE_8000_192000 | SNDRV_PCM_RATE_KNOT,
		.formats	= SUN55I_FORMATS,
	},
	.capture = {
		.stream_name	= "Capture",
		.channels_min	= 1,
		.channels_max	= 3,
		.rates		= SNDRV_PCM_RATE_8000_48000 | SNDRV_PCM_RATE_KNOT,
		.formats	= SUN55I_FORMATS,
	},
	.ops	= &sun55i_codec_cpu_dai_ops,
};

static const struct snd_soc_dai_ops sun55i_codec_codec_dai_ops = {
	.hw_params	= sun55i_codec_hw_params,
	.prepare	= sun55i_codec_prepare,
	.trigger	= sun55i_codec_trigger,
};

static struct snd_soc_dai_driver sun55i_codec_codec_dai = {
	.name	= "Codec",
	.playback = {
		.stream_name	= "Codec Playback",
		.channels_min	= 1,
		.channels_max	= 2,
		.rates		= SNDRV_PCM_RATE_8000_192000 | SNDRV_PCM_RATE_KNOT,
		.formats	= SUN55I_FORMATS,
	},
	.capture = {
		.stream_name	= "Codec Capture",
		.channels_min	= 1,
		.channels_max	= 3,
		.rates		= SNDRV_PCM_RATE_8000_48000 | SNDRV_PCM_RATE_KNOT,
		.formats	= SUN55I_FORMATS,
	},
	.ops	= &sun55i_codec_codec_dai_ops,
};

/* ---- mixer controls ---- */
static const DECLARE_TLV_DB_SCALE(sun55i_dac_vol_tlv, -7424, 116, 0);
static const DECLARE_TLV_DB_SCALE(sun55i_dacl_vol_tlv, -11925, 75, 1);
static const DECLARE_TLV_DB_SCALE(sun55i_dacr_vol_tlv, -11925, 75, 1);
static const DECLARE_TLV_DB_SCALE(sun55i_adc_vol_tlv, -11925, 75, 1);
static const DECLARE_TLV_DB_SCALE(sun55i_hpout_gain_tlv, -4200, 600, 1);
static const DECLARE_TLV_DB_SCALE(sun55i_adc_gain_tlv, 0, 100, 0);
static const DECLARE_TLV_DB_RANGE(sun55i_lineout_gain_tlv,
	0, 1, TLV_DB_SCALE_ITEM(0, 0, 1),
	2, 31, TLV_DB_SCALE_ITEM(-4350, 150, 1));

static const char * const sun55i_swap_text[] = { "Off", "On" };
static SOC_ENUM_SINGLE_DECL(sun55i_dac_swap_enum, SUN55I_DAC_DEBUG,
			    SUN55I_DAC_DA_SWP, sun55i_swap_text);
static SOC_ENUM_SINGLE_DECL(sun55i_adc12_swap_enum, SUN55I_ADC_DEBUG,
			    SUN55I_ADC_SWP1, sun55i_swap_text);
static SOC_ENUM_SINGLE_DECL(sun55i_adc34_swap_enum, SUN55I_ADC_DEBUG,
			    SUN55I_ADC_SWP2, sun55i_swap_text);

static const struct snd_kcontrol_new sun55i_codec_controls[] = {
	SOC_SINGLE_TLV("DAC Volume", SUN55I_DAC_DPC, SUN55I_DAC_DPC_DVOL,
		       0x3f, 1, sun55i_dac_vol_tlv),
	SOC_SINGLE_TLV("DACL Volume", SUN55I_DAC_VOL_CTL, SUN55I_DAC_VOL_L,
		       0xff, 0, sun55i_dacl_vol_tlv),
	SOC_SINGLE_TLV("DACR Volume", SUN55I_DAC_VOL_CTL, SUN55I_DAC_VOL_R,
		       0xff, 0, sun55i_dacr_vol_tlv),
	SOC_SINGLE_TLV("ADC1 Volume", SUN55I_ADC_VOL_CTL1, SUN55I_ADC1_VOL,
		       0xff, 0, sun55i_adc_vol_tlv),
	SOC_SINGLE_TLV("ADC2 Volume", SUN55I_ADC_VOL_CTL1, SUN55I_ADC2_VOL,
		       0xff, 0, sun55i_adc_vol_tlv),
	SOC_SINGLE_TLV("ADC3 Volume", SUN55I_ADC_VOL_CTL1, SUN55I_ADC3_VOL,
		       0xff, 0, sun55i_adc_vol_tlv),
	SOC_SINGLE_TLV("LINEOUT Gain", SUN55I_DAC_AN_REG, SUN55I_LINEOUT_GAIN,
		       0x1f, 0, sun55i_lineout_gain_tlv),
	SOC_SINGLE_TLV("HPOUT Gain", SUN55I_DAC_AN_REG, SUN55I_HEADPHONE_GAIN,
		       0x7, 1, sun55i_hpout_gain_tlv),
	SOC_SINGLE_TLV("ADC1 Gain", SUN55I_ADC1_AN_CTL, SUN55I_ADCn_PGA_GAIN_CTL,
		       0x1f, 0, sun55i_adc_gain_tlv),
	SOC_SINGLE_TLV("ADC2 Gain", SUN55I_ADC2_AN_CTL, SUN55I_ADCn_PGA_GAIN_CTL,
		       0x1f, 0, sun55i_adc_gain_tlv),
	SOC_SINGLE_TLV("ADC3 Gain", SUN55I_ADC3_AN_CTL, SUN55I_ADCn_PGA_GAIN_CTL,
		       0x1f, 0, sun55i_adc_gain_tlv),
	SOC_ENUM("DACL DACR Swap", sun55i_dac_swap_enum),
	SOC_ENUM("ADC1 ADC2 Swap", sun55i_adc12_swap_enum),
	SOC_ENUM("ADC3 ADC4 Swap", sun55i_adc34_swap_enum),
};

/* ---- DAPM ---- */
static int sun55i_playback_event(struct snd_soc_dapm_widget *w,
				 struct snd_kcontrol *k, int event)
{
	struct snd_soc_component *cmp = snd_soc_dapm_to_component(w->dapm);
	struct sun55i_codec *scodec = snd_soc_component_get_drvdata(cmp);

	regmap_update_bits(scodec->regmap, SUN55I_DAC_DPC,
			   BIT(SUN55I_DAC_DPC_EN_DA),
			   SND_SOC_DAPM_EVENT_ON(event) ?
				BIT(SUN55I_DAC_DPC_EN_DA) : 0);
	return 0;
}

static int sun55i_lineoutl_event(struct snd_soc_dapm_widget *w,
				 struct snd_kcontrol *k, int event)
{
	struct snd_soc_component *cmp = snd_soc_dapm_to_component(w->dapm);
	struct sun55i_codec *scodec = snd_soc_component_get_drvdata(cmp);

	if (SND_SOC_DAPM_EVENT_ON(event)) {
		regmap_update_bits(scodec->regmap, SUN55I_DAC_AN_REG,
				   BIT(SUN55I_LMUTE), BIT(SUN55I_LMUTE));
		regmap_update_bits(scodec->regmap, SUN55I_DAC_AN_REG,
				   BIT(SUN55I_LINEOUTL_EN),
				   BIT(SUN55I_LINEOUTL_EN));
	} else {
		regmap_update_bits(scodec->regmap, SUN55I_DAC_AN_REG,
				   BIT(SUN55I_LINEOUTL_EN), 0);
		regmap_update_bits(scodec->regmap, SUN55I_DAC_AN_REG,
				   BIT(SUN55I_LMUTE), 0);
	}
	return 0;
}

static int sun55i_lineoutr_event(struct snd_soc_dapm_widget *w,
				 struct snd_kcontrol *k, int event)
{
	struct snd_soc_component *cmp = snd_soc_dapm_to_component(w->dapm);
	struct sun55i_codec *scodec = snd_soc_component_get_drvdata(cmp);

	if (SND_SOC_DAPM_EVENT_ON(event)) {
		regmap_update_bits(scodec->regmap, SUN55I_DAC_AN_REG,
				   BIT(SUN55I_RMUTE), BIT(SUN55I_RMUTE));
		regmap_update_bits(scodec->regmap, SUN55I_DAC_AN_REG,
				   BIT(SUN55I_LINEOUTR_EN),
				   BIT(SUN55I_LINEOUTR_EN));
	} else {
		regmap_update_bits(scodec->regmap, SUN55I_DAC_AN_REG,
				   BIT(SUN55I_LINEOUTR_EN), 0);
		regmap_update_bits(scodec->regmap, SUN55I_DAC_AN_REG,
				   BIT(SUN55I_RMUTE), 0);
	}
	return 0;
}

static int sun55i_hpout_event(struct snd_soc_dapm_widget *w,
			      struct snd_kcontrol *k, int event)
{
	struct snd_soc_component *cmp = snd_soc_dapm_to_component(w->dapm);
	struct sun55i_codec *scodec = snd_soc_component_get_drvdata(cmp);

	regmap_update_bits(scodec->regmap, SUN55I_HP_AN_CTL, BIT(SUN55I_HPPA_EN),
			   SND_SOC_DAPM_EVENT_ON(event) ?
				BIT(SUN55I_HPPA_EN) : 0);
	return 0;
}

/* MIC bias + ADC digital master are shared across the three mics (ref-counted). */
static int sun55i_mic_event(struct sun55i_codec *scodec, int idx,
			    unsigned int an_ctl, int event)
{
	bool first_or_last;

	if (SND_SOC_DAPM_EVENT_ON(event)) {
		regmap_update_bits(scodec->regmap, an_ctl, BIT(SUN55I_MICn_PGA_EN),
				   BIT(SUN55I_MICn_PGA_EN));
		regmap_update_bits(scodec->regmap, an_ctl, BIT(SUN55I_ADCn_EN),
				   BIT(SUN55I_ADCn_EN));

		mutex_lock(&scodec->mic_lock);
		first_or_last = !scodec->mic_active[0] && !scodec->mic_active[1] &&
				!scodec->mic_active[2];
		scodec->mic_active[idx] = true;
		if (first_or_last) {
			regmap_update_bits(scodec->regmap, SUN55I_MICBIAS_AN_CTL,
					   BIT(SUN55I_MMIC_BIAS_EN),
					   BIT(SUN55I_MMIC_BIAS_EN));
			msleep(240);	/* mic-bias settle / anti-pop (BSP: >=80ms) */
			regmap_update_bits(scodec->regmap, SUN55I_ADC_FIFO_CTL,
					   BIT(SUN55I_ADC_DIG_EN),
					   BIT(SUN55I_ADC_DIG_EN));
		}
		mutex_unlock(&scodec->mic_lock);
	} else {
		mutex_lock(&scodec->mic_lock);
		scodec->mic_active[idx] = false;
		first_or_last = !scodec->mic_active[0] && !scodec->mic_active[1] &&
				!scodec->mic_active[2];
		if (first_or_last) {
			regmap_update_bits(scodec->regmap, SUN55I_ADC_FIFO_CTL,
					   BIT(SUN55I_ADC_DIG_EN), 0);
			regmap_update_bits(scodec->regmap, SUN55I_MICBIAS_AN_CTL,
					   BIT(SUN55I_MMIC_BIAS_EN), 0);
		}
		mutex_unlock(&scodec->mic_lock);

		regmap_update_bits(scodec->regmap, an_ctl, BIT(SUN55I_ADCn_EN), 0);
		regmap_update_bits(scodec->regmap, an_ctl,
				   BIT(SUN55I_MICn_PGA_EN), 0);
	}
	return 0;
}

#define SUN55I_MIC_EVENT(n, idx, reg)					\
static int sun55i_mic##n##_event(struct snd_soc_dapm_widget *w,		\
				 struct snd_kcontrol *k, int event)	\
{									\
	struct snd_soc_component *cmp = snd_soc_dapm_to_component(w->dapm); \
	return sun55i_mic_event(snd_soc_component_get_drvdata(cmp),	\
				idx, reg, event);			\
}
SUN55I_MIC_EVENT(1, 0, SUN55I_ADC1_AN_CTL)
SUN55I_MIC_EVENT(2, 1, SUN55I_ADC2_AN_CTL)
SUN55I_MIC_EVENT(3, 2, SUN55I_ADC3_AN_CTL)

static const struct snd_soc_dapm_widget sun55i_codec_widgets[] = {
	SND_SOC_DAPM_AIF_IN_E("DACL", "Playback", 0, SUN55I_DAC_AN_REG,
			      SUN55I_DACL_EN, 0, sun55i_playback_event,
			      SND_SOC_DAPM_POST_PMU | SND_SOC_DAPM_POST_PMD),
	SND_SOC_DAPM_AIF_IN_E("DACR", "Playback", 0, SUN55I_DAC_AN_REG,
			      SUN55I_DACR_EN, 0, sun55i_playback_event,
			      SND_SOC_DAPM_POST_PMU | SND_SOC_DAPM_POST_PMD),
	SND_SOC_DAPM_AIF_OUT("ADC1", "Capture", 0, SUN55I_ADC_DIG_CTL, 0, 0),
	SND_SOC_DAPM_AIF_OUT("ADC2", "Capture", 0, SUN55I_ADC_DIG_CTL, 1, 0),
	SND_SOC_DAPM_AIF_OUT("ADC3", "Capture", 0, SUN55I_ADC_DIG_CTL, 2, 0),

	SND_SOC_DAPM_OUTPUT("LINEOUTLP_PIN"),
	SND_SOC_DAPM_OUTPUT("LINEOUTLN_PIN"),
	SND_SOC_DAPM_OUTPUT("LINEOUTRP_PIN"),
	SND_SOC_DAPM_OUTPUT("LINEOUTRN_PIN"),
	SND_SOC_DAPM_OUTPUT("HPOUTL_PIN"),
	SND_SOC_DAPM_OUTPUT("HPOUTR_PIN"),
	SND_SOC_DAPM_INPUT("MIC1P_PIN"),
	SND_SOC_DAPM_INPUT("MIC1N_PIN"),
	SND_SOC_DAPM_INPUT("MIC2P_PIN"),
	SND_SOC_DAPM_INPUT("MIC2N_PIN"),
	SND_SOC_DAPM_INPUT("MIC3P_PIN"),
	SND_SOC_DAPM_INPUT("MIC3N_PIN"),

	SND_SOC_DAPM_LINE("LINEOUTL", sun55i_lineoutl_event),
	SND_SOC_DAPM_LINE("LINEOUTR", sun55i_lineoutr_event),
	SND_SOC_DAPM_HP("HPOUT", sun55i_hpout_event),
	SND_SOC_DAPM_MIC("MIC1", sun55i_mic1_event),
	SND_SOC_DAPM_MIC("MIC2", sun55i_mic2_event),
	SND_SOC_DAPM_MIC("MIC3", sun55i_mic3_event),
};

static const struct snd_soc_dapm_route sun55i_codec_routes[] = {
	{ "LINEOUTLP_PIN", NULL, "DACL" },
	{ "LINEOUTLN_PIN", NULL, "DACL" },
	{ "LINEOUTRP_PIN", NULL, "DACR" },
	{ "LINEOUTRN_PIN", NULL, "DACR" },
	{ "HPOUTL_PIN", NULL, "DACL" },
	{ "HPOUTR_PIN", NULL, "DACR" },
	{ "ADC1", NULL, "MIC1P_PIN" },
	{ "ADC1", NULL, "MIC1N_PIN" },
	{ "ADC2", NULL, "MIC2P_PIN" },
	{ "ADC2", NULL, "MIC2N_PIN" },
	{ "ADC3", NULL, "MIC3P_PIN" },
	{ "ADC3", NULL, "MIC3N_PIN" },
};

/* One-time analog/digital bring-up (BSP sunxi_codec_init). */
static void sun55i_codec_init(struct sun55i_codec *scodec)
{
	struct regmap *rm = scodec->regmap;

	regmap_update_bits(rm, SUN55I_RAMP, BIT(SUN55I_RAMP_EN),
			   BIT(SUN55I_RAMP_EN));
	regmap_update_bits(rm, SUN55I_DAC_DAP_CTL, BIT(SUN55I_DDAP_EN),
			   BIT(SUN55I_DDAP_EN));
	regmap_update_bits(rm, SUN55I_ADC_DAP_CTL,
			   BIT(SUN55I_ADAP0_EN) | BIT(SUN55I_ADAP1_EN),
			   BIT(SUN55I_ADAP0_EN) | BIT(SUN55I_ADAP1_EN));

	/* Headphone charge-pump LDO on, ~1.2 V. */
	regmap_update_bits(rm, SUN55I_DAC_AN_REG, BIT(SUN55I_CPLDO_EN),
			   BIT(SUN55I_CPLDO_EN));
	regmap_update_bits(rm, SUN55I_DAC_AN_REG, 0x3 << SUN55I_CPLDO_VOLTAGE,
			   0x3 << SUN55I_CPLDO_VOLTAGE);

	/* MIC bias 2.55 V; denoise LDO. */
	regmap_update_bits(rm, SUN55I_MICBIAS_AN_CTL, 0x3 << SUN55I_HMIC_BIAS_SEL,
			   0x3 << SUN55I_HMIC_BIAS_SEL);
	regmap_update_bits(rm, SUN55I_POWER_AN_CTL, BIT(SUN55I_VRP_LDO_EN),
			   BIT(SUN55I_VRP_LDO_EN));

	/* ADC fifo delay. */
	regmap_update_bits(rm, SUN55I_ADC_FIFO_CTL, BIT(SUN55I_ADC_DFEN),
			   BIT(SUN55I_ADC_DFEN));
	regmap_update_bits(rm, SUN55I_ADC_FIFO_CTL, 0x3 << SUN55I_ADC_FDT,
			   0x2 << SUN55I_ADC_FDT);

	/* Volume-update enables. */
	regmap_update_bits(rm, SUN55I_DAC_VOL_CTL, BIT(SUN55I_DAC_VOL_SEL),
			   BIT(SUN55I_DAC_VOL_SEL));
	regmap_update_bits(rm, SUN55I_ADC_DIG_CTL,
			   BIT(SUN55I_ADC1_2_VOL_EN) | BIT(SUN55I_ADC3_VOL_EN),
			   BIT(SUN55I_ADC1_2_VOL_EN) | BIT(SUN55I_ADC3_VOL_EN));
	/* TODO: SID-efuse bias calibration (BIAS_AN_CTL / POWER_AN_CTL). */
}

static int sun55i_codec_component_probe(struct snd_soc_component *component)
{
	struct sun55i_codec *scodec = snd_soc_component_get_drvdata(component);

	sun55i_codec_init(scodec);
	return 0;
}

static const struct snd_soc_component_driver sun55i_codec_codec_component = {
	.probe			= sun55i_codec_component_probe,
	.controls		= sun55i_codec_controls,
	.num_controls		= ARRAY_SIZE(sun55i_codec_controls),
	.dapm_widgets		= sun55i_codec_widgets,
	.num_dapm_widgets	= ARRAY_SIZE(sun55i_codec_widgets),
	.dapm_routes		= sun55i_codec_routes,
	.num_dapm_routes	= ARRAY_SIZE(sun55i_codec_routes),
	.idle_bias_on		= 1,
	.suspend_bias_off	= 1,
};

/* The cpu/platform component: just the dmaengine PCM wrapper. */
static const struct snd_soc_component_driver sun55i_codec_cpu_component = {
	.name			= "sun55i-codec",
	.legacy_dai_naming	= 1,
};

/*
 * ---- headset jack (HMIC) ----
 * HW-GATED: the HMIC insert/removal + mic-button detection is timing-sensitive
 * (the BSP runs a multi-stage DET_MODE/scan dance). This is the faithful but
 * simplified single-pass version; the debounce, det-level and the HMIC_DATA
 * button thresholds need calibration on the device.
 */
static void sun55i_codec_jack_setup(struct sun55i_codec *scodec)
{
	struct regmap *rm = scodec->regmap;

	regmap_update_bits(rm, SUN55I_HMIC_CTL, 0xffff, 0);
	regmap_update_bits(rm, SUN55I_HMIC_STA, 0xffff, 0x6000);

	/* Detection ADC basedata; low-level jack detect (vendor jack-det-level=0). */
	regmap_update_bits(rm, SUN55I_MICBIAS_AN_CTL,
			   0x3 << SUN55I_SEL_DET_ADC_BF, 0x1 << SUN55I_SEL_DET_ADC_BF);
	regmap_update_bits(rm, SUN55I_MICBIAS_AN_CTL,
			   BIT(SUN55I_AUTO_PULL_LOW_EN), BIT(SUN55I_AUTO_PULL_LOW_EN));
	regmap_update_bits(rm, SUN55I_MICBIAS_AN_CTL, BIT(SUN55I_DET_MODE), 0);
	regmap_update_bits(rm, SUN55I_MICBIAS_AN_CTL,
			   BIT(SUN55I_AUTO_PULL_LOW_EN), 0);
	regmap_update_bits(rm, SUN55I_MICBIAS_AN_CTL, BIT(SUN55I_DET_MODE),
			   BIT(SUN55I_DET_MODE));
	regmap_update_bits(rm, SUN55I_MICBIAS_AN_CTL, BIT(SUN55I_JACK_DET_EN),
			   BIT(SUN55I_JACK_DET_EN));

	regmap_update_bits(rm, SUN55I_HMIC_CTL, 0xf << SUN55I_HMIC_N, 0);
	regmap_update_bits(rm, SUN55I_HMIC_CTL, BIT(SUN55I_JACK_IN_IRQ_EN),
			   BIT(SUN55I_JACK_IN_IRQ_EN));
	regmap_update_bits(rm, SUN55I_HMIC_CTL, BIT(SUN55I_JACK_OUT_IRQ_EN),
			   BIT(SUN55I_JACK_OUT_IRQ_EN));
}

static int sun55i_codec_jack_button(unsigned int data)
{
	/* HMIC_DATA (5-bit) -> key, from the vendor jack-key-det-voltage ranges. */
	switch (data) {
	case 0:
		return SND_JACK_BTN_0;		/* hook / play-pause */
	case 1:
		return SND_JACK_BTN_3;		/* voice */
	case 2:
		return SND_JACK_BTN_1;		/* volume up */
	case 4:
	case 5:
		return SND_JACK_BTN_2;		/* volume down */
	default:
		return 0;
	}
}

static irqreturn_t sun55i_codec_jack_irq(int irq, void *data)
{
	struct sun55i_codec *scodec = data;
	struct regmap *rm = scodec->regmap;
	unsigned int sta, report;

	if (!scodec->jack.jack)
		return IRQ_NONE;

	regmap_read(rm, SUN55I_HMIC_STA, &sta);

	if (sta & BIT(SUN55I_JACK_OUT_IRQ_STA)) {
		regmap_update_bits(rm, SUN55I_HMIC_CTL, BIT(SUN55I_MIC_DET_IRQ_EN), 0);
		regmap_update_bits(rm, SUN55I_MICBIAS_AN_CTL,
				   BIT(SUN55I_MIC_DET_ADC_EN), 0);
		snd_soc_jack_report(&scodec->jack, 0,
				    SND_JACK_HEADSET | SUN55I_JACK_BUTTONS);
	} else if (sta & BIT(SUN55I_JACK_IN_IRQ_STA)) {
		regmap_update_bits(rm, SUN55I_MICBIAS_AN_CTL,
				   BIT(SUN55I_MIC_DET_ADC_EN),
				   BIT(SUN55I_MIC_DET_ADC_EN));
		regmap_update_bits(rm, SUN55I_HMIC_CTL, BIT(SUN55I_MIC_DET_IRQ_EN),
				   BIT(SUN55I_MIC_DET_IRQ_EN));
		snd_soc_jack_report(&scodec->jack, SND_JACK_HEADSET,
				    SND_JACK_HEADSET);
	} else if (sta & BIT(SUN55I_MIC_DET_IRQ_STA)) {
		report = sun55i_codec_jack_button((sta >> SUN55I_HMIC_DATA) & 0x1f);
		snd_soc_jack_report(&scodec->jack, report, SUN55I_JACK_BUTTONS);
	}

	regmap_write(rm, SUN55I_HMIC_STA,
		     BIT(SUN55I_JACK_OUT_IRQ_STA) | BIT(SUN55I_JACK_IN_IRQ_STA) |
		     BIT(SUN55I_MIC_DET_IRQ_STA));
	return IRQ_HANDLED;
}

static int sun55i_codec_init_jack(struct snd_soc_pcm_runtime *rtd)
{
	struct snd_soc_card *card = rtd->card;
	struct sun55i_codec *scodec = snd_soc_card_get_drvdata(card);
	int ret;

	if (scodec->irq <= 0)
		return 0;

	ret = snd_soc_card_jack_new(card, "Headset",
				    SND_JACK_HEADSET | SUN55I_JACK_BUTTONS,
				    &scodec->jack);
	if (ret)
		return ret;

	snd_jack_set_key(scodec->jack.jack, SND_JACK_BTN_0, KEY_PLAYPAUSE);
	snd_jack_set_key(scodec->jack.jack, SND_JACK_BTN_1, KEY_VOLUMEUP);
	snd_jack_set_key(scodec->jack.jack, SND_JACK_BTN_2, KEY_VOLUMEDOWN);
	snd_jack_set_key(scodec->jack.jack, SND_JACK_BTN_3, KEY_VOICECOMMAND);

	sun55i_codec_jack_setup(scodec);
	return 0;
}

/* ---- card (the codec self-registers it, like sun4i-codec) ---- */
static int sun55i_codec_spk_event(struct snd_soc_dapm_widget *w,
				  struct snd_kcontrol *k, int event)
{
	struct snd_soc_card *card = snd_soc_dapm_to_card(w->dapm);
	struct sun55i_codec *scodec = snd_soc_card_get_drvdata(card);

	gpiod_set_value_cansleep(scodec->gpio_pa, !!SND_SOC_DAPM_EVENT_ON(event));
	if (SND_SOC_DAPM_EVENT_ON(event))
		msleep(700);	/* let the DAC settle before un-muting the amp */
	return 0;
}

static const struct snd_soc_dapm_widget sun55i_codec_card_widgets[] = {
	SND_SOC_DAPM_SPK("SPK", sun55i_codec_spk_event),
};

static struct snd_soc_dai_link *sun55i_codec_create_link(struct device *dev,
							 int *num_links)
{
	struct snd_soc_dai_link *link = devm_kzalloc(dev, sizeof(*link),
						     GFP_KERNEL);
	struct snd_soc_dai_link_component *dlc = devm_kzalloc(dev, 3 * sizeof(*dlc),
							     GFP_KERNEL);
	if (!link || !dlc)
		return NULL;

	link->cpus	= &dlc[0];
	link->codecs	= &dlc[1];
	link->platforms	= &dlc[2];
	link->num_cpus		= 1;
	link->num_codecs	= 1;
	link->num_platforms	= 1;

	link->name		= "cdc";
	link->stream_name	= "CDC PCM";
	link->codecs->dai_name	= "Codec";
	link->cpus->dai_name	= dev_name(dev);
	link->codecs->name	= dev_name(dev);
	link->platforms->name	= dev_name(dev);
	link->dai_fmt		= SND_SOC_DAIFMT_I2S;
	link->init		= sun55i_codec_init_jack;

	*num_links = 1;
	return link;
}

static struct snd_soc_card *sun55i_codec_create_card(struct device *dev)
{
	struct snd_soc_card *card;
	int ret;

	card = devm_kzalloc(dev, sizeof(*card), GFP_KERNEL);
	if (!card)
		return ERR_PTR(-ENOMEM);

	card->dai_link = sun55i_codec_create_link(dev, &card->num_links);
	if (!card->dai_link)
		return ERR_PTR(-ENOMEM);

	card->dev		= dev;
	card->owner		= THIS_MODULE;
	card->name		= "sun55i-audio";
	card->dapm_widgets	= sun55i_codec_card_widgets;
	card->num_dapm_widgets	= ARRAY_SIZE(sun55i_codec_card_widgets);
	card->fully_routed	= true;

	ret = snd_soc_of_parse_audio_routing(card, "allwinner,audio-routing");
	if (ret)
		dev_warn(dev, "failed to parse audio-routing: %d\n", ret);

	return card;
}

static bool sun55i_codec_volatile_reg(struct device *dev, unsigned int reg)
{
	switch (reg) {
	case SUN55I_DAC_FIFO_STA:
	case SUN55I_DAC_CNT:
	case SUN55I_ADC_FIFO_STA:
	case SUN55I_ADC_RXDATA:
	case SUN55I_ADC_CNT:
	case SUN55I_HMIC_STA:
		return true;
	default:
		return false;
	}
}

static const struct regmap_config sun55i_codec_regmap_config = {
	.reg_bits	= 32,
	.reg_stride	= 4,
	.val_bits	= 32,
	.max_register	= SUN55I_CODEC_MAX_REG,
	.volatile_reg	= sun55i_codec_volatile_reg,
};

static int sun55i_codec_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct sun55i_codec *scodec;
	struct snd_soc_card *card;
	struct resource *res;
	void __iomem *base;
	int ret;

	scodec = devm_kzalloc(dev, sizeof(*scodec), GFP_KERNEL);
	if (!scodec)
		return -ENOMEM;
	scodec->dev = dev;
	mutex_init(&scodec->mic_lock);
	platform_set_drvdata(pdev, scodec);

	base = devm_platform_get_and_ioremap_resource(pdev, 0, &res);
	if (IS_ERR(base))
		return PTR_ERR(base);

	scodec->regmap = devm_regmap_init_mmio(dev, base,
					       &sun55i_codec_regmap_config);
	if (IS_ERR(scodec->regmap))
		return dev_err_probe(dev, PTR_ERR(scodec->regmap),
				     "failed to init regmap\n");

	scodec->clk_bus = devm_clk_get_enabled(dev, "bus");
	if (IS_ERR(scodec->clk_bus))
		return dev_err_probe(dev, PTR_ERR(scodec->clk_bus),
				     "failed to get bus clock\n");
	scodec->clk_dac = devm_clk_get_enabled(dev, "dac");
	if (IS_ERR(scodec->clk_dac))
		return dev_err_probe(dev, PTR_ERR(scodec->clk_dac),
				     "failed to get dac clock\n");
	scodec->clk_adc = devm_clk_get_enabled(dev, "adc");
	if (IS_ERR(scodec->clk_adc))
		return dev_err_probe(dev, PTR_ERR(scodec->clk_adc),
				     "failed to get adc clock\n");

	scodec->clk_pll_audio0_4x = devm_clk_get_optional(dev, "pll-audio0-4x");
	if (IS_ERR(scodec->clk_pll_audio0_4x))
		return PTR_ERR(scodec->clk_pll_audio0_4x);
	scodec->clk_pll_audio1_div5 = devm_clk_get_optional(dev, "pll-audio1-div5");
	if (IS_ERR(scodec->clk_pll_audio1_div5))
		return PTR_ERR(scodec->clk_pll_audio1_div5);

	scodec->rst = devm_reset_control_get_exclusive(dev, NULL);
	if (IS_ERR(scodec->rst))
		return dev_err_probe(dev, PTR_ERR(scodec->rst),
				     "failed to get reset\n");
	ret = reset_control_deassert(scodec->rst);
	if (ret)
		return ret;

	/* All three analog supplies are external on this codec. */
	ret = devm_regulator_get_enable(dev, "avcc");
	if (ret)
		return dev_err_probe(dev, ret, "failed to enable avcc\n");
	ret = devm_regulator_get_enable(dev, "vdd");
	if (ret)
		return dev_err_probe(dev, ret, "failed to enable vdd\n");
	ret = devm_regulator_get_enable(dev, "cpvin");
	if (ret)
		return dev_err_probe(dev, ret, "failed to enable cpvin\n");

	scodec->gpio_pa = devm_gpiod_get_optional(dev, "allwinner,pa",
						  GPIOD_OUT_LOW);
	if (IS_ERR(scodec->gpio_pa))
		return dev_err_probe(dev, PTR_ERR(scodec->gpio_pa),
				     "failed to get pa gpio\n");

	/* Codec IRQ drives the headset/HMIC jack detection (enabled per-jack). */
	scodec->irq = platform_get_irq(pdev, 0);
	if (scodec->irq < 0)
		return scodec->irq;
	ret = devm_request_threaded_irq(dev, scodec->irq, NULL,
					sun55i_codec_jack_irq, IRQF_ONESHOT,
					"sun55i-codec", scodec);
	if (ret)
		return dev_err_probe(dev, ret, "failed to request irq\n");

	scodec->playback_dma.addr	= res->start + SUN55I_DAC_TXDATA;
	scodec->playback_dma.maxburst	= 8;
	scodec->playback_dma.addr_width	= DMA_SLAVE_BUSWIDTH_4_BYTES;
	scodec->capture_dma.addr	= res->start + SUN55I_ADC_RXDATA;
	scodec->capture_dma.maxburst	= 8;
	scodec->capture_dma.addr_width	= DMA_SLAVE_BUSWIDTH_4_BYTES;

	ret = devm_snd_soc_register_component(dev, &sun55i_codec_codec_component,
					      &sun55i_codec_codec_dai, 1);
	if (ret)
		return dev_err_probe(dev, ret, "failed to register codec\n");

	ret = devm_snd_soc_register_component(dev, &sun55i_codec_cpu_component,
					      &sun55i_codec_cpu_dai, 1);
	if (ret)
		return dev_err_probe(dev, ret, "failed to register cpu dai\n");

	ret = devm_snd_dmaengine_pcm_register(dev, NULL, 0);
	if (ret)
		return dev_err_probe(dev, ret, "failed to register PCM\n");

	card = sun55i_codec_create_card(dev);
	if (IS_ERR(card))
		return dev_err_probe(dev, PTR_ERR(card), "failed to create card\n");
	snd_soc_card_set_drvdata(card, scodec);

	ret = devm_snd_soc_register_card(dev, card);
	if (ret)
		return dev_err_probe(dev, ret, "failed to register card\n");

	return 0;
}

static const struct of_device_id sun55i_codec_of_match[] = {
	{ .compatible = "allwinner,sun55i-a523-codec" },
	{ }
};
MODULE_DEVICE_TABLE(of, sun55i_codec_of_match);

static struct platform_driver sun55i_codec_driver = {
	.driver = {
		.name		= "sun55i-codec",
		.of_match_table	= sun55i_codec_of_match,
	},
	.probe	= sun55i_codec_probe,
};
module_platform_driver(sun55i_codec_driver);

MODULE_DESCRIPTION("Allwinner A523 internal audio codec driver");
MODULE_AUTHOR("Midgy BALON <midgy971@gmail.com>");
MODULE_LICENSE("GPL");
