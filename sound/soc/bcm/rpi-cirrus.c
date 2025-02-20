/*
 * ASoC machine driver for Cirrus Logic Audio Card
 * (with WM5102 and WM8804 codecs)
 *
 * Copyright 2015-2017 Matthias Reichl <hias@horus.com>
 *
 * Based on rpi-cirrus-sound-pi driver (c) Wolfson / Cirrus Logic Inc.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/list.h>
#include <linux/delay.h>
#include <sound/pcm_params.h>

#include <linux/mfd/arizona/registers.h>

#include "../codecs/wm5102.h"
#include "../codecs/wm8804.h"

#define WM8804_CLKOUT_HZ 12000000

#define RPI_CIRRUS_DEFAULT_RATE 44100
#define WM5102_MAX_SYSCLK_1 49152000 /* max sysclk for 4K family */
#define WM5102_MAX_SYSCLK_2 45158400 /* max sysclk for 11.025K family */

static inline unsigned int calc_sysclk(unsigned int rate)
{
	return (rate % 4000) ? WM5102_MAX_SYSCLK_2 : WM5102_MAX_SYSCLK_1;
}

enum {
	DAI_WM5102 = 0,
	DAI_WM8804,
};

struct rpi_cirrus_priv {
	/* mutex for synchronzing FLL1 access with DAPM */
	struct mutex lock;
	unsigned int card_rate;
	int sync_path_enable;
	int fll1_freq; /* negative means RefClock in spdif rx case */

	/* track hw params/free for substreams */
	unsigned int params_set;
	unsigned int min_rate_idx, max_rate_idx;
	unsigned char iec958_status[4];
};

/* helper functions */
static inline struct snd_soc_pcm_runtime *get_wm5102_runtime(
	struct snd_soc_card *card) {
	return snd_soc_get_pcm_runtime(card, &card->dai_link[DAI_WM5102]);
}

static inline struct snd_soc_pcm_runtime *get_wm8804_runtime(
	struct snd_soc_card *card) {
	return snd_soc_get_pcm_runtime(card, &card->dai_link[DAI_WM8804]);
}


struct rate_info {
	unsigned int value;
	char *text;
};

static struct rate_info min_rates[] = {
	{     0, "off"},
	{ 32000, "32kHz"},
	{ 44100, "44.1kHz"}
};

#define NUM_MIN_RATES ARRAY_SIZE(min_rates)

static int rpi_cirrus_min_rate_info(struct snd_kcontrol *kcontrol,
	struct snd_ctl_elem_info *uinfo)
{
	uinfo->type = SNDRV_CTL_ELEM_TYPE_ENUMERATED;
	uinfo->count = 1;
	uinfo->value.enumerated.items = NUM_MIN_RATES;

	if (uinfo->value.enumerated.item >= NUM_MIN_RATES)
		uinfo->value.enumerated.item = NUM_MIN_RATES - 1;
	strcpy(uinfo->value.enumerated.name,
		min_rates[uinfo->value.enumerated.item].text);
	return 0;
}

static int rpi_cirrus_min_rate_get(struct snd_kcontrol *kcontrol,
	struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_card *card = snd_kcontrol_chip(kcontrol);
	struct rpi_cirrus_priv *priv = snd_soc_card_get_drvdata(card);

	ucontrol->value.enumerated.item[0] = priv->min_rate_idx;
	return 0;
}

static int rpi_cirrus_min_rate_put(struct snd_kcontrol *kcontrol,
	struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_card *card = snd_kcontrol_chip(kcontrol);
	struct rpi_cirrus_priv *priv = snd_soc_card_get_drvdata(card);
	int changed = 0;

	if (priv->min_rate_idx != ucontrol->value.enumerated.item[0]) {
		changed = 1;
		priv->min_rate_idx = ucontrol->value.enumerated.item[0];
	}

	return changed;
}

static struct rate_info max_rates[] = {
	{     0, "off"},
	{ 48000, "48kHz"},
	{ 96000, "96kHz"}
};

#define NUM_MAX_RATES ARRAY_SIZE(max_rates)

static int rpi_cirrus_max_rate_info(struct snd_kcontrol *kcontrol,
	struct snd_ctl_elem_info *uinfo)
{
	uinfo->type = SNDRV_CTL_ELEM_TYPE_ENUMERATED;
	uinfo->count = 1;
	uinfo->value.enumerated.items = NUM_MAX_RATES;
	if (uinfo->value.enumerated.item >= NUM_MAX_RATES)
		uinfo->value.enumerated.item = NUM_MAX_RATES - 1;
	strcpy(uinfo->value.enumerated.name,
		max_rates[uinfo->value.enumerated.item].text);
	return 0;
}

static int rpi_cirrus_max_rate_get(struct snd_kcontrol *kcontrol,
	struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_card *card = snd_kcontrol_chip(kcontrol);
	struct rpi_cirrus_priv *priv = snd_soc_card_get_drvdata(card);

	ucontrol->value.enumerated.item[0] = priv->max_rate_idx;
	return 0;
}

static int rpi_cirrus_max_rate_put(struct snd_kcontrol *kcontrol,
	struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_card *card = snd_kcontrol_chip(kcontrol);
	struct rpi_cirrus_priv *priv = snd_soc_card_get_drvdata(card);
	int changed = 0;

	if (priv->max_rate_idx != ucontrol->value.enumerated.item[0]) {
		changed = 1;
		priv->max_rate_idx = ucontrol->value.enumerated.item[0];
	}

	return changed;
}

static int rpi_cirrus_spdif_info(struct snd_kcontrol *kcontrol,
	struct snd_ctl_elem_info *uinfo)
{
	uinfo->type = SNDRV_CTL_ELEM_TYPE_IEC958;
	uinfo->count = 1;
	return 0;
}

static int rpi_cirrus_spdif_playback_get(struct snd_kcontrol *kcontrol,
	struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_card *card = snd_kcontrol_chip(kcontrol);
	struct rpi_cirrus_priv *priv = snd_soc_card_get_drvdata(card);
	int i;

	for (i = 0; i < 4; i++)
		ucontrol->value.iec958.status[i] = priv->iec958_status[i];

	return 0;
}

static int rpi_cirrus_spdif_playback_put(struct snd_kcontrol *kcontrol,
	struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_card *card = snd_kcontrol_chip(kcontrol);
	struct snd_soc_component *wm8804_component =
		asoc_rtd_to_codec(get_wm8804_runtime(card), 0)->component;
	struct rpi_cirrus_priv *priv = snd_soc_card_get_drvdata(card);
	unsigned char *stat = priv->iec958_status;
	unsigned char *ctrl_stat = ucontrol->value.iec958.status;
	unsigned int mask;
	int i, changed = 0;

	for (i = 0; i < 4; i++) {
		mask = (i == 3) ? 0x3f : 0xff;
		if ((ctrl_stat[i] & mask) != (stat[i] & mask)) {
			changed = 1;
			stat[i] = ctrl_stat[i] & mask;
			snd_soc_component_update_bits(wm8804_component,
				WM8804_SPDTX1 + i, mask, stat[i]);
		}
	}

	return changed;
}

static int rpi_cirrus_spdif_mask_get(struct snd_kcontrol *kcontrol,
	struct snd_ctl_elem_value *ucontrol)
{
	ucontrol->value.iec958.status[0] = 0xff;
	ucontrol->value.iec958.status[1] = 0xff;
	ucontrol->value.iec958.status[2] = 0xff;
	ucontrol->value.iec958.status[3] = 0x3f;

	return 0;
}

static int rpi_cirrus_spdif_capture_get(struct snd_kcontrol *kcontrol,
	struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_card *card = snd_kcontrol_chip(kcontrol);
	struct snd_soc_component *wm8804_component =
		asoc_rtd_to_codec(get_wm8804_runtime(card), 0)->component;
	unsigned int val, mask;
	int i;

	for (i = 0; i < 4; i++) {
		val = snd_soc_component_read(wm8804_component,
			WM8804_RXCHAN1 + i);
		mask = (i == 3) ? 0x3f : 0xff;
		ucontrol->value.iec958.status[i] = val & mask;
	}

	return 0;
}

#define SPDIF_FLAG_CTRL(desc, reg, bit, invert) \
{ \
		.access =  SNDRV_CTL_ELEM_ACCESS_READ \
			   | SNDRV_CTL_ELEM_ACCESS_VOLATILE, \
		.iface =   SNDRV_CTL_ELEM_IFACE_MIXER, \
		.name =    SNDRV_CTL_NAME_IEC958("", CAPTURE, NONE) \
				desc " Flag", \
		.info =    snd_ctl_boolean_mono_info, \
		.get =     rpi_cirrus_spdif_status_flag_get, \
		.private_value = \
			(bit) | ((reg) << 8) | ((invert) << 16) \
}

static int rpi_cirrus_spdif_status_flag_get(struct snd_kcontrol *kcontrol,
	struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_card *card = snd_kcontrol_chip(kcontrol);
	struct snd_soc_component *wm8804_component =
		asoc_rtd_to_codec(get_wm8804_runtime(card), 0)->component;

	unsigned int bit = kcontrol->private_value & 0xff;
	unsigned int reg = (kcontrol->private_value >> 8) & 0xff;
	unsigned int invert = (kcontrol->private_value >> 16) & 0xff;
	unsigned int val;
	bool flag;

	val = snd_soc_component_read(wm8804_component, reg);

	flag = val & (1 << bit);

	ucontrol->value.integer.value[0] = invert ? !flag : flag;

	return 0;
}

static const char * const recovered_frequency_texts[] = {
	"176.4/192 kHz",
	"88.2/96 kHz",
	"44.1/48 kHz",
	"32 kHz"
};

#define NUM_RECOVERED_FREQUENCIES \
	ARRAY_SIZE(recovered_frequency_texts)

static int rpi_cirrus_recovered_frequency_info(struct snd_kcontrol *kcontrol,
	struct snd_ctl_elem_info *uinfo)
{
	uinfo->type = SNDRV_CTL_ELEM_TYPE_ENUMERATED;
	uinfo->count = 1;
	uinfo->value.enumerated.items = NUM_RECOVERED_FREQUENCIES;
	if (uinfo->value.enumerated.item >= NUM_RECOVERED_FREQUENCIES)
		uinfo->value.enumerated.item = NUM_RECOVERED_FREQUENCIES - 1;
	strcpy(uinfo->value.enumerated.name,
		recovered_frequency_texts[uinfo->value.enumerated.item]);
	return 0;
}

static int rpi_cirrus_recovered_frequency_get(struct snd_kcontrol *kcontrol,
	struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_card *card = snd_kcontrol_chip(kcontrol);
	struct snd_soc_component *wm8804_component =
		asoc_rtd_to_codec(get_wm8804_runtime(card), 0)->component;
	unsigned int val;

	val = snd_soc_component_read(wm8804_component, WM8804_SPDSTAT);

	ucontrol->value.enumerated.item[0] = (val >> 4) & 0x03;
	return 0;
}

static const struct snd_kcontrol_new rpi_cirrus_controls[] = {
	{
		.iface =   SNDRV_CTL_ELEM_IFACE_MIXER,
		.name =    "Min Sample Rate",
		.info =    rpi_cirrus_min_rate_info,
		.get =     rpi_cirrus_min_rate_get,
		.put =     rpi_cirrus_min_rate_put,
	},
	{
		.iface =   SNDRV_CTL_ELEM_IFACE_MIXER,
		.name =    "Max Sample Rate",
		.info =    rpi_cirrus_max_rate_info,
		.get =     rpi_cirrus_max_rate_get,
		.put =     rpi_cirrus_max_rate_put,
	},
	{
		.iface =   SNDRV_CTL_ELEM_IFACE_MIXER,
		.name =    SNDRV_CTL_NAME_IEC958("", PLAYBACK, DEFAULT),
		.info =    rpi_cirrus_spdif_info,
		.get =     rpi_cirrus_spdif_playback_get,
		.put =     rpi_cirrus_spdif_playback_put,
	},
	{
		.access =  SNDRV_CTL_ELEM_ACCESS_READ
			   | SNDRV_CTL_ELEM_ACCESS_VOLATILE,
		.iface =   SNDRV_CTL_ELEM_IFACE_MIXER,
		.name =    SNDRV_CTL_NAME_IEC958("", CAPTURE, DEFAULT),
		.info =    rpi_cirrus_spdif_info,
		.get =     rpi_cirrus_spdif_capture_get,
	},
	{
		.access =  SNDRV_CTL_ELEM_ACCESS_READ,
		.iface =   SNDRV_CTL_ELEM_IFACE_MIXER,
		.name =    SNDRV_CTL_NAME_IEC958("", PLAYBACK, MASK),
		.info =    rpi_cirrus_spdif_info,
		.get =     rpi_cirrus_spdif_mask_get,
	},
	{
		.access =  SNDRV_CTL_ELEM_ACCESS_READ
			   | SNDRV_CTL_ELEM_ACCESS_VOLATILE,
		.iface =   SNDRV_CTL_ELEM_IFACE_MIXER,
		.name =    SNDRV_CTL_NAME_IEC958("", CAPTURE, NONE)
				"Recovered Frequency",
		.info =    rpi_cirrus_recovered_frequency_info,
		.get =     rpi_cirrus_recovered_frequency_get,
	},
	SPDIF_FLAG_CTRL("Audio", WM8804_SPDSTAT, 0, 1),
	SPDIF_FLAG_CTRL("Non-PCM", WM8804_SPDSTAT, 1, 0),
	SPDIF_FLAG_CTRL("Copyright", WM8804_SPDSTAT, 2, 1),
	SPDIF_FLAG_CTRL("De-Emphasis", WM8804_SPDSTAT, 3, 0),
	SPDIF_FLAG_CTRL("Lock", WM8804_SPDSTAT, 6, 1),
	SPDIF_FLAG_CTRL("Invalid", WM8804_INTSTAT, 1, 0),
	SPDIF_FLAG_CTRL("TransErr", WM8804_INTSTAT, 3, 0),
};

static const char * const linein_micbias_texts[] = {
	"off", "on",
};

static SOC_ENUM_SINGLE_VIRT_DECL(linein_micbias_enum,
	linein_micbias_texts);

static const struct snd_kcontrol_new linein_micbias_mux =
	SOC_DAPM_ENUM("Route", linein_micbias_enum);

static int rpi_cirrus_spdif_rx_enable_event(struct snd_soc_dapm_widget *w,
	struct snd_kcontrol *kcontrol, int event);

const struct snd_soc_dapm_widget rpi_cirrus_dapm_widgets[] = {
	SND_SOC_DAPM_MIC("DMIC", NULL),
	SND_SOC_DAPM_MIC("Headset Mic", NULL),
	SND_SOC_DAPM_INPUT("Line Input"),
	SND_SOC_DAPM_MIC("Line Input with Micbias", NULL),
	SND_SOC_DAPM_MUX("Line Input Micbias", SND_SOC_NOPM, 0, 0,
		&linein_micbias_mux),
	SND_SOC_DAPM_INPUT("dummy SPDIF in"),
	SND_SOC_DAPM_PGA_E("dummy SPDIFRX", SND_SOC_NOPM, 0, 0, NULL, 0,
		rpi_cirrus_spdif_rx_enable_event,
		SND_SOC_DAPM_POST_PMU | SND_SOC_DAPM_POST_PMD),
	SND_SOC_DAPM_INPUT("Dummy Input"),
	SND_SOC_DAPM_OUTPUT("Dummy Output"),
};

const struct snd_soc_dapm_route rpi_cirrus_dapm_routes[] = {
	{ "IN1L", NULL, "Headset Mic" },
	{ "IN1R", NULL, "Headset Mic" },
	{ "Headset Mic", NULL, "MICBIAS1" },

	{ "IN2L", NULL, "DMIC" },
	{ "IN2R", NULL, "DMIC" },
	{ "DMIC", NULL, "MICBIAS2" },

	{ "IN3L", NULL, "Line Input Micbias" },
	{ "IN3R", NULL, "Line Input Micbias" },

	{ "Line Input Micbias", "off", "Line Input" },
	{ "Line Input Micbias", "on", "Line Input with Micbias" },

	/* Make sure MICVDD is enabled, otherwise we get noise */
	{ "Line Input", NULL, "MICVDD" },
	{ "Line Input with Micbias", NULL, "MICBIAS3" },

	/* Dummy routes to check whether SPDIF RX is enabled or not */
	{"dummy SPDIFRX", NULL, "dummy SPDIF in"},
	{"AIFTX", NULL, "dummy SPDIFRX"},

	/*
	 * Dummy routes to keep wm5102 from staying off on
	 * playback/capture if all mixers are off.
	 */
	{ "Dummy Output", NULL, "AIF1RX1" },
	{ "Dummy Output", NULL, "AIF1RX2" },
	{ "AIF1TX1", NULL, "Dummy Input" },
	{ "AIF1TX2", NULL, "Dummy Input" },
};

static int rpi_cirrus_clear_flls(struct snd_soc_card *card,
	struct snd_soc_component *wm5102_component) {

	int ret1, ret2;

	ret1 = snd_soc_component_set_pll(wm5102_component,
		WM5102_FLL1, ARIZONA_FLL_SRC_NONE, 0, 0);
	ret2 = snd_soc_component_set_pll(wm5102_component,
		WM5102_FLL1_REFCLK, ARIZONA_FLL_SRC_NONE, 0, 0);

	if (ret1) {
		dev_warn(card->dev,
			"setting FLL1 to zero failed: %d\n", ret1);
		return ret1;
	}
	if (ret2) {
		dev_warn(card->dev,
			"setting FLL1_REFCLK to zero failed: %d\n", ret2);
		return ret2;
	}
	return 0;
}

static int rpi_cirrus_set_fll(struct snd_soc_card *card,
	struct snd_soc_component *wm5102_component, unsigned int clk_freq)
{
	int ret = snd_soc_component_set_pll(wm5102_component,
		WM5102_FLL1,
		ARIZONA_CLK_SRC_MCLK1,
		WM8804_CLKOUT_HZ,
		clk_freq);
	if (ret)
		dev_err(card->dev, "Failed to set FLL1 to %d: %d\n",
			clk_freq, ret);

	usleep_range(1000, 2000);
	return ret;
}

static int rpi_cirrus_set_fll_refclk(struct snd_soc_card *card,
	struct snd_soc_component *wm5102_component,
	unsigned int clk_freq, unsigned int aif2_freq)
{
	int ret = snd_soc_component_set_pll(wm5102_component,
		WM5102_FLL1_REFCLK,
		ARIZONA_CLK_SRC_MCLK1,
		WM8804_CLKOUT_HZ,
		clk_freq);
	if (ret) {
		dev_err(card->dev,
			"Failed to set FLL1_REFCLK to %d: %d\n",
			clk_freq, ret);
		return ret;
	}

	ret = snd_soc_component_set_pll(wm5102_component,
		WM5102_FLL1,
		ARIZONA_CLK_SRC_AIF2BCLK,
		aif2_freq, clk_freq);
	if (ret)
		dev_err(card->dev,
			"Failed to set FLL1 with Sync Clock %d to %d: %d\n",
			aif2_freq, clk_freq, ret);

	usleep_range(1000, 2000);
	return ret;
}

static int rpi_cirrus_spdif_rx_enable_event(struct snd_soc_dapm_widget *w,
	struct snd_kcontrol *kcontrol, int event)
{
	struct snd_soc_card *card = w->dapm->card;
	struct rpi_cirrus_priv *priv = snd_soc_card_get_drvdata(card);
	struct snd_soc_component *wm5102_component =
		asoc_rtd_to_codec(get_wm5102_runtime(card), 0)->component;

	unsigned int clk_freq, aif2_freq;
	int ret = 0;

	switch (event) {
	case SND_SOC_DAPM_POST_PMU:
		mutex_lock(&priv->lock);

		/* Enable sync path in case of SPDIF capture use case */

		clk_freq = calc_sysclk(priv->card_rate);
		aif2_freq = 64 * priv->card_rate;

		dev_dbg(card->dev,
			"spdif_rx: changing FLL1 to use Ref Clock clk: %d spdif: %d\n",
			clk_freq, aif2_freq);

		ret = rpi_cirrus_clear_flls(card, wm5102_component);
		if (ret) {
			dev_err(card->dev, "spdif_rx: failed to clear FLLs\n");
			goto out;
		}

		ret = rpi_cirrus_set_fll_refclk(card, wm5102_component,
			clk_freq, aif2_freq);

		if (ret) {
			dev_err(card->dev, "spdif_rx: failed to set FLLs\n");
			goto out;
		}

		/* set to negative to indicate we're doing spdif rx */
		priv->fll1_freq = -clk_freq;
		priv->sync_path_enable = 1;
		break;

	case SND_SOC_DAPM_POST_PMD:
		mutex_lock(&priv->lock);
		priv->sync_path_enable = 0;
		break;

	default:
		return 0;
	}

out:
	mutex_unlock(&priv->lock);
	return ret;
}

static int rpi_cirrus_set_bias_level(struct snd_soc_card *card,
	struct snd_soc_dapm_context *dapm,
	enum snd_soc_bias_level level)
{
	struct rpi_cirrus_priv *priv = snd_soc_card_get_drvdata(card);
	struct snd_soc_pcm_runtime *wm5102_runtime = get_wm5102_runtime(card);
	struct snd_soc_component *wm5102_component =
		asoc_rtd_to_codec(wm5102_runtime, 0)->component;

	int ret = 0;
	unsigned int clk_freq;

	if (dapm->dev != asoc_rtd_to_codec(wm5102_runtime, 0)->dev)
		return 0;

	switch (level) {
	case SND_SOC_BIAS_PREPARE:
		if (dapm->bias_level == SND_SOC_BIAS_ON)
			break;

		mutex_lock(&priv->lock);

		if (!priv->sync_path_enable) {
			clk_freq = calc_sysclk(priv->card_rate);

			dev_dbg(card->dev,
				"set_bias: changing FLL1 from %d to %d\n",
				priv->fll1_freq, clk_freq);

			ret = rpi_cirrus_set_fll(card,
				wm5102_component, clk_freq);
			if (ret)
				dev_err(card->dev,
					"set_bias: Failed to set FLL1\n");
			else
				priv->fll1_freq = clk_freq;
		}
		mutex_unlock(&priv->lock);
		break;
	default:
		break;
	}

	return ret;
}

static int rpi_cirrus_set_bias_level_post(struct snd_soc_card *card,
	struct snd_soc_dapm_context *dapm,
	enum snd_soc_bias_level level)
{
	struct rpi_cirrus_priv *priv = snd_soc_card_get_drvdata(card);
	struct snd_soc_pcm_runtime *wm5102_runtime = get_wm5102_runtime(card);
	struct snd_soc_component *wm5102_component =
		asoc_rtd_to_codec(wm5102_runtime, 0)->component;

	if (dapm->dev != asoc_rtd_to_codec(wm5102_runtime, 0)->dev)
		return 0;

	switch (level) {
	case SND_SOC_BIAS_STANDBY:
		mutex_lock(&priv->lock);

		dev_dbg(card->dev,
			"set_bias_post: changing FLL1 from %d to off\n",
				priv->fll1_freq);

		if (rpi_cirrus_clear_flls(card, wm5102_component))
			dev_err(card->dev,
				"set_bias_post: failed to clear FLLs\n");
		else
			priv->fll1_freq = 0;

		mutex_unlock(&priv->lock);

		break;
	default:
		break;
	}

	return 0;
}

static int rpi_cirrus_set_wm8804_pll(struct snd_soc_card *card,
	struct snd_soc_dai *wm8804_dai, unsigned int rate)
{
	int ret;

	/* use 256fs */
	unsigned int clk_freq = rate * 256;

	ret = snd_soc_dai_set_pll(wm8804_dai, 0, 0,
		WM8804_CLKOUT_HZ, clk_freq);
	if (ret) {
		dev_err(card->dev,
			"Failed to set WM8804 PLL to %d: %d\n", clk_freq, ret);
		return ret;
	}

	/* Set MCLK as PLL Output */
	ret = snd_soc_dai_set_sysclk(wm8804_dai,
		WM8804_TX_CLKSRC_PLL, clk_freq, 0);
	if (ret) {
		dev_err(card->dev,
			"Failed to set MCLK as PLL Output: %d\n", ret);
		return ret;
	}

	return ret;
}

static int rpi_cirrus_startup(struct snd_pcm_substream *substream)
{
	struct snd_soc_pcm_runtime *rtd = substream->private_data;
	struct snd_soc_card *card = rtd->card;
	struct rpi_cirrus_priv *priv = snd_soc_card_get_drvdata(card);
	unsigned int min_rate = min_rates[priv->min_rate_idx].value;
	unsigned int max_rate = max_rates[priv->max_rate_idx].value;

	if (min_rate || max_rate) {
		if (max_rate == 0)
			max_rate = UINT_MAX;

		dev_dbg(card->dev,
			"startup: limiting rate to %u-%u\n",
			min_rate, max_rate);

		snd_pcm_hw_constraint_minmax(substream->runtime,
			SNDRV_PCM_HW_PARAM_RATE, min_rate, max_rate);
	}

	return 0;
}

static struct snd_soc_pcm_stream rpi_cirrus_dai_link2_params = {
	.formats = SNDRV_PCM_FMTBIT_S24_LE,
	.channels_min = 2,
	.channels_max = 2,
	.rate_min = RPI_CIRRUS_DEFAULT_RATE,
	.rate_max = RPI_CIRRUS_DEFAULT_RATE,
};

static int rpi_cirrus_hw_params(struct snd_pcm_substream *substream,
	struct snd_pcm_hw_params *params)
{
	struct snd_soc_pcm_runtime *rtd = substream->private_data;
	struct snd_soc_card *card = rtd->card;
	struct rpi_cirrus_priv *priv = snd_soc_card_get_drvdata(card);
	struct snd_soc_dai *bcm_i2s_dai = asoc_rtd_to_cpu(rtd, 0);
	struct snd_soc_component *wm5102_component = asoc_rtd_to_codec(rtd, 0)->component;
	struct snd_soc_dai *wm8804_dai = asoc_rtd_to_codec(get_wm8804_runtime(card), 0);

	int ret;

	unsigned int width = snd_pcm_format_width(params_format(params));
	unsigned int rate = params_rate(params);
	unsigned int clk_freq = calc_sysclk(rate);

	mutex_lock(&priv->lock);

	dev_dbg(card->dev, "hw_params: setting rate to %d\n", rate);

	ret = snd_soc_dai_set_bclk_ratio(bcm_i2s_dai, 2 * width);
	if (ret) {
		dev_err(card->dev, "set_bclk_ratio failed: %d\n", ret);
		goto out;
	}

	ret = snd_soc_dai_set_tdm_slot(asoc_rtd_to_codec(rtd, 0), 0x03, 0x03, 2, width);
	if (ret) {
		dev_err(card->dev, "set_tdm_slot failed: %d\n", ret);
		goto out;
	}

	/* WM8804 supports sample rates from 32k only */
	if (rate >= 32000) {
		ret = rpi_cirrus_set_wm8804_pll(card, wm8804_dai, rate);
		if (ret)
			goto out;
	}

	ret = snd_soc_component_set_sysclk(wm5102_component,
		ARIZONA_CLK_SYSCLK,
		ARIZONA_CLK_SRC_FLL1,
		clk_freq,
		SND_SOC_CLOCK_IN);
	if (ret) {
		dev_err(card->dev, "Failed to set SYSCLK: %d\n", ret);
		goto out;
	}

	if ((priv->fll1_freq > 0) && (priv->fll1_freq != clk_freq)) {
		dev_dbg(card->dev,
			"hw_params: changing FLL1 from %d to %d\n",
			priv->fll1_freq, clk_freq);

		if (rpi_cirrus_clear_flls(card, wm5102_component)) {
			dev_err(card->dev, "hw_params: failed to clear FLLs\n");
			goto out;
		}

		if (rpi_cirrus_set_fll(card, wm5102_component, clk_freq)) {
			dev_err(card->dev, "hw_params: failed to set FLL\n");
			goto out;
		}

		priv->fll1_freq = clk_freq;
	}

	priv->card_rate = rate;
	rpi_cirrus_dai_link2_params.rate_min = rate;
	rpi_cirrus_dai_link2_params.rate_max = rate;

	priv->params_set |= 1 << substream->stream;

out:
	mutex_unlock(&priv->lock);

	return ret;
}

static int rpi_cirrus_hw_free(struct snd_pcm_substream *substream)
{
	struct snd_soc_pcm_runtime *rtd = substream->private_data;
	struct snd_soc_card *card = rtd->card;
	struct rpi_cirrus_priv *priv = snd_soc_card_get_drvdata(card);
	struct snd_soc_component *wm5102_component = asoc_rtd_to_codec(rtd, 0)->component;
	int ret;
	unsigned int old_params_set = priv->params_set;

	priv->params_set &= ~(1 << substream->stream);

	/* disable sysclk if this was the last open stream */
	if (priv->params_set == 0 && old_params_set) {
		dev_dbg(card->dev,
			"hw_free: Setting SYSCLK to Zero\n");

		ret = snd_soc_component_set_sysclk(wm5102_component,
			ARIZONA_CLK_SYSCLK,
			ARIZONA_CLK_SRC_FLL1,
			0,
			SND_SOC_CLOCK_IN);
		if (ret)
			dev_err(card->dev,
				"hw_free: Failed to set SYSCLK to Zero: %d\n",
				ret);
	}
	return 0;
}

static int rpi_cirrus_init_wm5102(struct snd_soc_pcm_runtime *rtd)
{
	struct snd_soc_component *component = asoc_rtd_to_codec(rtd, 0)->component;
	int ret;

	/* no 32kHz input, derive it from sysclk if needed  */
	snd_soc_component_update_bits(component,
			ARIZONA_CLOCK_32K_1, ARIZONA_CLK_32K_SRC_MASK, 2);

	if (rpi_cirrus_clear_flls(rtd->card, component))
		dev_warn(rtd->card->dev,
			"init_wm5102: failed to clear FLLs\n");

	ret = snd_soc_component_set_sysclk(component,
		ARIZONA_CLK_SYSCLK, ARIZONA_CLK_SRC_FLL1,
		0, SND_SOC_CLOCK_IN);
	if (ret) {
		dev_err(rtd->card->dev,
			"Failed to set SYSCLK to Zero: %d\n", ret);
		return ret;
	}

	return 0;
}

static int rpi_cirrus_init_wm8804(struct snd_soc_pcm_runtime *rtd)
{
	struct snd_soc_dai *codec_dai = asoc_rtd_to_codec(rtd, 0);
	struct snd_soc_component *component = codec_dai->component;
	struct snd_soc_card *card = rtd->card;
	struct rpi_cirrus_priv *priv = snd_soc_card_get_drvdata(card);
	unsigned int val, mask;
	int i, ret;

	for (i = 0; i < 4; i++) {
		val = snd_soc_component_read(component,
			WM8804_SPDTX1 + i);
		mask = (i == 3) ? 0x3f : 0xff;
		priv->iec958_status[i] = val & mask;
	}

	/* Setup for 256fs */
	ret = snd_soc_dai_set_clkdiv(codec_dai,
		WM8804_MCLK_DIV, WM8804_MCLKDIV_256FS);
	if (ret) {
		dev_err(card->dev,
			"init_wm8804: Failed to set MCLK_DIV to 256fs: %d\n",
			ret);
		return ret;
	}

	/* Output OSC on CLKOUT */
	ret = snd_soc_dai_set_sysclk(codec_dai,
		WM8804_CLKOUT_SRC_OSCCLK, WM8804_CLKOUT_HZ, 0);
	if (ret)
		dev_err(card->dev,
			"init_wm8804: Failed to set CLKOUT as OSC Frequency: %d\n",
			ret);

	/* Init PLL with default samplerate */
	ret = rpi_cirrus_set_wm8804_pll(card, codec_dai,
		RPI_CIRRUS_DEFAULT_RATE);
	if (ret)
		dev_err(card->dev,
			"init_wm8804: Failed to setup PLL for %dHz: %d\n",
			RPI_CIRRUS_DEFAULT_RATE, ret);

	return ret;
}

static struct snd_soc_ops rpi_cirrus_ops = {
	.startup = rpi_cirrus_startup,
	.hw_params = rpi_cirrus_hw_params,
	.hw_free = rpi_cirrus_hw_free,
};

SND_SOC_DAILINK_DEFS(wm5102,
     DAILINK_COMP_ARRAY(COMP_EMPTY()),
     DAILINK_COMP_ARRAY(COMP_CODEC("wm5102-codec", "wm5102-aif1")),
     DAILINK_COMP_ARRAY(COMP_EMPTY()));

SND_SOC_DAILINK_DEFS(wm8804,
     DAILINK_COMP_ARRAY(COMP_CPU("wm5102-aif2")),
     DAILINK_COMP_ARRAY(COMP_CODEC("wm8804.1-003b", "wm8804-spdif")));

static struct snd_soc_dai_link rpi_cirrus_dai[] = {
	[DAI_WM5102] = {
		.name		= "WM5102",
		.stream_name	= "WM5102 AiFi",
		.dai_fmt	=   SND_SOC_DAIFMT_I2S
				  | SND_SOC_DAIFMT_NB_NF
				  | SND_SOC_DAIFMT_CBM_CFM,
		.ops		= &rpi_cirrus_ops,
		.init		= rpi_cirrus_init_wm5102,
		SND_SOC_DAILINK_REG(wm5102),
	},
	[DAI_WM8804] = {
		.name		= "WM5102 SPDIF",
		.stream_name	= "SPDIF Tx/Rx",
		.dai_fmt	=   SND_SOC_DAIFMT_I2S
				  | SND_SOC_DAIFMT_NB_NF
				  | SND_SOC_DAIFMT_CBM_CFM,
		.ignore_suspend = 1,
		.c2c_params	= &rpi_cirrus_dai_link2_params,
		.init		= rpi_cirrus_init_wm8804,
		SND_SOC_DAILINK_REG(wm8804),
	},
};


static int rpi_cirrus_late_probe(struct snd_soc_card *card)
{
	struct rpi_cirrus_priv *priv = snd_soc_card_get_drvdata(card);
	struct snd_soc_pcm_runtime *wm5102_runtime = get_wm5102_runtime(card);
	struct snd_soc_pcm_runtime *wm8804_runtime = get_wm8804_runtime(card);
	int ret;

	dev_dbg(card->dev, "iec958_bits: %02x %02x %02x %02x\n",
		priv->iec958_status[0],
		priv->iec958_status[1],
		priv->iec958_status[2],
		priv->iec958_status[3]);

	ret = snd_soc_dai_set_sysclk(
		asoc_rtd_to_codec(wm5102_runtime, 0), ARIZONA_CLK_SYSCLK, 0, 0);
	if (ret) {
		dev_err(card->dev,
			"Failed to set WM5102 codec dai clk domain: %d\n", ret);
		return ret;
	}

	ret = snd_soc_dai_set_sysclk(
		asoc_rtd_to_cpu(wm8804_runtime, 0), ARIZONA_CLK_SYSCLK, 0, 0);
	if (ret)
		dev_err(card->dev,
			"Failed to set WM8804 codec dai clk domain: %d\n", ret);

	return ret;
}

/* audio machine driver */
static struct snd_soc_card rpi_cirrus_card = {
	.name			= "RPi-Cirrus",
	.driver_name		= "RPiCirrus",
	.owner			= THIS_MODULE,
	.dai_link		= rpi_cirrus_dai,
	.num_links		= ARRAY_SIZE(rpi_cirrus_dai),
	.late_probe		= rpi_cirrus_late_probe,
	.controls		= rpi_cirrus_controls,
	.num_controls		= ARRAY_SIZE(rpi_cirrus_controls),
	.dapm_widgets		= rpi_cirrus_dapm_widgets,
	.num_dapm_widgets	= ARRAY_SIZE(rpi_cirrus_dapm_widgets),
	.dapm_routes		= rpi_cirrus_dapm_routes,
	.num_dapm_routes	= ARRAY_SIZE(rpi_cirrus_dapm_routes),
	.set_bias_level		= rpi_cirrus_set_bias_level,
	.set_bias_level_post	= rpi_cirrus_set_bias_level_post,
};

static int rpi_cirrus_probe(struct platform_device *pdev)
{
	int ret = 0;
	struct rpi_cirrus_priv *priv;
	struct device_node *i2s_node;

	priv = devm_kzalloc(&pdev->dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	priv->min_rate_idx = 1; /* min samplerate 32kHz */
	priv->card_rate = RPI_CIRRUS_DEFAULT_RATE;

	mutex_init(&priv->lock);

	snd_soc_card_set_drvdata(&rpi_cirrus_card, priv);

	if (!pdev->dev.of_node)
		return -ENODEV;

	i2s_node = of_parse_phandle(
			pdev->dev.of_node, "i2s-controller", 0);
	if (!i2s_node) {
		dev_err(&pdev->dev, "i2s-controller missing in DT\n");
		return -ENODEV;
	}

	rpi_cirrus_dai[DAI_WM5102].cpus->of_node = i2s_node;
	rpi_cirrus_dai[DAI_WM5102].platforms->of_node = i2s_node;

	rpi_cirrus_card.dev = &pdev->dev;

	ret = devm_snd_soc_register_card(&pdev->dev, &rpi_cirrus_card);
	if (ret) {
		if (ret == -EPROBE_DEFER)
			dev_dbg(&pdev->dev,
				"register card requested probe deferral\n");
		else
			dev_err(&pdev->dev,
				"Failed to register card: %d\n", ret);
	}

	return ret;
}

static const struct of_device_id rpi_cirrus_of_match[] = {
	{ .compatible = "wlf,rpi-cirrus", },
	{},
};
MODULE_DEVICE_TABLE(of, rpi_cirrus_of_match);

static struct platform_driver rpi_cirrus_driver = {
	.driver	= {
		.name   = "snd-rpi-cirrus",
		.of_match_table = of_match_ptr(rpi_cirrus_of_match),
	},
	.probe	= rpi_cirrus_probe,
};

module_platform_driver(rpi_cirrus_driver);

MODULE_AUTHOR("Matthias Reichl <hias@horus.com>");
MODULE_DESCRIPTION("ASoC driver for Cirrus Logic Audio Card");
MODULE_LICENSE("GPL");
