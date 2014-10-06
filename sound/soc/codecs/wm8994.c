/*
 * wm8994.c  --  WM8994 ALSA SoC Audio driver
 *
 * Copyright 2009 Wolfson Microelectronics plc
 *
 * Author: Mark Brown <broonie@opensource.wolfsonmicro.com>
 *
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/init.h>
#include <linux/delay.h>
#include <linux/pm.h>
#include <linux/i2c.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>
#include <linux/regulator/consumer.h>
#include <linux/slab.h>
#include <sound/core.h>
#include <sound/jack.h>
#include <sound/pcm.h>
#include <sound/pcm_params.h>
#include <sound/soc.h>
#include <sound/initval.h>
#include <sound/tlv.h>
#include <trace/events/asoc.h>

#include <linux/mfd/wm8994/core.h>
#include <linux/mfd/wm8994/registers.h>
#include <linux/mfd/wm8994/pdata.h>
#include <linux/mfd/wm8994/gpio.h>

#include <linux/i2c/fm34_we395.h>

#include "wm8994.h"
#include "wm_hubs.h"

#include "boeffla_sound.h"


#define WM1811_JACKDET_MODE_NONE  0x0000
#define WM1811_JACKDET_MODE_JACK  0x0100
#define WM1811_JACKDET_MODE_MIC   0x0080
#define WM1811_JACKDET_MODE_AUDIO 0x0180

#define WM8994_NUM_DRC 3
#define WM8994_NUM_EQ  3

static int wm8994_drc_base[] = {
	WM8994_AIF1_DRC1_1,
	WM8994_AIF1_DRC2_1,
	WM8994_AIF2_DRC_1,
};

static int wm8994_retune_mobile_base[] = {
	WM8994_AIF1_DAC1_EQ_GAINS_1,
	WM8994_AIF1_DAC2_EQ_GAINS_1,
	WM8994_AIF2_EQ_GAINS_1,
};

static const struct wm8958_micd_rate micdet_rates[] = {
	{ 32768,       true,  1, 4 },
	{ 32768,       false, 1, 1 },
	{ 44100 * 256, true,  7, 10 },
	{ 44100 * 256, false, 7, 10 },
};

static const struct wm8958_micd_rate jackdet_rates[] = {
	{ 32768,       true,  0, 1 },
	{ 32768,       false, 0, 1 },
	{ 44100 * 256, true,  10, 10 },
	{ 44100 * 256, false, 7, 8 },
};

static void wm8958_micd_set_rate(struct snd_soc_codec *codec)
{
	struct wm8994_priv *wm8994 = snd_soc_codec_get_drvdata(codec);
	int best, i, sysclk, val;
	bool idle;
	const struct wm8958_micd_rate *rates;
	int num_rates;

	idle = !wm8994->jack_mic;

	sysclk = snd_soc_read(codec, WM8994_CLOCKING_1);
	if (sysclk & WM8994_SYSCLK_SRC)
		sysclk = wm8994->aifclk[1];
	else
		sysclk = wm8994->aifclk[0];

	if (wm8994->pdata && wm8994->pdata->micd_rates) {
		rates = wm8994->pdata->micd_rates;
		num_rates = wm8994->pdata->num_micd_rates;
	} else if (wm8994->jackdet) {
		rates = jackdet_rates;
		num_rates = ARRAY_SIZE(jackdet_rates);
	} else {
		rates = micdet_rates;
		num_rates = ARRAY_SIZE(micdet_rates);
	}

	best = 0;
	for (i = 0; i < num_rates; i++) {
		if (rates[i].idle != idle)
			continue;
		if (abs(rates[i].sysclk - sysclk) <
		    abs(rates[best].sysclk - sysclk))
			best = i;
		else if (rates[best].idle != idle)
			best = i;
	}

	val = rates[best].start << WM8958_MICD_BIAS_STARTTIME_SHIFT
		| rates[best].rate << WM8958_MICD_RATE_SHIFT;

	dev_dbg(codec->dev, "MICD rate %d,%d for %dHz %s\n",
		rates[best].start, rates[best].rate, sysclk,
		idle ? "idle" : "active");

	snd_soc_update_bits(codec, WM8958_MIC_DETECT_1,
			    WM8958_MICD_BIAS_STARTTIME_MASK |
			    WM8958_MICD_RATE_MASK, val);
}

static int wm8994_readable(struct snd_soc_codec *codec, unsigned int reg)
{
	struct wm8994_priv *wm8994 = snd_soc_codec_get_drvdata(codec);
	struct wm8994 *control = codec->control_data;

	switch (reg) {
	case WM8994_GPIO_1:
	case WM8994_GPIO_2:
	case WM8994_GPIO_3:
	case WM8994_GPIO_4:
	case WM8994_GPIO_5:
	case WM8994_GPIO_6:
	case WM8994_GPIO_7:
	case WM8994_GPIO_8:
	case WM8994_GPIO_9:
	case WM8994_GPIO_10:
	case WM8994_GPIO_11:
	case WM8994_INTERRUPT_STATUS_1:
	case WM8994_INTERRUPT_STATUS_2:
	case WM8994_INTERRUPT_STATUS_1_MASK:
	case WM8994_INTERRUPT_STATUS_2_MASK:
	case WM8994_INTERRUPT_RAW_STATUS_2:
		return 1;

	case WM8958_DSP2_PROGRAM:
	case WM8958_DSP2_CONFIG:
	case WM8958_DSP2_EXECCONTROL:
		if (control->type == WM8958)
			return 1;
		else
			return 0;

	default:
		break;
	}

	if (reg >= WM8994_CACHE_SIZE)
		return 0;
	return wm8994_access_masks[reg].readable != 0;
}

static int wm8994_volatile(struct snd_soc_codec *codec, unsigned int reg)
{
	if (reg >= WM8994_CACHE_SIZE)
		return 1;

	switch (reg) {
	case WM8994_SOFTWARE_RESET:
	case WM8994_CHIP_REVISION:
	case WM8994_DC_SERVO_1:
	case WM8994_DC_SERVO_READBACK:
	case WM8994_RATE_STATUS:
	case WM8994_LDO_1:
	case WM8994_LDO_2:
	case WM8958_DSP2_EXECCONTROL:
	case WM8958_MIC_DETECT_3:
	case WM8994_DC_SERVO_4E:
		return 1;
	default:
		return 0;
	}
}

static int wm8994_write(struct snd_soc_codec *codec, unsigned int reg,
	unsigned int value)
{
	int ret;

	BUG_ON(reg > WM8994_MAX_REGISTER);

	value = Boeffla_sound_hook_wm8994_write(reg, value);

	if (!wm8994_volatile(codec, reg)) {
		ret = snd_soc_cache_write(codec, reg, value);
		if (ret != 0)
			dev_err(codec->dev, "Cache write to %x failed: %d\n",
				reg, ret);
	}

	return wm8994_reg_write(codec->control_data, reg, value);
}

static unsigned int wm8994_read(struct snd_soc_codec *codec,
				unsigned int reg)
{
	unsigned int val;
	int ret;

	BUG_ON(reg > WM8994_MAX_REGISTER);

	if (!wm8994_volatile(codec, reg) && wm8994_readable(codec, reg) &&
	    reg < codec->driver->reg_cache_size) {
		ret = snd_soc_cache_read(codec, reg, &val);
		if (ret >= 0)
			return val;
		else
			dev_err(codec->dev, "Cache read from %x failed: %d\n",
				reg, ret);
	}

	return wm8994_reg_read(codec->control_data, reg);
}

static int configure_aif_clock(struct snd_soc_codec *codec, int aif)
{
	struct wm8994_priv *wm8994 = snd_soc_codec_get_drvdata(codec);
	int rate;
	int reg1 = 0;
	int offset;

	if (aif)
		offset = 4;
	else
		offset = 0;

	switch (wm8994->sysclk[aif]) {
	case WM8994_SYSCLK_MCLK1:
		rate = wm8994->mclk[0];
		break;

	case WM8994_SYSCLK_MCLK2:
		reg1 |= 0x8;
		rate = wm8994->mclk[1];
		break;

	case WM8994_SYSCLK_FLL1:
		reg1 |= 0x10;
		rate = wm8994->fll[0].out;
		break;

	case WM8994_SYSCLK_FLL2:
		reg1 |= 0x18;
		rate = wm8994->fll[1].out;
		break;

	default:
		return -EINVAL;
	}

	if (rate >= 13500000) {
		rate /= 2;
		reg1 |= WM8994_AIF1CLK_DIV;

		dev_dbg(codec->dev, "Dividing AIF%d clock to %dHz\n",
			aif + 1, rate);
	}

	wm8994->aifclk[aif] = rate;

	snd_soc_update_bits(codec, WM8994_AIF1_CLOCKING_1 + offset,
			    WM8994_AIF1CLK_SRC_MASK | WM8994_AIF1CLK_DIV,
			    reg1);

	return 0;
}

static int configure_clock(struct snd_soc_codec *codec)
{
	struct wm8994_priv *wm8994 = snd_soc_codec_get_drvdata(codec);
	int new, change;

	/* Bring up the AIF clocks first */
	configure_aif_clock(codec, 0);
	configure_aif_clock(codec, 1);

	/* Then switch CLK_SYS over to the higher of them; a change
	 * can only happen as a result of a clocking change which can
	 * only be made outside of DAPM so we can safely redo the
	 * clocking.
	 */

	/* If they're equal it doesn't matter which is used */
	if (wm8994->aifclk[0] == wm8994->aifclk[1]) {
		wm8958_micd_set_rate(codec);
		return 0;
	}

	if (wm8994->aifclk[0] < wm8994->aifclk[1])
		new = WM8994_SYSCLK_SRC;
	else
		new = 0;

	change = snd_soc_update_bits(codec, WM8994_CLOCKING_1,
				     WM8994_SYSCLK_SRC, new);
	if (change)
		snd_soc_dapm_sync(&codec->dapm);

	wm8958_micd_set_rate(codec);

	return 0;
}

static int check_clk_sys(struct snd_soc_dapm_widget *source,
			 struct snd_soc_dapm_widget *sink)
{
	int reg = snd_soc_read(source->codec, WM8994_CLOCKING_1);
	const char *clk;

	/* Check what we're currently using for CLK_SYS */
	if (reg & WM8994_SYSCLK_SRC)
		clk = "AIF2CLK";
	else
		clk = "AIF1CLK";

	return strcmp(source->name, clk) == 0;
}

static const char *sidetone_hpf_text[] = {
	"2.7kHz", "1.35kHz", "675Hz", "370Hz", "180Hz", "90Hz", "45Hz"
};

static const struct soc_enum sidetone_hpf =
	SOC_ENUM_SINGLE(WM8994_SIDETONE, 7, 7, sidetone_hpf_text);

static const char *adc_hpf_text[] = {
	"HiFi", "Voice 1", "Voice 2", "Voice 3"
};

static const struct soc_enum aif1adc1_hpf =
	SOC_ENUM_SINGLE(WM8994_AIF1_ADC1_FILTERS, 13, 4, adc_hpf_text);

static const struct soc_enum aif1adc2_hpf =
	SOC_ENUM_SINGLE(WM8994_AIF1_ADC2_FILTERS, 13, 4, adc_hpf_text);

static const struct soc_enum aif2adc_hpf =
	SOC_ENUM_SINGLE(WM8994_AIF2_ADC_FILTERS, 13, 4, adc_hpf_text);

static const DECLARE_TLV_DB_SCALE(aif_tlv, 0, 600, 0);
static const DECLARE_TLV_DB_SCALE(digital_tlv, -7200, 75, 1);
static const DECLARE_TLV_DB_SCALE(st_tlv, -3600, 300, 0);
static const DECLARE_TLV_DB_SCALE(wm8994_3d_tlv, -1600, 183, 0);
static const DECLARE_TLV_DB_SCALE(eq_tlv, -1200, 100, 0);
static const DECLARE_TLV_DB_SCALE(ng_tlv, -10200, 600, 0);
static const DECLARE_TLV_DB_SCALE(mixin_boost_tlv, 0, 900, 0);

#define WM8994_DRC_SWITCH(xname, reg, shift) \
{	.iface = SNDRV_CTL_ELEM_IFACE_MIXER, .name = xname, \
	.info = snd_soc_info_volsw, .get = snd_soc_get_volsw,\
	.put = wm8994_put_drc_sw, \
	.private_value =  SOC_SINGLE_VALUE(reg, shift, 1, 0) }

static int wm8994_put_drc_sw(struct snd_kcontrol *kcontrol,
			     struct snd_ctl_elem_value *ucontrol)
{
	struct soc_mixer_control *mc =
		(struct soc_mixer_control *)kcontrol->private_value;
	struct snd_soc_codec *codec = snd_kcontrol_chip(kcontrol);
	int mask, ret;

	/* Can't enable both ADC and DAC paths simultaneously */
	if (mc->shift == WM8994_AIF1DAC1_DRC_ENA_SHIFT)
		mask = WM8994_AIF1ADC1L_DRC_ENA_MASK |
			WM8994_AIF1ADC1R_DRC_ENA_MASK;
	else
		mask = WM8994_AIF1DAC1_DRC_ENA_MASK;

	ret = snd_soc_read(codec, mc->reg);
	if (ret < 0)
		return ret;
	if (ret & mask)
		return -EINVAL;

	return snd_soc_put_volsw(kcontrol, ucontrol);
}

static void wm8994_set_drc(struct snd_soc_codec *codec, int drc)
{
	struct wm8994_priv *wm8994 = snd_soc_codec_get_drvdata(codec);
	struct wm8994_pdata *pdata = wm8994->pdata;
	int base = wm8994_drc_base[drc];
	int cfg = wm8994->drc_cfg[drc];
	int save, i;

	/* Save any enables; the configuration should clear them. */
	save = snd_soc_read(codec, base);
	save &= WM8994_AIF1DAC1_DRC_ENA | WM8994_AIF1ADC1L_DRC_ENA |
		WM8994_AIF1ADC1R_DRC_ENA;

	for (i = 0; i < WM8994_DRC_REGS; i++)
		snd_soc_update_bits(codec, base + i, 0xffff,
				    pdata->drc_cfgs[cfg].regs[i]);

	snd_soc_update_bits(codec, base, WM8994_AIF1DAC1_DRC_ENA |
			     WM8994_AIF1ADC1L_DRC_ENA |
			     WM8994_AIF1ADC1R_DRC_ENA, save);
}

/* Icky as hell but saves code duplication */
static int wm8994_get_drc(const char *name)
{
	if (strcmp(name, "AIF1DRC1 Mode") == 0)
		return 0;
	if (strcmp(name, "AIF1DRC2 Mode") == 0)
		return 1;
	if (strcmp(name, "AIF2DRC Mode") == 0)
		return 2;
	return -EINVAL;
}

static int wm8994_put_drc_enum(struct snd_kcontrol *kcontrol,
			       struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_codec *codec = snd_kcontrol_chip(kcontrol);
	struct wm8994_priv *wm8994 = snd_soc_codec_get_drvdata(codec);
	struct wm8994_pdata *pdata = wm8994->pdata;
	int drc = wm8994_get_drc(kcontrol->id.name);
	int value = ucontrol->value.integer.value[0];

	if (drc < 0)
		return drc;

	if (value >= pdata->num_drc_cfgs)
		return -EINVAL;

	wm8994->drc_cfg[drc] = value;

	wm8994_set_drc(codec, drc);

	return 0;
}

static int wm8994_get_drc_enum(struct snd_kcontrol *kcontrol,
			       struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_codec *codec = snd_kcontrol_chip(kcontrol);
	struct wm8994_priv *wm8994 = snd_soc_codec_get_drvdata(codec);
	int drc = wm8994_get_drc(kcontrol->id.name);

	ucontrol->value.enumerated.item[0] = wm8994->drc_cfg[drc];

	return 0;
}

static void wm8994_set_retune_mobile(struct snd_soc_codec *codec, int block)
{
	struct wm8994_priv *wm8994 = snd_soc_codec_get_drvdata(codec);
	struct wm8994_pdata *pdata = wm8994->pdata;
	int base = wm8994_retune_mobile_base[block];
	int iface, best, best_val, save, i, cfg;

	if (!pdata || !wm8994->num_retune_mobile_texts)
		return;

	switch (block) {
	case 0:
	case 1:
		iface = 0;
		break;
	case 2:
		iface = 1;
		break;
	default:
		return;
	}

	/* Find the version of the currently selected configuration
	 * with the nearest sample rate. */
	cfg = wm8994->retune_mobile_cfg[block];
	best = 0;
	best_val = INT_MAX;
	for (i = 0; i < pdata->num_retune_mobile_cfgs; i++) {
		if (strcmp(pdata->retune_mobile_cfgs[i].name,
			   wm8994->retune_mobile_texts[cfg]) == 0 &&
		    abs(pdata->retune_mobile_cfgs[i].rate
			- wm8994->dac_rates[iface]) < best_val) {
			best = i;
			best_val = abs(pdata->retune_mobile_cfgs[i].rate
				       - wm8994->dac_rates[iface]);
		}
	}

	dev_dbg(codec->dev, "ReTune Mobile %d %s/%dHz for %dHz sample rate\n",
		block,
		pdata->retune_mobile_cfgs[best].name,
		pdata->retune_mobile_cfgs[best].rate,
		wm8994->dac_rates[iface]);

	/* The EQ will be disabled while reconfiguring it, remember the
	 * current configuration.
	 */
	save = snd_soc_read(codec, base);
	save &= WM8994_AIF1DAC1_EQ_ENA;

	for (i = 0; i < WM8994_EQ_REGS; i++)
		snd_soc_update_bits(codec, base + i, 0xffff,
				pdata->retune_mobile_cfgs[best].regs[i]);

	snd_soc_update_bits(codec, base, WM8994_AIF1DAC1_EQ_ENA, save);
}

/* Icky as hell but saves code duplication */
static int wm8994_get_retune_mobile_block(const char *name)
{
	if (strcmp(name, "AIF1.1 EQ Mode") == 0)
		return 0;
	if (strcmp(name, "AIF1.2 EQ Mode") == 0)
		return 1;
	if (strcmp(name, "AIF2 EQ Mode") == 0)
		return 2;
	return -EINVAL;
}

static int wm8994_put_retune_mobile_enum(struct snd_kcontrol *kcontrol,
					 struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_codec *codec = snd_kcontrol_chip(kcontrol);
	struct wm8994_priv *wm8994 = snd_soc_codec_get_drvdata(codec);
	struct wm8994_pdata *pdata = wm8994->pdata;
	int block = wm8994_get_retune_mobile_block(kcontrol->id.name);
	int value = ucontrol->value.integer.value[0];

	if (block < 0)
		return block;

	if (value >= pdata->num_retune_mobile_cfgs)
		return -EINVAL;

	wm8994->retune_mobile_cfg[block] = value;

	wm8994_set_retune_mobile(codec, block);

	return 0;
}

static int wm8994_get_retune_mobile_enum(struct snd_kcontrol *kcontrol,
					 struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_codec *codec = snd_kcontrol_chip(kcontrol);
	struct wm8994_priv *wm8994 = snd_soc_codec_get_drvdata(codec);
	int block = wm8994_get_retune_mobile_block(kcontrol->id.name);

	ucontrol->value.enumerated.item[0] = wm8994->retune_mobile_cfg[block];

	return 0;
}

#if defined(CONFIG_MACH_C1_KOR_LGT) || defined(CONFIG_MACH_BAFFIN_KOR_LGT)
static const char *fm_control[] = {
"OFF", "RCV", "EAR", "SPK", "SPK",
};

static const struct soc_enum fm_control_enum[] = {
SOC_ENUM_SINGLE_EXT(ARRAY_SIZE(fm_control), fm_control),
};

static int wm8994_get_fm_control(struct snd_kcontrol *kcontrol,
					struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_codec *codec = snd_kcontrol_chip(kcontrol);
	struct wm8994_priv *wm8994 = snd_soc_codec_get_drvdata(codec);

	return 0;
}

static int wm8994_put_fm_control(struct snd_kcontrol *kcontrol,
					struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_codec *codec = snd_kcontrol_chip(kcontrol);
	struct wm8994_priv *wm8994 = snd_soc_codec_get_drvdata(codec);
	int value = ucontrol->value.integer.value[0];
	int ret = 0;

#ifdef CONFIG_FM34_WE395
	ret = fm34_set_mode(value);
#endif
	return ret;
}
#endif /* CONFIG_MACH_C1_KOR_LGT */

static const char *aif_chan_src_text[] = {
	"Left", "Right"
};

static const struct soc_enum aif1adcl_src =
	SOC_ENUM_SINGLE(WM8994_AIF1_CONTROL_1, 15, 2, aif_chan_src_text);

static const struct soc_enum aif1adcr_src =
	SOC_ENUM_SINGLE(WM8994_AIF1_CONTROL_1, 14, 2, aif_chan_src_text);

static const struct soc_enum aif2adcl_src =
	SOC_ENUM_SINGLE(WM8994_AIF2_CONTROL_1, 15, 2, aif_chan_src_text);

static const struct soc_enum aif2adcr_src =
	SOC_ENUM_SINGLE(WM8994_AIF2_CONTROL_1, 14, 2, aif_chan_src_text);

static const struct soc_enum aif1dacl_src =
	SOC_ENUM_SINGLE(WM8994_AIF1_CONTROL_2, 15, 2, aif_chan_src_text);

static const struct soc_enum aif1dacr_src =
	SOC_ENUM_SINGLE(WM8994_AIF1_CONTROL_2, 14, 2, aif_chan_src_text);

static const struct soc_enum aif2dacl_src =
	SOC_ENUM_SINGLE(WM8994_AIF2_CONTROL_2, 15, 2, aif_chan_src_text);

static const struct soc_enum aif2dacr_src =
	SOC_ENUM_SINGLE(WM8994_AIF2_CONTROL_2, 14, 2, aif_chan_src_text);

static const char *osr_text[] = {
	"Low Power", "High Performance",
};

static const struct soc_enum dac_osr =
	SOC_ENUM_SINGLE(WM8994_OVERSAMPLING, 0, 2, osr_text);

static const struct soc_enum adc_osr =
	SOC_ENUM_SINGLE(WM8994_OVERSAMPLING, 1, 2, osr_text);

static const struct snd_kcontrol_new wm8994_snd_controls[] = {
SOC_DOUBLE_R_TLV("AIF1ADC1 Volume", WM8994_AIF1_ADC1_LEFT_VOLUME,
		 WM8994_AIF1_ADC1_RIGHT_VOLUME,
		 1, 119, 0, digital_tlv),
SOC_DOUBLE_R_TLV("AIF1ADC2 Volume", WM8994_AIF1_ADC2_LEFT_VOLUME,
		 WM8994_AIF1_ADC2_RIGHT_VOLUME,
		 1, 119, 0, digital_tlv),
SOC_DOUBLE_R_TLV("AIF2ADC Volume", WM8994_AIF2_ADC_LEFT_VOLUME,
		 WM8994_AIF2_ADC_RIGHT_VOLUME,
		 1, 119, 0, digital_tlv),

SOC_ENUM("AIF1ADCL Source", aif1adcl_src),
SOC_ENUM("AIF1ADCR Source", aif1adcr_src),
SOC_ENUM("AIF2ADCL Source", aif2adcl_src),
SOC_ENUM("AIF2ADCR Source", aif2adcr_src),

SOC_ENUM("AIF1DACL Source", aif1dacl_src),
SOC_ENUM("AIF1DACR Source", aif1dacr_src),
SOC_ENUM("AIF2DACL Source", aif2dacl_src),
SOC_ENUM("AIF2DACR Source", aif2dacr_src),

SOC_DOUBLE_R_TLV("AIF1DAC1 Volume", WM8994_AIF1_DAC1_LEFT_VOLUME,
		 WM8994_AIF1_DAC1_RIGHT_VOLUME, 1, 96, 0, digital_tlv),
SOC_DOUBLE_R_TLV("AIF1DAC2 Volume", WM8994_AIF1_DAC2_LEFT_VOLUME,
		 WM8994_AIF1_DAC2_RIGHT_VOLUME, 1, 96, 0, digital_tlv),
SOC_DOUBLE_R_TLV("AIF2DAC Volume", WM8994_AIF2_DAC_LEFT_VOLUME,
		 WM8994_AIF2_DAC_RIGHT_VOLUME, 1, 96, 0, digital_tlv),

SOC_SINGLE_TLV("AIF1 Boost Volume", WM8994_AIF1_CONTROL_2, 10, 3, 0, aif_tlv),
SOC_SINGLE_TLV("AIF2 Boost Volume", WM8994_AIF2_CONTROL_2, 10, 3, 0, aif_tlv),

SOC_SINGLE("AIF1DAC1 EQ Switch", WM8994_AIF1_DAC1_EQ_GAINS_1, 0, 1, 0),
SOC_SINGLE("AIF1DAC2 EQ Switch", WM8994_AIF1_DAC2_EQ_GAINS_1, 0, 1, 0),
SOC_SINGLE("AIF2 EQ Switch", WM8994_AIF2_EQ_GAINS_1, 0, 1, 0),

WM8994_DRC_SWITCH("AIF1DAC1 DRC Switch", WM8994_AIF1_DRC1_1, 2),
WM8994_DRC_SWITCH("AIF1ADC1L DRC Switch", WM8994_AIF1_DRC1_1, 1),
WM8994_DRC_SWITCH("AIF1ADC1R DRC Switch", WM8994_AIF1_DRC1_1, 0),

WM8994_DRC_SWITCH("AIF1DAC2 DRC Switch", WM8994_AIF1_DRC2_1, 2),
WM8994_DRC_SWITCH("AIF1ADC2L DRC Switch", WM8994_AIF1_DRC2_1, 1),
WM8994_DRC_SWITCH("AIF1ADC2R DRC Switch", WM8994_AIF1_DRC2_1, 0),

WM8994_DRC_SWITCH("AIF2DAC DRC Switch", WM8994_AIF2_DRC_1, 2),
WM8994_DRC_SWITCH("AIF2ADCL DRC Switch", WM8994_AIF2_DRC_1, 1),
WM8994_DRC_SWITCH("AIF2ADCR DRC Switch", WM8994_AIF2_DRC_1, 0),

SOC_SINGLE_TLV("DAC1 Right Sidetone Volume", WM8994_DAC1_MIXER_VOLUMES,
	       5, 12, 0, st_tlv),
SOC_SINGLE_TLV("DAC1 Left Sidetone Volume", WM8994_DAC1_MIXER_VOLUMES,
	       0, 12, 0, st_tlv),
SOC_SINGLE_TLV("DAC2 Right Sidetone Volume", WM8994_DAC2_MIXER_VOLUMES,
	       5, 12, 0, st_tlv),
SOC_SINGLE_TLV("DAC2 Left Sidetone Volume", WM8994_DAC2_MIXER_VOLUMES,
	       0, 12, 0, st_tlv),
SOC_ENUM("Sidetone HPF Mux", sidetone_hpf),
SOC_SINGLE("Sidetone HPF Switch", WM8994_SIDETONE, 6, 1, 0),

SOC_ENUM("AIF1ADC1 HPF Mode", aif1adc1_hpf),
SOC_DOUBLE("AIF1ADC1 HPF Switch", WM8994_AIF1_ADC1_FILTERS, 12, 11, 1, 0),

SOC_ENUM("AIF1ADC2 HPF Mode", aif1adc2_hpf),
SOC_DOUBLE("AIF1ADC2 HPF Switch", WM8994_AIF1_ADC2_FILTERS, 12, 11, 1, 0),

SOC_ENUM("AIF2ADC HPF Mode", aif2adc_hpf),
SOC_DOUBLE("AIF2ADC HPF Switch", WM8994_AIF2_ADC_FILTERS, 12, 11, 1, 0),

SOC_ENUM("ADC OSR", adc_osr),
SOC_ENUM("DAC OSR", dac_osr),

SOC_DOUBLE_R_TLV("DAC1 Volume", WM8994_DAC1_LEFT_VOLUME,
		 WM8994_DAC1_RIGHT_VOLUME, 1, 96, 0, digital_tlv),
SOC_DOUBLE_R("DAC1 Switch", WM8994_DAC1_LEFT_VOLUME,
	     WM8994_DAC1_RIGHT_VOLUME, 9, 1, 1),

SOC_DOUBLE_R_TLV("DAC2 Volume", WM8994_DAC2_LEFT_VOLUME,
		 WM8994_DAC2_RIGHT_VOLUME, 1, 96, 0, digital_tlv),
SOC_DOUBLE_R("DAC2 Switch", WM8994_DAC2_LEFT_VOLUME,
	     WM8994_DAC2_RIGHT_VOLUME, 9, 1, 1),

SOC_SINGLE_TLV("SPKL DAC2 Volume", WM8994_SPKMIXL_ATTENUATION,
	       6, 1, 1, wm_hubs_spkmix_tlv),
SOC_SINGLE_TLV("SPKL DAC1 Volume", WM8994_SPKMIXL_ATTENUATION,
	       2, 1, 1, wm_hubs_spkmix_tlv),

SOC_SINGLE_TLV("SPKR DAC2 Volume", WM8994_SPKMIXR_ATTENUATION,
	       6, 1, 1, wm_hubs_spkmix_tlv),
SOC_SINGLE_TLV("SPKR DAC1 Volume", WM8994_SPKMIXR_ATTENUATION,
	       2, 1, 1, wm_hubs_spkmix_tlv),

SOC_SINGLE_TLV("AIF1DAC1 3D Stereo Volume", WM8994_AIF1_DAC1_FILTERS_2,
	       10, 15, 0, wm8994_3d_tlv),
SOC_SINGLE("AIF1DAC1 3D Stereo Switch", WM8994_AIF1_DAC1_FILTERS_2,
	   8, 1, 0),
SOC_SINGLE_TLV("AIF1DAC2 3D Stereo Volume", WM8994_AIF1_DAC2_FILTERS_2,
	       10, 15, 0, wm8994_3d_tlv),
SOC_SINGLE("AIF1DAC2 3D Stereo Switch", WM8994_AIF1_DAC2_FILTERS_2,
	   8, 1, 0),
SOC_SINGLE_TLV("AIF2DAC 3D Stereo Volume", WM8994_AIF2_DAC_FILTERS_2,
	       10, 15, 0, wm8994_3d_tlv),
SOC_SINGLE("AIF2DAC 3D Stereo Switch", WM8994_AIF2_DAC_FILTERS_2,
	   8, 1, 0),
};

static const struct snd_kcontrol_new wm8994_eq_controls[] = {
SOC_SINGLE_TLV("AIF1DAC1 EQ1 Volume", WM8994_AIF1_DAC1_EQ_GAINS_1, 11, 31, 0,
	       eq_tlv),
SOC_SINGLE_TLV("AIF1DAC1 EQ2 Volume", WM8994_AIF1_DAC1_EQ_GAINS_1, 6, 31, 0,
	       eq_tlv),
SOC_SINGLE_TLV("AIF1DAC1 EQ3 Volume", WM8994_AIF1_DAC1_EQ_GAINS_1, 1, 31, 0,
	       eq_tlv),
SOC_SINGLE_TLV("AIF1DAC1 EQ4 Volume", WM8994_AIF1_DAC1_EQ_GAINS_2, 11, 31, 0,
	       eq_tlv),
SOC_SINGLE_TLV("AIF1DAC1 EQ5 Volume", WM8994_AIF1_DAC1_EQ_GAINS_2, 6, 31, 0,
	       eq_tlv),

SOC_SINGLE_TLV("AIF1DAC2 EQ1 Volume", WM8994_AIF1_DAC2_EQ_GAINS_1, 11, 31, 0,
	       eq_tlv),
SOC_SINGLE_TLV("AIF1DAC2 EQ2 Volume", WM8994_AIF1_DAC2_EQ_GAINS_1, 6, 31, 0,
	       eq_tlv),
SOC_SINGLE_TLV("AIF1DAC2 EQ3 Volume", WM8994_AIF1_DAC2_EQ_GAINS_1, 1, 31, 0,
	       eq_tlv),
SOC_SINGLE_TLV("AIF1DAC2 EQ4 Volume", WM8994_AIF1_DAC2_EQ_GAINS_2, 11, 31, 0,
	       eq_tlv),
SOC_SINGLE_TLV("AIF1DAC2 EQ5 Volume", WM8994_AIF1_DAC2_EQ_GAINS_2, 6, 31, 0,
	       eq_tlv),

SOC_SINGLE_TLV("AIF2 EQ1 Volume", WM8994_AIF2_EQ_GAINS_1, 11, 31, 0,
	       eq_tlv),
SOC_SINGLE_TLV("AIF2 EQ2 Volume", WM8994_AIF2_EQ_GAINS_1, 6, 31, 0,
	       eq_tlv),
SOC_SINGLE_TLV("AIF2 EQ3 Volume", WM8994_AIF2_EQ_GAINS_1, 1, 31, 0,
	       eq_tlv),
SOC_SINGLE_TLV("AIF2 EQ4 Volume", WM8994_AIF2_EQ_GAINS_2, 11, 31, 0,
	       eq_tlv),
SOC_SINGLE_TLV("AIF2 EQ5 Volume", WM8994_AIF2_EQ_GAINS_2, 6, 31, 0,
	       eq_tlv),
};

static const char *wm8958_ng_text[] = {
	"30ms", "125ms", "250ms", "500ms",
};

static const struct soc_enum wm8958_aif1dac1_ng_hold =
	SOC_ENUM_SINGLE(WM8958_AIF1_DAC1_NOISE_GATE,
			WM8958_AIF1DAC1_NG_THR_SHIFT, 4, wm8958_ng_text);

static const struct soc_enum wm8958_aif1dac2_ng_hold =
	SOC_ENUM_SINGLE(WM8958_AIF1_DAC2_NOISE_GATE,
			WM8958_AIF1DAC2_NG_THR_SHIFT, 4, wm8958_ng_text);

static const struct soc_enum wm8958_aif2dac_ng_hold =
	SOC_ENUM_SINGLE(WM8958_AIF2_DAC_NOISE_GATE,
			WM8958_AIF2DAC_NG_THR_SHIFT, 4, wm8958_ng_text);

static const struct snd_kcontrol_new wm8958_snd_controls[] = {
SOC_SINGLE_TLV("AIF3 Boost Volume", WM8958_AIF3_CONTROL_2, 10, 3, 0, aif_tlv),

SOC_SINGLE("AIF1DAC1 Noise Gate Switch", WM8958_AIF1_DAC1_NOISE_GATE,
	   WM8958_AIF1DAC1_NG_ENA_SHIFT, 1, 0),
SOC_ENUM("AIF1DAC1 Noise Gate Hold Time", wm8958_aif1dac1_ng_hold),
SOC_SINGLE_TLV("AIF1DAC1 Noise Gate Threshold Volume",
	       WM8958_AIF1_DAC1_NOISE_GATE, WM8958_AIF1DAC1_NG_THR_SHIFT,
	       7, 1, ng_tlv),

SOC_SINGLE("AIF1DAC2 Noise Gate Switch", WM8958_AIF1_DAC2_NOISE_GATE,
	   WM8958_AIF1DAC2_NG_ENA_SHIFT, 1, 0),
SOC_ENUM("AIF1DAC2 Noise Gate Hold Time", wm8958_aif1dac2_ng_hold),
SOC_SINGLE_TLV("AIF1DAC2 Noise Gate Threshold Volume",
	       WM8958_AIF1_DAC2_NOISE_GATE, WM8958_AIF1DAC2_NG_THR_SHIFT,
	       7, 1, ng_tlv),

SOC_SINGLE("AIF2DAC Noise Gate Switch", WM8958_AIF2_DAC_NOISE_GATE,
	   WM8958_AIF2DAC_NG_ENA_SHIFT, 1, 0),
SOC_ENUM("AIF2DAC Noise Gate Hold Time", wm8958_aif2dac_ng_hold),
SOC_SINGLE_TLV("AIF2DAC Noise Gate Threshold Volume",
	       WM8958_AIF2_DAC_NOISE_GATE, WM8958_AIF2DAC_NG_THR_SHIFT,
	       7, 1, ng_tlv),
#if defined(CONFIG_MACH_C1_KOR_LGT) || defined(CONFIG_MACH_BAFFIN_KOR_LGT)
SOC_ENUM_EXT("FM Control", fm_control_enum,
		wm8994_get_fm_control, wm8994_put_fm_control),
#endif
SOC_SINGLE("AIF2ADCL DAT Invert", WM8994_AIF2ADC_DATA, 1, 1, 0),
SOC_SINGLE("AIF2ADCR DAT Invert", WM8994_AIF2ADC_DATA, 0, 1, 0),
};

static const struct snd_kcontrol_new wm1811_snd_controls[] = {
SOC_SINGLE_TLV("MIXINL IN1LP Boost Volume", WM8994_INPUT_MIXER_1, 7, 1, 0,
	       mixin_boost_tlv),
SOC_SINGLE_TLV("MIXINL IN1RP Boost Volume", WM8994_INPUT_MIXER_1, 8, 1, 0,
	       mixin_boost_tlv),
};

/* We run all mode setting through a function to enforce audio mode */
static void wm1811_jackdet_set_mode(struct snd_soc_codec *codec, u16 mode)
{
	struct wm8994_priv *wm8994 = snd_soc_codec_get_drvdata(codec);

	if (!wm8994->jackdet || !wm8994->micdet[0].jack)
		return;

	if (wm8994->active_refcount)
		mode = WM1811_JACKDET_MODE_AUDIO;

	if (mode == wm8994->jackdet_mode)
		return;

	wm8994->jackdet_mode = mode;

	/* Always use audio mode to detect while the system is active */
	if (mode != WM1811_JACKDET_MODE_NONE)
		mode = WM1811_JACKDET_MODE_AUDIO;

	snd_soc_update_bits(codec, WM8994_ANTIPOP_2,
			    WM1811_JACKDET_MODE_MASK, mode);
}

static void active_reference(struct snd_soc_codec *codec)
{
	struct wm8994_priv *wm8994 = snd_soc_codec_get_drvdata(codec);

	mutex_lock(&wm8994->accdet_lock);

	wm8994->active_refcount++;

	dev_dbg(codec->dev, "Active refcount incremented, now %d\n",
		wm8994->active_refcount);

	/* If we're using jack detection go into audio mode */
	wm1811_jackdet_set_mode(codec, WM1811_JACKDET_MODE_AUDIO);

	mutex_unlock(&wm8994->accdet_lock);
}

static void active_dereference(struct snd_soc_codec *codec)
{
	struct wm8994_priv *wm8994 = snd_soc_codec_get_drvdata(codec);
	u16 mode;

	mutex_lock(&wm8994->accdet_lock);

	wm8994->active_refcount--;

	dev_dbg(codec->dev, "Active refcount decremented, now %d\n",
		wm8994->active_refcount);

	if (wm8994->active_refcount == 0) {
		/* Go into appropriate detection only mode */
		if (wm8994->jack_mic || wm8994->mic_detecting)
			mode = WM1811_JACKDET_MODE_MIC;
		else
			mode = WM1811_JACKDET_MODE_JACK;

		wm1811_jackdet_set_mode(codec, mode);
	}

	mutex_unlock(&wm8994->accdet_lock);
}

static int clk_sys_event(struct snd_soc_dapm_widget *w,
			 struct snd_kcontrol *kcontrol, int event)
{
	struct snd_soc_codec *codec = w->codec;

	switch (event) {
	case SND_SOC_DAPM_PRE_PMU:
		return configure_clock(codec);

	case SND_SOC_DAPM_POST_PMD:
		configure_clock(codec);
		break;
	}

	return 0;
}

static void vmid_reference(struct snd_soc_codec *codec)
{
	struct wm8994_priv *wm8994 = snd_soc_codec_get_drvdata(codec);

	pm_runtime_get_sync(codec->dev);

	wm8994->vmid_refcount++;

	dev_dbg(codec->dev, "Referencing VMID, refcount is now %d\n",
		wm8994->vmid_refcount);

	if (wm8994->vmid_refcount == 1) {
		snd_soc_update_bits(codec, WM8994_ANTIPOP_1,
				    WM8994_LINEOUT1_DISCH |
				    WM8994_LINEOUT2_DISCH, 0);

		wm_hubs_vmid_ena(codec);

		switch (wm8994->vmid_mode) {
		default:
			WARN_ON(0 == "Invalid VMID mode");
		case WM8994_VMID_NORMAL:
			/* Startup bias, VMID ramp & buffer */
			snd_soc_update_bits(codec, WM8994_ANTIPOP_2,
					    WM8994_BIAS_SRC |
					    WM8994_VMID_DISCH |
					    WM8994_STARTUP_BIAS_ENA |
					    WM8994_VMID_BUF_ENA |
					    WM8994_VMID_RAMP_MASK,
					    WM8994_BIAS_SRC |
					    WM8994_STARTUP_BIAS_ENA |
					    WM8994_VMID_BUF_ENA |
					    (0x3 << WM8994_VMID_RAMP_SHIFT));

			/* Main bias enable, VMID=2x40k */
			snd_soc_update_bits(codec, WM8994_POWER_MANAGEMENT_1,
					    WM8994_BIAS_ENA |
					    WM8994_VMID_SEL_MASK,
					    WM8994_BIAS_ENA | 0x2);

			msleep(50);

			snd_soc_update_bits(codec, WM8994_ANTIPOP_2,
					    WM8994_VMID_RAMP_MASK |
					    WM8994_BIAS_SRC,
					    0);
			break;

		case WM8994_VMID_FORCE:
			/* Startup bias, slow VMID ramp & buffer */
			snd_soc_update_bits(codec, WM8994_ANTIPOP_2,
					    WM8994_BIAS_SRC |
					    WM8994_VMID_DISCH |
					    WM8994_STARTUP_BIAS_ENA |
					    WM8994_VMID_BUF_ENA |
					    WM8994_VMID_RAMP_MASK,
					    WM8994_BIAS_SRC |
					    WM8994_STARTUP_BIAS_ENA |
					    WM8994_VMID_BUF_ENA |
					    (0x2 << WM8994_VMID_RAMP_SHIFT));

			/* Main bias enable, VMID=2x40k */
			snd_soc_update_bits(codec, WM8994_POWER_MANAGEMENT_1,
					    WM8994_BIAS_ENA |
					    WM8994_VMID_SEL_MASK,
					    WM8994_BIAS_ENA | 0x2);

			msleep(400);

			snd_soc_update_bits(codec, WM8994_ANTIPOP_2,
					    WM8994_VMID_RAMP_MASK |
					    WM8994_BIAS_SRC,
					    0);
			break;
		}
	}
}

static void vmid_dereference(struct snd_soc_codec *codec)
{
	struct wm8994_priv *wm8994 = snd_soc_codec_get_drvdata(codec);

	wm8994->vmid_refcount--;

	dev_dbg(codec->dev, "Dereferencing VMID, refcount is now %d\n",
		wm8994->vmid_refcount);

	if (wm8994->vmid_refcount == 0) {
		if (wm8994->hubs.lineout1_se)
			snd_soc_update_bits(codec, WM8994_POWER_MANAGEMENT_3,
					    WM8994_LINEOUT1N_ENA |
					    WM8994_LINEOUT1P_ENA,
					    WM8994_LINEOUT1N_ENA |
					    WM8994_LINEOUT1P_ENA);

		if (wm8994->hubs.lineout2_se)
			snd_soc_update_bits(codec, WM8994_POWER_MANAGEMENT_3,
					    WM8994_LINEOUT2N_ENA |
					    WM8994_LINEOUT2P_ENA,
					    WM8994_LINEOUT2N_ENA |
					    WM8994_LINEOUT2P_ENA);

		/* Start discharging VMID */
		snd_soc_update_bits(codec, WM8994_ANTIPOP_2,
				    WM8994_BIAS_SRC |
				    WM8994_VMID_DISCH,
				    WM8994_BIAS_SRC |
				    WM8994_VMID_DISCH);

		switch (wm8994->vmid_mode) {
		case WM8994_VMID_FORCE:
			msleep(350);
			break;
		default:
			break;
		}

		snd_soc_update_bits(codec, WM8994_ADDITIONAL_CONTROL,
				    WM8994_VROI, WM8994_VROI);

		/* Active discharge */
		snd_soc_update_bits(codec, WM8994_ANTIPOP_1,
				    WM8994_LINEOUT1_DISCH |
				    WM8994_LINEOUT2_DISCH,
				    WM8994_LINEOUT1_DISCH |
				    WM8994_LINEOUT2_DISCH);

		msleep(150);

		snd_soc_update_bits(codec, WM8994_POWER_MANAGEMENT_3,
				    WM8994_LINEOUT1N_ENA |
				    WM8994_LINEOUT1P_ENA |
				    WM8994_LINEOUT2N_ENA |
				    WM8994_LINEOUT2P_ENA, 0);

		snd_soc_update_bits(codec, WM8994_ADDITIONAL_CONTROL,
				    WM8994_VROI, 0);

		/* Switch off startup biases */
		snd_soc_update_bits(codec, WM8994_ANTIPOP_2,
				    WM8994_BIAS_SRC |
				    WM8994_STARTUP_BIAS_ENA |
				    WM8994_VMID_BUF_ENA |
				    WM8994_VMID_RAMP_MASK, 0);

		snd_soc_update_bits(codec, WM8994_POWER_MANAGEMENT_1,
				    WM8994_BIAS_ENA | WM8994_VMID_SEL_MASK, 0);

		snd_soc_update_bits(codec, WM8994_ANTIPOP_2,
				    WM8994_VMID_RAMP_MASK, 0);
	}

	pm_runtime_put(codec->dev);
}

static int vmid_event(struct snd_soc_dapm_widget *w,
		      struct snd_kcontrol *kcontrol, int event)
{
	struct snd_soc_codec *codec = w->codec;

	switch (event) {
	case SND_SOC_DAPM_PRE_PMU:
		vmid_reference(codec);
		break;

	case SND_SOC_DAPM_POST_PMD:
		vmid_dereference(codec);
		break;
	}

	return 0;
}

static bool wm8994_check_class_w_digital(struct snd_soc_codec *codec)
{
	int source = 0;  /* GCC flow analysis can't track enable */
	int reg, reg_r;

	/* We also need the same AIF source for L/R and only one path */
	reg = snd_soc_read(codec, WM8994_DAC1_LEFT_MIXER_ROUTING);
	switch (reg) {
	case WM8994_AIF2DACL_TO_DAC1L:
		dev_vdbg(codec->dev, "Class W source AIF2DAC\n");
		source = 2 << WM8994_CP_DYN_SRC_SEL_SHIFT;
		break;
	case WM8994_AIF1DAC2L_TO_DAC1L:
		dev_vdbg(codec->dev, "Class W source AIF1DAC2\n");
		source = 1 << WM8994_CP_DYN_SRC_SEL_SHIFT;
		break;
	case WM8994_AIF1DAC1L_TO_DAC1L:
		dev_vdbg(codec->dev, "Class W source AIF1DAC1\n");
		source = 0 << WM8994_CP_DYN_SRC_SEL_SHIFT;
		break;
	default:
		dev_vdbg(codec->dev, "DAC mixer setting: %x\n", reg);
		return false;
	}

	reg_r = snd_soc_read(codec, WM8994_DAC1_RIGHT_MIXER_ROUTING);
	if (reg_r != reg) {
		dev_vdbg(codec->dev, "Left and right DAC mixers different\n");
		return false;
	}

	/* Set the source up */
	snd_soc_update_bits(codec, WM8994_CLASS_W_1,
			    WM8994_CP_DYN_SRC_SEL_MASK, source);

	return true;
}

static int aif1clk_ev(struct snd_soc_dapm_widget *w,
		      struct snd_kcontrol *kcontrol, int event)
{
	struct snd_soc_codec *codec = w->codec;
	struct wm8994 *control = codec->control_data;
	int mask = WM8994_AIF1DAC1L_ENA | WM8994_AIF1DAC1R_ENA;
	int dac;
	int adc;
	int val;

	switch (control->type) {
	case WM8994:
	case WM8958:
		mask |= WM8994_AIF1DAC2L_ENA | WM8994_AIF1DAC2R_ENA;
		break;
	default:
		break;
	}

	switch (event) {
	case SND_SOC_DAPM_PRE_PMU:
		val = snd_soc_read(codec, WM8994_AIF1_CONTROL_1);
		if ((val & WM8994_AIF1ADCL_SRC) &&
		    (val & WM8994_AIF1ADCR_SRC))
			adc = WM8994_AIF1ADC1R_ENA | WM8994_AIF1ADC2R_ENA;
		else if (!(val & WM8994_AIF1ADCL_SRC) &&
			 !(val & WM8994_AIF1ADCR_SRC))
			adc = WM8994_AIF1ADC1L_ENA | WM8994_AIF1ADC2L_ENA;
		else
			adc = WM8994_AIF1ADC1R_ENA | WM8994_AIF1ADC2R_ENA |
				WM8994_AIF1ADC1L_ENA | WM8994_AIF1ADC2L_ENA;

		val = snd_soc_read(codec, WM8994_AIF1_CONTROL_2);
		if ((val & WM8994_AIF1DACL_SRC) &&
		    (val & WM8994_AIF1DACR_SRC))
			dac = WM8994_AIF1DAC1R_ENA | WM8994_AIF1DAC2R_ENA;
		else if (!(val & WM8994_AIF1DACL_SRC) &&
			 !(val & WM8994_AIF1DACR_SRC))
			dac = WM8994_AIF1DAC1L_ENA | WM8994_AIF1DAC2L_ENA;
		else
			dac = WM8994_AIF1DAC1R_ENA | WM8994_AIF1DAC2R_ENA |
				WM8994_AIF1DAC1L_ENA | WM8994_AIF1DAC2L_ENA;

		snd_soc_update_bits(codec, WM8994_POWER_MANAGEMENT_4,
				    mask, adc);
		snd_soc_update_bits(codec, WM8994_POWER_MANAGEMENT_5,
				    mask, dac);
		snd_soc_update_bits(codec, WM8994_CLOCKING_1,
				    WM8994_AIF1DSPCLK_ENA |
				    WM8994_SYSDSPCLK_ENA,
				    WM8994_AIF1DSPCLK_ENA |
				    WM8994_SYSDSPCLK_ENA);
		snd_soc_update_bits(codec, WM8994_POWER_MANAGEMENT_4, mask,
				    WM8994_AIF1ADC1R_ENA |
				    WM8994_AIF1ADC1L_ENA |
				    WM8994_AIF1ADC2R_ENA |
				    WM8994_AIF1ADC2L_ENA);
		snd_soc_update_bits(codec, WM8994_POWER_MANAGEMENT_5, mask,
				    WM8994_AIF1DAC1R_ENA |
				    WM8994_AIF1DAC1L_ENA |
				    WM8994_AIF1DAC2R_ENA |
				    WM8994_AIF1DAC2L_ENA);
		break;

	case SND_SOC_DAPM_PRE_PMD:
	case SND_SOC_DAPM_POST_PMD:
		snd_soc_update_bits(codec, WM8994_POWER_MANAGEMENT_5,
				    mask, 0);
		snd_soc_update_bits(codec, WM8994_POWER_MANAGEMENT_4,
				    mask, 0);

		val = snd_soc_read(codec, WM8994_CLOCKING_1);
		if (val & WM8994_AIF2DSPCLK_ENA)
			val = WM8994_SYSDSPCLK_ENA;
		else
			val = 0;
		snd_soc_update_bits(codec, WM8994_CLOCKING_1,
				    WM8994_SYSDSPCLK_ENA |
				    WM8994_AIF1DSPCLK_ENA, val);
		break;
	}

	return 0;
}

static int aif2clk_ev(struct snd_soc_dapm_widget *w,
		      struct snd_kcontrol *kcontrol, int event)
{
	struct snd_soc_codec *codec = w->codec;
	int dac;
	int adc;
	int val;

	switch (event) {
	case SND_SOC_DAPM_PRE_PMU:
		val = snd_soc_read(codec, WM8994_AIF2_CONTROL_1);
		if ((val & WM8994_AIF2ADCL_SRC) &&
		    (val & WM8994_AIF2ADCR_SRC))
			adc = WM8994_AIF2ADCR_ENA;
		else if (!(val & WM8994_AIF2ADCL_SRC) &&
			 !(val & WM8994_AIF2ADCR_SRC))
			adc = WM8994_AIF2ADCL_ENA;
		else
			adc = WM8994_AIF2ADCL_ENA | WM8994_AIF2ADCR_ENA;


		val = snd_soc_read(codec, WM8994_AIF2_CONTROL_2);
		if ((val & WM8994_AIF2DACL_SRC) &&
		    (val & WM8994_AIF2DACR_SRC))
			dac = WM8994_AIF2DACR_ENA;
		else if (!(val & WM8994_AIF2DACL_SRC) &&
			 !(val & WM8994_AIF2DACR_SRC))
			dac = WM8994_AIF2DACL_ENA;
		else
			dac = WM8994_AIF2DACL_ENA | WM8994_AIF2DACR_ENA;

		snd_soc_update_bits(codec, WM8994_POWER_MANAGEMENT_4,
				    WM8994_AIF2ADCL_ENA |
				    WM8994_AIF2ADCR_ENA, adc);
		snd_soc_update_bits(codec, WM8994_POWER_MANAGEMENT_5,
				    WM8994_AIF2DACL_ENA |
				    WM8994_AIF2DACR_ENA, dac);
		snd_soc_update_bits(codec, WM8994_CLOCKING_1,
				    WM8994_AIF2DSPCLK_ENA |
				    WM8994_SYSDSPCLK_ENA,
				    WM8994_AIF2DSPCLK_ENA |
				    WM8994_SYSDSPCLK_ENA);
		snd_soc_update_bits(codec, WM8994_POWER_MANAGEMENT_4,
				    WM8994_AIF2ADCL_ENA |
				    WM8994_AIF2ADCR_ENA,
				    WM8994_AIF2ADCL_ENA |
				    WM8994_AIF2ADCR_ENA);
		snd_soc_update_bits(codec, WM8994_POWER_MANAGEMENT_5,
				    WM8994_AIF2DACL_ENA |
				    WM8994_AIF2DACR_ENA,
				    WM8994_AIF2DACL_ENA |
				    WM8994_AIF2DACR_ENA);
		break;

	case SND_SOC_DAPM_PRE_PMD:
	case SND_SOC_DAPM_POST_PMD:
		snd_soc_update_bits(codec, WM8994_POWER_MANAGEMENT_5,
				    WM8994_AIF2DACL_ENA |
				    WM8994_AIF2DACR_ENA, 0);
 #if !defined(CONFIG_MACH_BAFFIN)
		snd_soc_update_bits(codec, WM8994_POWER_MANAGEMENT_4,
				    WM8994_AIF2ADCL_ENA |
				    WM8994_AIF2ADCR_ENA, 0);
 #endif

		val = snd_soc_read(codec, WM8994_CLOCKING_1);
		if (val & WM8994_AIF1DSPCLK_ENA)
			val = WM8994_SYSDSPCLK_ENA;
		else
			val = 0;
		snd_soc_update_bits(codec, WM8994_CLOCKING_1,
				    WM8994_SYSDSPCLK_ENA |
				    WM8994_AIF2DSPCLK_ENA, val);
		break;
	}

	return 0;
}

static int aif1clk_late_ev(struct snd_soc_dapm_widget *w,
			   struct snd_kcontrol *kcontrol, int event)
{
	struct snd_soc_codec *codec = w->codec;
	struct wm8994_priv *wm8994 = snd_soc_codec_get_drvdata(codec);

	switch (event) {
	case SND_SOC_DAPM_PRE_PMU:
		wm8994->aif1clk_enable = 1;
		break;
	case SND_SOC_DAPM_POST_PMD:
		wm8994->aif1clk_disable = 1;
		break;
	}

	return 0;
}

static int aif2clk_late_ev(struct snd_soc_dapm_widget *w,
			   struct snd_kcontrol *kcontrol, int event)
{
	struct snd_soc_codec *codec = w->codec;
	struct wm8994_priv *wm8994 = snd_soc_codec_get_drvdata(codec);

	switch (event) {
	case SND_SOC_DAPM_PRE_PMU:
		wm8994->aif2clk_enable = 1;
		break;
	case SND_SOC_DAPM_POST_PMD:
		wm8994->aif2clk_disable = 1;
		break;
	}

	return 0;
}

static int late_enable_ev(struct snd_soc_dapm_widget *w,
			  struct snd_kcontrol *kcontrol, int event)
{
	struct snd_soc_codec *codec = w->codec;
	struct wm8994_priv *wm8994 = snd_soc_codec_get_drvdata(codec);

	switch (event) {
	case SND_SOC_DAPM_PRE_PMU:
		if (wm8994->aif1clk_enable) {
			aif1clk_ev(w, kcontrol, event);
			snd_soc_update_bits(codec, WM8994_AIF1_CLOCKING_1,
					    WM8994_AIF1CLK_ENA_MASK,
					    WM8994_AIF1CLK_ENA);
			wm8994->aif1clk_enable = 0;
		}
		if (wm8994->aif2clk_enable) {
			aif2clk_ev(w, kcontrol, event);
			snd_soc_update_bits(codec, WM8994_AIF2_CLOCKING_1,
					    WM8994_AIF2CLK_ENA_MASK,
					    WM8994_AIF2CLK_ENA);
			wm8994->aif2clk_enable = 0;
		}
		break;
	}

	/* We may also have postponed startup of DSP, handle that. */
	wm8958_aif_ev(w, kcontrol, event);

	return 0;
}

static int late_disable_ev(struct snd_soc_dapm_widget *w,
			   struct snd_kcontrol *kcontrol, int event)
{
	struct snd_soc_codec *codec = w->codec;
	struct wm8994_priv *wm8994 = snd_soc_codec_get_drvdata(codec);

	switch (event) {
	case SND_SOC_DAPM_POST_PMD:
		if (wm8994->aif1clk_disable) {
			snd_soc_update_bits(codec, WM8994_AIF1_CLOCKING_1,
					    WM8994_AIF1CLK_ENA_MASK, 0);
			aif1clk_ev(w, kcontrol, event);
			wm8994->aif1clk_disable = 0;
		}
		if (wm8994->aif2clk_disable) {
			snd_soc_update_bits(codec, WM8994_AIF2_CLOCKING_1,
					    WM8994_AIF2CLK_ENA_MASK, 0);
			aif2clk_ev(w, kcontrol, event);
			wm8994->aif2clk_disable = 0;
		}
		break;
	}

	return 0;
}

static int adc_mux_ev(struct snd_soc_dapm_widget *w,
		      struct snd_kcontrol *kcontrol, int event)
{
	late_enable_ev(w, kcontrol, event);
	return 0;
}

static int micbias_ev(struct snd_soc_dapm_widget *w,
		      struct snd_kcontrol *kcontrol, int event)
{
	late_enable_ev(w, kcontrol, event);
	return 0;
}

static int dac_ev(struct snd_soc_dapm_widget *w,
		  struct snd_kcontrol *kcontrol, int event)
{
	struct snd_soc_codec *codec = w->codec;
	unsigned int mask = 1 << w->shift;

	snd_soc_update_bits(codec, WM8994_POWER_MANAGEMENT_5,
			    mask, mask);
	return 0;
}

static const char *adc_mux_text[] = {
	"ADC",
	"DMIC",
};

static const struct soc_enum adc_enum =
	SOC_ENUM_SINGLE(0, 0, 2, adc_mux_text);

static const struct snd_kcontrol_new adcl_mux =
	SOC_DAPM_ENUM_VIRT("ADCL Mux", adc_enum);

static const struct snd_kcontrol_new adcr_mux =
	SOC_DAPM_ENUM_VIRT("ADCR Mux", adc_enum);

static const struct snd_kcontrol_new left_speaker_mixer[] = {
SOC_DAPM_SINGLE("DAC2 Switch", WM8994_SPEAKER_MIXER, 9, 1, 0),
SOC_DAPM_SINGLE("Input Switch", WM8994_SPEAKER_MIXER, 7, 1, 0),
SOC_DAPM_SINGLE("IN1LP Switch", WM8994_SPEAKER_MIXER, 5, 1, 0),
SOC_DAPM_SINGLE("Output Switch", WM8994_SPEAKER_MIXER, 3, 1, 0),
SOC_DAPM_SINGLE("DAC1 Switch", WM8994_SPEAKER_MIXER, 1, 1, 0),
};

static const struct snd_kcontrol_new right_speaker_mixer[] = {
SOC_DAPM_SINGLE("DAC2 Switch", WM8994_SPEAKER_MIXER, 8, 1, 0),
SOC_DAPM_SINGLE("Input Switch", WM8994_SPEAKER_MIXER, 6, 1, 0),
SOC_DAPM_SINGLE("IN1RP Switch", WM8994_SPEAKER_MIXER, 4, 1, 0),
SOC_DAPM_SINGLE("Output Switch", WM8994_SPEAKER_MIXER, 2, 1, 0),
SOC_DAPM_SINGLE("DAC1 Switch", WM8994_SPEAKER_MIXER, 0, 1, 0),
};

/* Debugging; dump chip status after DAPM transitions */
static int post_ev(struct snd_soc_dapm_widget *w,
	    struct snd_kcontrol *kcontrol, int event)
{
	struct snd_soc_codec *codec = w->codec;
	dev_dbg(codec->dev, "SRC status: %x\n",
		snd_soc_read(codec,
			     WM8994_RATE_STATUS));
	return 0;
}

static const struct snd_kcontrol_new aif1adc1l_mix[] = {
SOC_DAPM_SINGLE("ADC/DMIC Switch", WM8994_AIF1_ADC1_LEFT_MIXER_ROUTING,
		1, 1, 0),
SOC_DAPM_SINGLE("AIF2 Switch", WM8994_AIF1_ADC1_LEFT_MIXER_ROUTING,
		0, 1, 0),
};

static const struct snd_kcontrol_new aif1adc1r_mix[] = {
SOC_DAPM_SINGLE("ADC/DMIC Switch", WM8994_AIF1_ADC1_RIGHT_MIXER_ROUTING,
		1, 1, 0),
SOC_DAPM_SINGLE("AIF2 Switch", WM8994_AIF1_ADC1_RIGHT_MIXER_ROUTING,
		0, 1, 0),
};

static const struct snd_kcontrol_new aif1adc2l_mix[] = {
SOC_DAPM_SINGLE("DMIC Switch", WM8994_AIF1_ADC2_LEFT_MIXER_ROUTING,
		1, 1, 0),
SOC_DAPM_SINGLE("AIF2 Switch", WM8994_AIF1_ADC2_LEFT_MIXER_ROUTING,
		0, 1, 0),
};

static const struct snd_kcontrol_new aif1adc2r_mix[] = {
SOC_DAPM_SINGLE("DMIC Switch", WM8994_AIF1_ADC2_RIGHT_MIXER_ROUTING,
		1, 1, 0),
SOC_DAPM_SINGLE("AIF2 Switch", WM8994_AIF1_ADC2_RIGHT_MIXER_ROUTING,
		0, 1, 0),
};

static const struct snd_kcontrol_new aif2dac2l_mix[] = {
SOC_DAPM_SINGLE("Right Sidetone Switch", WM8994_DAC2_LEFT_MIXER_ROUTING,
		5, 1, 0),
SOC_DAPM_SINGLE("Left Sidetone Switch", WM8994_DAC2_LEFT_MIXER_ROUTING,
		4, 1, 0),
SOC_DAPM_SINGLE("AIF2 Switch", WM8994_DAC2_LEFT_MIXER_ROUTING,
		2, 1, 0),
SOC_DAPM_SINGLE("AIF1.2 Switch", WM8994_DAC2_LEFT_MIXER_ROUTING,
		1, 1, 0),
SOC_DAPM_SINGLE("AIF1.1 Switch", WM8994_DAC2_LEFT_MIXER_ROUTING,
		0, 1, 0),
};

static const struct snd_kcontrol_new aif2dac2r_mix[] = {
SOC_DAPM_SINGLE("Right Sidetone Switch", WM8994_DAC2_RIGHT_MIXER_ROUTING,
		5, 1, 0),
SOC_DAPM_SINGLE("Left Sidetone Switch", WM8994_DAC2_RIGHT_MIXER_ROUTING,
		4, 1, 0),
SOC_DAPM_SINGLE("AIF2 Switch", WM8994_DAC2_RIGHT_MIXER_ROUTING,
		2, 1, 0),
SOC_DAPM_SINGLE("AIF1.2 Switch", WM8994_DAC2_RIGHT_MIXER_ROUTING,
		1, 1, 0),
SOC_DAPM_SINGLE("AIF1.1 Switch", WM8994_DAC2_RIGHT_MIXER_ROUTING,
		0, 1, 0),
};

#define WM8994_CLASS_W_SWITCH(xname, reg, shift, max, invert) \
{	.iface = SNDRV_CTL_ELEM_IFACE_MIXER, .name = xname, \
	.info = snd_soc_info_volsw, \
	.get = snd_soc_dapm_get_volsw, .put = wm8994_put_class_w, \
	.private_value =  SOC_SINGLE_VALUE(reg, shift, max, invert) }

static int wm8994_put_class_w(struct snd_kcontrol *kcontrol,
			      struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_dapm_widget_list *wlist = snd_kcontrol_chip(kcontrol);
	struct snd_soc_dapm_widget *w = wlist->widgets[0];
	struct snd_soc_codec *codec = w->codec;
	int ret;

	ret = snd_soc_dapm_put_volsw(kcontrol, ucontrol);

	wm_hubs_update_class_w(codec);

	return ret;
}

static const struct snd_kcontrol_new dac1l_mix[] = {
WM8994_CLASS_W_SWITCH("Right Sidetone Switch", WM8994_DAC1_LEFT_MIXER_ROUTING,
		      5, 1, 0),
WM8994_CLASS_W_SWITCH("Left Sidetone Switch", WM8994_DAC1_LEFT_MIXER_ROUTING,
		      4, 1, 0),
WM8994_CLASS_W_SWITCH("AIF2 Switch", WM8994_DAC1_LEFT_MIXER_ROUTING,
		      2, 1, 0),
WM8994_CLASS_W_SWITCH("AIF1.2 Switch", WM8994_DAC1_LEFT_MIXER_ROUTING,
		      1, 1, 0),
WM8994_CLASS_W_SWITCH("AIF1.1 Switch", WM8994_DAC1_LEFT_MIXER_ROUTING,
		      0, 1, 0),
};

static const struct snd_kcontrol_new dac1r_mix[] = {
WM8994_CLASS_W_SWITCH("Right Sidetone Switch", WM8994_DAC1_RIGHT_MIXER_ROUTING,
		      5, 1, 0),
WM8994_CLASS_W_SWITCH("Left Sidetone Switch", WM8994_DAC1_RIGHT_MIXER_ROUTING,
		      4, 1, 0),
WM8994_CLASS_W_SWITCH("AIF2 Switch", WM8994_DAC1_RIGHT_MIXER_ROUTING,
		      2, 1, 0),
WM8994_CLASS_W_SWITCH("AIF1.2 Switch", WM8994_DAC1_RIGHT_MIXER_ROUTING,
		      1, 1, 0),
WM8994_CLASS_W_SWITCH("AIF1.1 Switch", WM8994_DAC1_RIGHT_MIXER_ROUTING,
		      0, 1, 0),
};

static const char *sidetone_text[] = {
	"ADC/DMIC1", "DMIC2",
};

static const struct soc_enum sidetone1_enum =
	SOC_ENUM_SINGLE(WM8994_SIDETONE, 0, 2, sidetone_text);

static const struct snd_kcontrol_new sidetone1_mux =
	SOC_DAPM_ENUM("Left Sidetone Mux", sidetone1_enum);

static const struct soc_enum sidetone2_enum =
	SOC_ENUM_SINGLE(WM8994_SIDETONE, 1, 2, sidetone_text);

static const struct snd_kcontrol_new sidetone2_mux =
	SOC_DAPM_ENUM("Right Sidetone Mux", sidetone2_enum);

static const char *aif1dac_text[] = {
	"AIF1DACDAT", "AIF3DACDAT",
};

static const struct soc_enum aif1dac_enum =
	SOC_ENUM_SINGLE(WM8994_POWER_MANAGEMENT_6, 0, 2, aif1dac_text);

static const struct snd_kcontrol_new aif1dac_mux =
	SOC_DAPM_ENUM("AIF1DAC Mux", aif1dac_enum);

static const char *aif2dac_text[] = {
	"AIF2DACDAT", "AIF3DACDAT",
};

static const struct soc_enum aif2dac_enum =
	SOC_ENUM_SINGLE(WM8994_POWER_MANAGEMENT_6, 1, 2, aif2dac_text);

static const struct snd_kcontrol_new aif2dac_mux =
	SOC_DAPM_ENUM("AIF2DAC Mux", aif2dac_enum);

static const char *aif2adc_text[] = {
	"AIF2ADCDAT", "AIF3DACDAT",
};

static const struct soc_enum aif2adc_enum =
	SOC_ENUM_SINGLE(WM8994_POWER_MANAGEMENT_6, 2, 2, aif2adc_text);

static const struct snd_kcontrol_new aif2adc_mux =
	SOC_DAPM_ENUM("AIF2ADC Mux", aif2adc_enum);

static const char *aif3adc_text[] = {
	"AIF1ADCDAT", "AIF2ADCDAT", "AIF2DACDAT", "Mono PCM",
};

static const struct soc_enum wm8994_aif3adc_enum =
	SOC_ENUM_SINGLE(WM8994_POWER_MANAGEMENT_6, 3, 3, aif3adc_text);

static const struct snd_kcontrol_new wm8994_aif3adc_mux =
	SOC_DAPM_ENUM("AIF3ADC Mux", wm8994_aif3adc_enum);

static const struct soc_enum wm8958_aif3adc_enum =
	SOC_ENUM_SINGLE(WM8994_POWER_MANAGEMENT_6, 3, 4, aif3adc_text);

static const struct snd_kcontrol_new wm8958_aif3adc_mux =
	SOC_DAPM_ENUM("AIF3ADC Mux", wm8958_aif3adc_enum);

static const char *mono_pcm_out_text[] = {
	"None", "AIF2ADCL", "AIF2ADCR",
};

static const struct soc_enum mono_pcm_out_enum =
	SOC_ENUM_SINGLE(WM8994_POWER_MANAGEMENT_6, 9, 3, mono_pcm_out_text);

static const struct snd_kcontrol_new mono_pcm_out_mux =
	SOC_DAPM_ENUM("Mono PCM Out Mux", mono_pcm_out_enum);

static const char *aif2dac_src_text[] = {
	"AIF2", "AIF3",
};

/* Note that these two control shouldn't be simultaneously switched to AIF3 */
static const struct soc_enum aif2dacl_src_enum =
	SOC_ENUM_SINGLE(WM8994_POWER_MANAGEMENT_6, 7, 2, aif2dac_src_text);

static const struct snd_kcontrol_new aif2dacl_src_mux =
	SOC_DAPM_ENUM("AIF2DACL Mux", aif2dacl_src_enum);

static const struct soc_enum aif2dacr_src_enum =
	SOC_ENUM_SINGLE(WM8994_POWER_MANAGEMENT_6, 8, 2, aif2dac_src_text);

static const struct snd_kcontrol_new aif2dacr_src_mux =
	SOC_DAPM_ENUM("AIF2DACR Mux", aif2dacr_src_enum);

static const struct snd_soc_dapm_widget wm8994_lateclk_revd_widgets[] = {
SND_SOC_DAPM_SUPPLY("AIF1CLK", SND_SOC_NOPM, 0, 0, aif1clk_late_ev,
	SND_SOC_DAPM_PRE_PMU | SND_SOC_DAPM_POST_PMD),
SND_SOC_DAPM_SUPPLY("AIF2CLK", SND_SOC_NOPM, 0, 0, aif2clk_late_ev,
	SND_SOC_DAPM_PRE_PMU | SND_SOC_DAPM_POST_PMD),

SND_SOC_DAPM_PGA_E("Late DAC1L Enable PGA", SND_SOC_NOPM, 0, 0, NULL, 0,
	late_enable_ev, SND_SOC_DAPM_PRE_PMU),
SND_SOC_DAPM_PGA_E("Late DAC1R Enable PGA", SND_SOC_NOPM, 0, 0, NULL, 0,
	late_enable_ev, SND_SOC_DAPM_PRE_PMU),
SND_SOC_DAPM_PGA_E("Late DAC2L Enable PGA", SND_SOC_NOPM, 0, 0, NULL, 0,
	late_enable_ev, SND_SOC_DAPM_PRE_PMU),
SND_SOC_DAPM_PGA_E("Late DAC2R Enable PGA", SND_SOC_NOPM, 0, 0, NULL, 0,
	late_enable_ev, SND_SOC_DAPM_PRE_PMU),
SND_SOC_DAPM_PGA_E("Direct Voice", SND_SOC_NOPM, 0, 0, NULL, 0,
	late_enable_ev, SND_SOC_DAPM_PRE_PMU),

SND_SOC_DAPM_MIXER_E("SPKL", WM8994_POWER_MANAGEMENT_3, 8, 0,
		     left_speaker_mixer, ARRAY_SIZE(left_speaker_mixer),
		     late_enable_ev, SND_SOC_DAPM_PRE_PMU),
SND_SOC_DAPM_MIXER_E("SPKR", WM8994_POWER_MANAGEMENT_3, 9, 0,
		     right_speaker_mixer, ARRAY_SIZE(right_speaker_mixer),
		     late_enable_ev, SND_SOC_DAPM_PRE_PMU),
SND_SOC_DAPM_MUX_E("Left Headphone Mux", SND_SOC_NOPM, 0, 0, &wm_hubs_hpl_mux,
		   late_enable_ev, SND_SOC_DAPM_PRE_PMU),
SND_SOC_DAPM_MUX_E("Right Headphone Mux", SND_SOC_NOPM, 0, 0, &wm_hubs_hpr_mux,
		   late_enable_ev, SND_SOC_DAPM_PRE_PMU),

SND_SOC_DAPM_POST("Late Disable PGA", late_disable_ev)
};

static const struct snd_soc_dapm_widget wm8994_lateclk_widgets[] = {
SND_SOC_DAPM_SUPPLY("AIF1CLK", WM8994_AIF1_CLOCKING_1, 0, 0, aif1clk_ev,
		    SND_SOC_DAPM_PRE_PMU | SND_SOC_DAPM_PRE_PMD),
SND_SOC_DAPM_SUPPLY("AIF2CLK", WM8994_AIF2_CLOCKING_1, 0, 0, aif2clk_ev,
		    SND_SOC_DAPM_PRE_PMU | SND_SOC_DAPM_PRE_PMD),
SND_SOC_DAPM_PGA("Direct Voice", SND_SOC_NOPM, 0, 0, NULL, 0),
SND_SOC_DAPM_MIXER("SPKL", WM8994_POWER_MANAGEMENT_3, 8, 0,
		   left_speaker_mixer, ARRAY_SIZE(left_speaker_mixer)),
SND_SOC_DAPM_MIXER("SPKR", WM8994_POWER_MANAGEMENT_3, 9, 0,
		   right_speaker_mixer, ARRAY_SIZE(right_speaker_mixer)),
SND_SOC_DAPM_MUX("Left Headphone Mux", SND_SOC_NOPM, 0, 0, &wm_hubs_hpl_mux),
SND_SOC_DAPM_MUX("Right Headphone Mux", SND_SOC_NOPM, 0, 0, &wm_hubs_hpr_mux),
};

static const struct snd_soc_dapm_widget wm8994_dac_revd_widgets[] = {
SND_SOC_DAPM_DAC_E("DAC2L", NULL, SND_SOC_NOPM, 3, 0,
	dac_ev, SND_SOC_DAPM_PRE_PMU),
SND_SOC_DAPM_DAC_E("DAC2R", NULL, SND_SOC_NOPM, 2, 0,
	dac_ev, SND_SOC_DAPM_PRE_PMU),
SND_SOC_DAPM_DAC_E("DAC1L", NULL, SND_SOC_NOPM, 1, 0,
	dac_ev, SND_SOC_DAPM_PRE_PMU),
SND_SOC_DAPM_DAC_E("DAC1R", NULL, SND_SOC_NOPM, 0, 0,
	dac_ev, SND_SOC_DAPM_PRE_PMU),
};

static const struct snd_soc_dapm_widget wm8994_dac_widgets[] = {
SND_SOC_DAPM_DAC("DAC2L", NULL, WM8994_POWER_MANAGEMENT_5, 3, 0),
SND_SOC_DAPM_DAC("DAC2R", NULL, WM8994_POWER_MANAGEMENT_5, 2, 0),
SND_SOC_DAPM_DAC("DAC1L", NULL, WM8994_POWER_MANAGEMENT_5, 1, 0),
SND_SOC_DAPM_DAC("DAC1R", NULL, WM8994_POWER_MANAGEMENT_5, 0, 0),
};

static const struct snd_soc_dapm_widget wm8994_adc_revd_widgets[] = {
SND_SOC_DAPM_VIRT_MUX_E("ADCL Mux", WM8994_POWER_MANAGEMENT_4, 1, 0, &adcl_mux,
			adc_mux_ev, SND_SOC_DAPM_PRE_PMU),
SND_SOC_DAPM_VIRT_MUX_E("ADCR Mux", WM8994_POWER_MANAGEMENT_4, 0, 0, &adcr_mux,
			adc_mux_ev, SND_SOC_DAPM_PRE_PMU),
};

static const struct snd_soc_dapm_widget wm8994_adc_widgets[] = {
SND_SOC_DAPM_VIRT_MUX("ADCL Mux", WM8994_POWER_MANAGEMENT_4, 1, 0, &adcl_mux),
SND_SOC_DAPM_VIRT_MUX("ADCR Mux", WM8994_POWER_MANAGEMENT_4, 0, 0, &adcr_mux),
};

static const struct snd_soc_dapm_widget wm8994_dapm_widgets[] = {
SND_SOC_DAPM_INPUT("DMIC1DAT"),
SND_SOC_DAPM_INPUT("DMIC2DAT"),
SND_SOC_DAPM_INPUT("Clock"),

SND_SOC_DAPM_SUPPLY_S("MICBIAS Supply", 1, SND_SOC_NOPM, 0, 0, micbias_ev,
		      SND_SOC_DAPM_PRE_PMU),
SND_SOC_DAPM_SUPPLY("VMID", SND_SOC_NOPM, 0, 0, vmid_event,
		    SND_SOC_DAPM_PRE_PMU | SND_SOC_DAPM_POST_PMD),

SND_SOC_DAPM_SUPPLY("CLK_SYS", SND_SOC_NOPM, 0, 0, clk_sys_event,
		    SND_SOC_DAPM_POST_PMU | SND_SOC_DAPM_PRE_PMD),

SND_SOC_DAPM_SUPPLY("DSP1CLK", SND_SOC_NOPM, 3, 0, NULL, 0),
SND_SOC_DAPM_SUPPLY("DSP2CLK", SND_SOC_NOPM, 2, 0, NULL, 0),
SND_SOC_DAPM_SUPPLY("DSPINTCLK", SND_SOC_NOPM, 1, 0, NULL, 0),

SND_SOC_DAPM_AIF_OUT("AIF1ADC1L", NULL,
		     0, SND_SOC_NOPM, 9, 0),
SND_SOC_DAPM_AIF_OUT("AIF1ADC1R", NULL,
		     0, SND_SOC_NOPM, 8, 0),
SND_SOC_DAPM_AIF_IN_E("AIF1DAC1L", NULL, 0,
		      SND_SOC_NOPM, 9, 0, wm8958_aif_ev,
		      SND_SOC_DAPM_POST_PMU | SND_SOC_DAPM_POST_PMD),
SND_SOC_DAPM_AIF_IN_E("AIF1DAC1R", NULL, 0,
		      SND_SOC_NOPM, 8, 0, wm8958_aif_ev,
		      SND_SOC_DAPM_POST_PMU | SND_SOC_DAPM_POST_PMD),

SND_SOC_DAPM_AIF_OUT("AIF1ADC2L", NULL,
		     0, SND_SOC_NOPM, 11, 0),
SND_SOC_DAPM_AIF_OUT("AIF1ADC2R", NULL,
		     0, SND_SOC_NOPM, 10, 0),
SND_SOC_DAPM_AIF_IN_E("AIF1DAC2L", NULL, 0,
		      SND_SOC_NOPM, 11, 0, wm8958_aif_ev,
		      SND_SOC_DAPM_POST_PMU | SND_SOC_DAPM_POST_PMD),
SND_SOC_DAPM_AIF_IN_E("AIF1DAC2R", NULL, 0,
		      SND_SOC_NOPM, 10, 0, wm8958_aif_ev,
		      SND_SOC_DAPM_POST_PMU | SND_SOC_DAPM_POST_PMD),

SND_SOC_DAPM_MIXER("AIF1ADC1L Mixer", SND_SOC_NOPM, 0, 0,
		   aif1adc1l_mix, ARRAY_SIZE(aif1adc1l_mix)),
SND_SOC_DAPM_MIXER("AIF1ADC1R Mixer", SND_SOC_NOPM, 0, 0,
		   aif1adc1r_mix, ARRAY_SIZE(aif1adc1r_mix)),

SND_SOC_DAPM_MIXER("AIF1ADC2L Mixer", SND_SOC_NOPM, 0, 0,
		   aif1adc2l_mix, ARRAY_SIZE(aif1adc2l_mix)),
SND_SOC_DAPM_MIXER("AIF1ADC2R Mixer", SND_SOC_NOPM, 0, 0,
		   aif1adc2r_mix, ARRAY_SIZE(aif1adc2r_mix)),

SND_SOC_DAPM_MIXER("AIF2DAC2L Mixer", SND_SOC_NOPM, 0, 0,
		   aif2dac2l_mix, ARRAY_SIZE(aif2dac2l_mix)),
SND_SOC_DAPM_MIXER("AIF2DAC2R Mixer", SND_SOC_NOPM, 0, 0,
		   aif2dac2r_mix, ARRAY_SIZE(aif2dac2r_mix)),

SND_SOC_DAPM_MUX("Left Sidetone", SND_SOC_NOPM, 0, 0, &sidetone1_mux),
SND_SOC_DAPM_MUX("Right Sidetone", SND_SOC_NOPM, 0, 0, &sidetone2_mux),

SND_SOC_DAPM_MIXER("DAC1L Mixer", SND_SOC_NOPM, 0, 0,
		   dac1l_mix, ARRAY_SIZE(dac1l_mix)),
SND_SOC_DAPM_MIXER("DAC1R Mixer", SND_SOC_NOPM, 0, 0,
		   dac1r_mix, ARRAY_SIZE(dac1r_mix)),

SND_SOC_DAPM_AIF_OUT("AIF2ADCL", NULL, 0,
		     SND_SOC_NOPM, 13, 0),
SND_SOC_DAPM_AIF_OUT("AIF2ADCR", NULL, 0,
		     SND_SOC_NOPM, 12, 0),
SND_SOC_DAPM_AIF_IN_E("AIF2DACL", NULL, 0,
		      SND_SOC_NOPM, 13, 0, wm8958_aif_ev,
		      SND_SOC_DAPM_POST_PMU | SND_SOC_DAPM_PRE_PMD),
SND_SOC_DAPM_AIF_IN_E("AIF2DACR", NULL, 0,
		      SND_SOC_NOPM, 12, 0, wm8958_aif_ev,
		      SND_SOC_DAPM_POST_PMU | SND_SOC_DAPM_PRE_PMD),

SND_SOC_DAPM_AIF_IN("AIF1DACDAT", "AIF1 Playback", 0, SND_SOC_NOPM, 0, 0),
SND_SOC_DAPM_AIF_IN("AIF2DACDAT", "AIF2 Playback", 0, SND_SOC_NOPM, 0, 0),
SND_SOC_DAPM_AIF_OUT("AIF1ADCDAT", "AIF1 Capture", 0, SND_SOC_NOPM, 0, 0),
SND_SOC_DAPM_AIF_OUT("AIF2ADCDAT", "AIF2 Capture", 0, SND_SOC_NOPM, 0, 0),

SND_SOC_DAPM_MUX("AIF1DAC Mux", SND_SOC_NOPM, 0, 0, &aif1dac_mux),
SND_SOC_DAPM_MUX("AIF2DAC Mux", SND_SOC_NOPM, 0, 0, &aif2dac_mux),
SND_SOC_DAPM_MUX("AIF2ADC Mux", SND_SOC_NOPM, 0, 0, &aif2adc_mux),

SND_SOC_DAPM_AIF_IN("AIF3DACDAT", "AIF3 Playback", 0, SND_SOC_NOPM, 0, 0),
SND_SOC_DAPM_AIF_OUT("AIF3ADCDAT", "AIF3 Capture", 0, SND_SOC_NOPM, 0, 0),

SND_SOC_DAPM_SUPPLY("TOCLK", WM8994_CLOCKING_1, 4, 0, NULL, 0),

SND_SOC_DAPM_ADC("DMIC2L", NULL, WM8994_POWER_MANAGEMENT_4, 5, 0),
SND_SOC_DAPM_ADC("DMIC2R", NULL, WM8994_POWER_MANAGEMENT_4, 4, 0),
SND_SOC_DAPM_ADC("DMIC1L", NULL, WM8994_POWER_MANAGEMENT_4, 3, 0),
SND_SOC_DAPM_ADC("DMIC1R", NULL, WM8994_POWER_MANAGEMENT_4, 2, 0),

/* Power is done with the muxes since the ADC power also controls the
 * downsampling chain, the chip will automatically manage the analogue
 * specific portions.
 */
SND_SOC_DAPM_ADC("ADCL", NULL, SND_SOC_NOPM, 1, 0),
SND_SOC_DAPM_ADC("ADCR", NULL, SND_SOC_NOPM, 0, 0),

SND_SOC_DAPM_POST("Debug log", post_ev),
};

static const struct snd_soc_dapm_widget wm8994_specific_dapm_widgets[] = {
SND_SOC_DAPM_MUX("AIF3ADC Mux", SND_SOC_NOPM, 0, 0, &wm8994_aif3adc_mux),
};

static const struct snd_soc_dapm_widget wm8958_dapm_widgets[] = {
SND_SOC_DAPM_MUX("Mono PCM Out Mux", SND_SOC_NOPM, 0, 0, &mono_pcm_out_mux),
SND_SOC_DAPM_MUX("AIF2DACL Mux", SND_SOC_NOPM, 0, 0, &aif2dacl_src_mux),
SND_SOC_DAPM_MUX("AIF2DACR Mux", SND_SOC_NOPM, 0, 0, &aif2dacr_src_mux),
SND_SOC_DAPM_MUX("AIF3ADC Mux", SND_SOC_NOPM, 0, 0, &wm8958_aif3adc_mux),
};

static const struct snd_soc_dapm_route intercon[] = {
	{ "CLK_SYS", NULL, "AIF1CLK", check_clk_sys },
	{ "CLK_SYS", NULL, "AIF2CLK", check_clk_sys },

	{ "DSP1CLK", NULL, "CLK_SYS" },
	{ "DSP2CLK", NULL, "CLK_SYS" },
	{ "DSPINTCLK", NULL, "CLK_SYS" },

	{ "AIF1ADC1L", NULL, "AIF1CLK" },
	{ "AIF1ADC1L", NULL, "DSP1CLK" },
	{ "AIF1ADC1R", NULL, "AIF1CLK" },
	{ "AIF1ADC1R", NULL, "DSP1CLK" },
	{ "AIF1ADC1R", NULL, "DSPINTCLK" },

	{ "AIF1DAC1L", NULL, "AIF1CLK" },
	{ "AIF1DAC1L", NULL, "DSP1CLK" },
	{ "AIF1DAC1R", NULL, "AIF1CLK" },
	{ "AIF1DAC1R", NULL, "DSP1CLK" },
	{ "AIF1DAC1R", NULL, "DSPINTCLK" },

	{ "AIF1ADC2L", NULL, "AIF1CLK" },
	{ "AIF1ADC2L", NULL, "DSP1CLK" },
	{ "AIF1ADC2R", NULL, "AIF1CLK" },
	{ "AIF1ADC2R", NULL, "DSP1CLK" },
	{ "AIF1ADC2R", NULL, "DSPINTCLK" },

	{ "AIF1DAC2L", NULL, "AIF1CLK" },
	{ "AIF1DAC2L", NULL, "DSP1CLK" },
	{ "AIF1DAC2R", NULL, "AIF1CLK" },
	{ "AIF1DAC2R", NULL, "DSP1CLK" },
	{ "AIF1DAC2R", NULL, "DSPINTCLK" },

	{ "AIF2ADCL", NULL, "AIF2CLK" },
	{ "AIF2ADCL", NULL, "DSP2CLK" },
	{ "AIF2ADCR", NULL, "AIF2CLK" },
	{ "AIF2ADCR", NULL, "DSP2CLK" },
	{ "AIF2ADCR", NULL, "DSPINTCLK" },

	{ "AIF2DACL", NULL, "AIF2CLK" },
	{ "AIF2DACL", NULL, "DSP2CLK" },
	{ "AIF2DACR", NULL, "AIF2CLK" },
	{ "AIF2DACR", NULL, "DSP2CLK" },
	{ "AIF2DACR", NULL, "DSPINTCLK" },

	{ "DMIC1L", NULL, "DMIC1DAT" },
	{ "DMIC1L", NULL, "CLK_SYS" },
	{ "DMIC1R", NULL, "DMIC1DAT" },
	{ "DMIC1R", NULL, "CLK_SYS" },
	{ "DMIC2L", NULL, "DMIC2DAT" },
	{ "DMIC2L", NULL, "CLK_SYS" },
	{ "DMIC2R", NULL, "DMIC2DAT" },
	{ "DMIC2R", NULL, "CLK_SYS" },

	{ "ADCL", NULL, "AIF1CLK" },
	{ "ADCL", NULL, "DSP1CLK" },
	{ "ADCL", NULL, "DSPINTCLK" },

	{ "ADCR", NULL, "AIF1CLK" },
	{ "ADCR", NULL, "DSP1CLK" },
	{ "ADCR", NULL, "DSPINTCLK" },

	{ "ADCL Mux", "ADC", "ADCL" },
	{ "ADCL Mux", "DMIC", "DMIC1L" },
	{ "ADCR Mux", "ADC", "ADCR" },
	{ "ADCR Mux", "DMIC", "DMIC1R" },

	{ "DAC1L", NULL, "AIF1CLK" },
	{ "DAC1L", NULL, "DSP1CLK" },
	{ "DAC1L", NULL, "DSPINTCLK" },

	{ "DAC1R", NULL, "AIF1CLK" },
	{ "DAC1R", NULL, "DSP1CLK" },
	{ "DAC1R", NULL, "DSPINTCLK" },

	{ "DAC2L", NULL, "AIF2CLK" },
	{ "DAC2L", NULL, "DSP2CLK" },
	{ "DAC2L", NULL, "DSPINTCLK" },

	{ "DAC2R", NULL, "AIF2DACR" },
	{ "DAC2R", NULL, "AIF2CLK" },
	{ "DAC2R", NULL, "DSP2CLK" },
	{ "DAC2R", NULL, "DSPINTCLK" },

	{ "TOCLK", NULL, "CLK_SYS" },

	/* AIF1 outputs */
	{ "AIF1ADC1L", NULL, "AIF1ADC1L Mixer" },
	{ "AIF1ADC1L Mixer", "ADC/DMIC Switch", "ADCL Mux" },
	{ "AIF1ADC1L Mixer", "AIF2 Switch", "AIF2DACL" },

	{ "AIF1ADC1R", NULL, "AIF1ADC1R Mixer" },
	{ "AIF1ADC1R Mixer", "ADC/DMIC Switch", "ADCR Mux" },
	{ "AIF1ADC1R Mixer", "AIF2 Switch", "AIF2DACR" },

	{ "AIF1ADC2L", NULL, "AIF1ADC2L Mixer" },
	{ "AIF1ADC2L Mixer", "DMIC Switch", "DMIC2L" },
	{ "AIF1ADC2L Mixer", "AIF2 Switch", "AIF2DACL" },

	{ "AIF1ADC2R", NULL, "AIF1ADC2R Mixer" },
	{ "AIF1ADC2R Mixer", "DMIC Switch", "DMIC2R" },
	{ "AIF1ADC2R Mixer", "AIF2 Switch", "AIF2DACR" },

	/* Pin level routing for AIF3 */
	{ "AIF1DAC1L", NULL, "AIF1DAC Mux" },
	{ "AIF1DAC1R", NULL, "AIF1DAC Mux" },
	{ "AIF1DAC2L", NULL, "AIF1DAC Mux" },
	{ "AIF1DAC2R", NULL, "AIF1DAC Mux" },

	{ "AIF1DAC Mux", "AIF1DACDAT", "AIF1DACDAT" },
	{ "AIF1DAC Mux", "AIF3DACDAT", "AIF3DACDAT" },
	{ "AIF2DAC Mux", "AIF2DACDAT", "AIF2DACDAT" },
	{ "AIF2DAC Mux", "AIF3DACDAT", "AIF3DACDAT" },
	{ "AIF2ADC Mux", "AIF2ADCDAT", "AIF2ADCL" },
	{ "AIF2ADC Mux", "AIF2ADCDAT", "AIF2ADCR" },
	{ "AIF2ADC Mux", "AIF3DACDAT", "AIF3ADCDAT" },

	/* DAC1 inputs */
	{ "DAC1L Mixer", "AIF2 Switch", "AIF2DACL" },
	{ "DAC1L Mixer", "AIF1.2 Switch", "AIF1DAC2L" },
	{ "DAC1L Mixer", "AIF1.1 Switch", "AIF1DAC1L" },
	{ "DAC1L Mixer", "Left Sidetone Switch", "Left Sidetone" },
	{ "DAC1L Mixer", "Right Sidetone Switch", "Right Sidetone" },

	{ "DAC1R Mixer", "AIF2 Switch", "AIF2DACR" },
	{ "DAC1R Mixer", "AIF1.2 Switch", "AIF1DAC2R" },
	{ "DAC1R Mixer", "AIF1.1 Switch", "AIF1DAC1R" },
	{ "DAC1R Mixer", "Left Sidetone Switch", "Left Sidetone" },
	{ "DAC1R Mixer", "Right Sidetone Switch", "Right Sidetone" },

	/* DAC2/AIF2 outputs  */
	{ "AIF2ADCL", NULL, "AIF2DAC2L Mixer" },
	{ "AIF2DAC2L Mixer", "AIF2 Switch", "AIF2DACL" },
	{ "AIF2DAC2L Mixer", "AIF1.2 Switch", "AIF1DAC2L" },
	{ "AIF2DAC2L Mixer", "AIF1.1 Switch", "AIF1DAC1L" },
	{ "AIF2DAC2L Mixer", "Left Sidetone Switch", "Left Sidetone" },
	{ "AIF2DAC2L Mixer", "Right Sidetone Switch", "Right Sidetone" },

	{ "AIF2ADCR", NULL, "AIF2DAC2R Mixer" },
	{ "AIF2DAC2R Mixer", "AIF2 Switch", "AIF2DACR" },
	{ "AIF2DAC2R Mixer", "AIF1.2 Switch", "AIF1DAC2R" },
	{ "AIF2DAC2R Mixer", "AIF1.1 Switch", "AIF1DAC1R" },
	{ "AIF2DAC2R Mixer", "Left Sidetone Switch", "Left Sidetone" },
	{ "AIF2DAC2R Mixer", "Right Sidetone Switch", "Right Sidetone" },

	{ "AIF1ADCDAT", NULL, "AIF1ADC1L" },
	{ "AIF1ADCDAT", NULL, "AIF1ADC1R" },
	{ "AIF1ADCDAT", NULL, "AIF1ADC2L" },
	{ "AIF1ADCDAT", NULL, "AIF1ADC2R" },

	{ "AIF2ADCDAT", NULL, "AIF2ADC Mux" },

	/* AIF3 output */
	{ "AIF3ADCDAT", "AIF1ADCDAT", "AIF1ADC1L" },
	{ "AIF3ADCDAT", "AIF1ADCDAT", "AIF1ADC1R" },
	{ "AIF3ADCDAT", "AIF1ADCDAT", "AIF1ADC2L" },
	{ "AIF3ADCDAT", "AIF1ADCDAT", "AIF1ADC2R" },
	{ "AIF3ADCDAT", "AIF2ADCDAT", "AIF2ADCL" },
	{ "AIF3ADCDAT", "AIF2ADCDAT", "AIF2ADCR" },
	{ "AIF3ADCDAT", "AIF2DACDAT", "AIF2DACL" },
	{ "AIF3ADCDAT", "AIF2DACDAT", "AIF2DACR" },

	/* Sidetone */
	{ "Left Sidetone", "ADC/DMIC1", "ADCL Mux" },
	{ "Left Sidetone", "DMIC2", "DMIC2L" },
	{ "Right Sidetone", "ADC/DMIC1", "ADCR Mux" },
	{ "Right Sidetone", "DMIC2", "DMIC2R" },

	/* Output stages */
	{ "Left Output Mixer", "DAC Switch", "DAC1L" },
	{ "Right Output Mixer", "DAC Switch", "DAC1R" },

	{ "SPKL", "DAC1 Switch", "DAC1L" },
	{ "SPKL", "DAC2 Switch", "DAC2L" },

	{ "SPKR", "DAC1 Switch", "DAC1R" },
	{ "SPKR", "DAC2 Switch", "DAC2R" },

	{ "Left Headphone Mux", "DAC", "DAC1L" },
	{ "Right Headphone Mux", "DAC", "DAC1R" },
};

static const struct snd_soc_dapm_route wm8994_lateclk_revd_intercon[] = {
	{ "DAC1L", NULL, "Late DAC1L Enable PGA" },
	{ "Late DAC1L Enable PGA", NULL, "DAC1L Mixer" },
	{ "DAC1R", NULL, "Late DAC1R Enable PGA" },
	{ "Late DAC1R Enable PGA", NULL, "DAC1R Mixer" },
	{ "DAC2L", NULL, "Late DAC2L Enable PGA" },
	{ "Late DAC2L Enable PGA", NULL, "AIF2DAC2L Mixer" },
	{ "DAC2R", NULL, "Late DAC2R Enable PGA" },
	{ "Late DAC2R Enable PGA", NULL, "AIF2DAC2R Mixer" }
};

static const struct snd_soc_dapm_route wm8994_lateclk_intercon[] = {
	{ "DAC1L", NULL, "DAC1L Mixer" },
	{ "DAC1R", NULL, "DAC1R Mixer" },
	{ "DAC2L", NULL, "AIF2DAC2L Mixer" },
	{ "DAC2R", NULL, "AIF2DAC2R Mixer" },
};

static const struct snd_soc_dapm_route wm8994_revd_intercon[] = {
	{ "AIF1DACDAT", NULL, "AIF2DACDAT" },
	{ "AIF2DACDAT", NULL, "AIF1DACDAT" },
	{ "AIF1ADCDAT", NULL, "AIF2ADCDAT" },
	{ "AIF2ADCDAT", NULL, "AIF1ADCDAT" },
	{ "MICBIAS1", NULL, "CLK_SYS" },
	{ "MICBIAS1", NULL, "MICBIAS Supply" },
	{ "MICBIAS2", NULL, "CLK_SYS" },
	{ "MICBIAS2", NULL, "MICBIAS Supply" },
};

static const struct snd_soc_dapm_route wm8994_intercon[] = {
	{ "AIF2DACL", NULL, "AIF2DAC Mux" },
	{ "AIF2DACR", NULL, "AIF2DAC Mux" },
	{ "MICBIAS1", NULL, "VMID" },
	{ "MICBIAS2", NULL, "VMID" },
};

static const struct snd_soc_dapm_route wm8958_intercon[] = {
	{ "AIF2DACL", NULL, "AIF2DACL Mux" },
	{ "AIF2DACR", NULL, "AIF2DACR Mux" },

	{ "AIF2DACL Mux", "AIF2", "AIF2DAC Mux" },
	{ "AIF2DACL Mux", "AIF3", "AIF3DACDAT" },
	{ "AIF2DACR Mux", "AIF2", "AIF2DAC Mux" },
	{ "AIF2DACR Mux", "AIF3", "AIF3DACDAT" },

	{ "Mono PCM Out Mux", "AIF2ADCL", "AIF2ADCL" },
	{ "Mono PCM Out Mux", "AIF2ADCR", "AIF2ADCR" },

	{ "AIF3ADC Mux", "Mono PCM", "Mono PCM Out Mux" },
};

/* The size in bits of the FLL divide multiplied by 10
 * to allow rounding later */
#define FIXED_FLL_SIZE ((1 << 16) * 10)

struct fll_div {
	u16 outdiv;
	u16 n;
	u16 k;
	u16 clk_ref_div;
	u16 fll_fratio;
};

static int wm8994_get_fll_config(struct fll_div *fll,
				 int freq_in, int freq_out)
{
	u64 Kpart;
	unsigned int K, Ndiv, Nmod;

	pr_debug("FLL input=%dHz, output=%dHz\n", freq_in, freq_out);

	/* Scale the input frequency down to <= 13.5MHz */
	fll->clk_ref_div = 0;
	while (freq_in > 13500000) {
		fll->clk_ref_div++;
		freq_in /= 2;

		if (fll->clk_ref_div > 3)
			return -EINVAL;
	}
	pr_debug("CLK_REF_DIV=%d, Fref=%dHz\n", fll->clk_ref_div, freq_in);

	/* Scale the output to give 90MHz<=Fvco<=100MHz */
	fll->outdiv = 3;
	while (freq_out * (fll->outdiv + 1) < 90000000) {
		fll->outdiv++;
		if (fll->outdiv > 63)
			return -EINVAL;
	}
	freq_out *= fll->outdiv + 1;
	pr_debug("OUTDIV=%d, Fvco=%dHz\n", fll->outdiv, freq_out);

	if (freq_in > 1000000) {
		fll->fll_fratio = 0;
	} else if (freq_in > 256000) {
		fll->fll_fratio = 1;
		freq_in *= 2;
	} else if (freq_in > 128000) {
		fll->fll_fratio = 2;
		freq_in *= 4;
	} else if (freq_in > 64000) {
		fll->fll_fratio = 3;
		freq_in *= 8;
	} else {
		fll->fll_fratio = 4;
		freq_in *= 16;
	}
	pr_debug("FLL_FRATIO=%d, Fref=%dHz\n", fll->fll_fratio, freq_in);

	/* Now, calculate N.K */
	Ndiv = freq_out / freq_in;

	fll->n = Ndiv;
	Nmod = freq_out % freq_in;
	pr_debug("Nmod=%d\n", Nmod);

	/* Calculate fractional part - scale up so we can round. */
	Kpart = FIXED_FLL_SIZE * (long long)Nmod;

	do_div(Kpart, freq_in);

	K = Kpart & 0xFFFFFFFF;

	if ((K % 10) >= 5)
		K += 5;

	/* Move down to proper range now rounding is done */
	fll->k = K / 10;

	pr_debug("N=%x K=%x\n", fll->n, fll->k);

	return 0;
}

static int _wm8994_set_fll(struct snd_soc_codec *codec, int id, int src,
			  unsigned int freq_in, unsigned int freq_out)
{
	struct wm8994_priv *wm8994 = snd_soc_codec_get_drvdata(codec);
	struct wm8994 *control = codec->control_data;
	int reg_offset, ret;
	struct fll_div fll;
	u16 reg, clk1, aif_reg, aif_src;
	unsigned long timeout;
	bool was_enabled;
	dev_info(codec->dev, "%s ++\n", __func__);

	switch (id) {
	case WM8994_FLL1:
		reg_offset = 0;
		id = 0;
		aif_src = 0x10;
		break;
	case WM8994_FLL2:
		reg_offset = 0x20;
		id = 1;
		aif_src = 0x18;
		break;
	default:
		return -EINVAL;
	}

	reg = snd_soc_read(codec, WM8994_FLL1_CONTROL_1 + reg_offset);
	was_enabled = reg & WM8994_FLL1_ENA;

	switch (src) {
	case 0:
		/* Allow no source specification when stopping */
		if (freq_out)
			return -EINVAL;
		src = wm8994->fll[id].src;
		break;
	case WM8994_FLL_SRC_MCLK1:
	case WM8994_FLL_SRC_MCLK2:
	case WM8994_FLL_SRC_LRCLK:
	case WM8994_FLL_SRC_BCLK:
		break;
	default:
		return -EINVAL;
	}

	/* Are we changing anything? */
	if (wm8994->fll[id].src == src &&
	    wm8994->fll[id].in == freq_in && wm8994->fll[id].out == freq_out)
		return 0;

	/* If we're stopping the FLL redo the old config - no
	 * registers will actually be written but we avoid GCC flow
	 * analysis bugs spewing warnings.
	 */
	if (freq_out)
		ret = wm8994_get_fll_config(&fll, freq_in, freq_out);
	else
		ret = wm8994_get_fll_config(&fll, wm8994->fll[id].in,
					    wm8994->fll[id].out);
	if (ret < 0)
		return ret;

	/* Make sure that we're not providing SYSCLK right now */
	clk1 = snd_soc_read(codec, WM8994_CLOCKING_1);
	if (clk1 & WM8994_SYSCLK_SRC)
		aif_reg = WM8994_AIF2_CLOCKING_1;
	else
		aif_reg = WM8994_AIF1_CLOCKING_1;
	reg = snd_soc_read(codec, aif_reg);

	if ((reg & WM8994_AIF1CLK_ENA) &&
	    (reg & WM8994_AIF1CLK_SRC_MASK) == aif_src) {
		dev_err(codec->dev, "FLL%d is currently providing SYSCLK\n",
			id + 1);
		return -EBUSY;
	}


	/* We always need to disable the FLL while reconfiguring */
	snd_soc_update_bits(codec, WM8994_FLL1_CONTROL_1 + reg_offset,
			    WM8994_FLL1_ENA, 0);

	if (wm8994->fll_byp && src == WM8994_FLL_SRC_BCLK &&
	    freq_in == freq_out && freq_out) {
		dev_dbg(codec->dev, "Bypassing FLL%d\n", id + 1);
		snd_soc_update_bits(codec, WM8994_FLL1_CONTROL_5 + reg_offset,
				    WM8958_FLL1_BYP, WM8958_FLL1_BYP);
		goto out;
	}

	reg = (fll.outdiv << WM8994_FLL1_OUTDIV_SHIFT) |
		(fll.fll_fratio << WM8994_FLL1_FRATIO_SHIFT);
	snd_soc_update_bits(codec, WM8994_FLL1_CONTROL_2 + reg_offset,
			    WM8994_FLL1_OUTDIV_MASK |
			    WM8994_FLL1_FRATIO_MASK, reg);

	snd_soc_update_bits(codec, WM8994_FLL1_CONTROL_3 + reg_offset,
			    WM8994_FLL1_K_MASK, fll.k);

	snd_soc_update_bits(codec, WM8994_FLL1_CONTROL_4 + reg_offset,
			    WM8994_FLL1_N_MASK,
				    fll.n << WM8994_FLL1_N_SHIFT);

	snd_soc_update_bits(codec, WM8994_FLL1_CONTROL_5 + reg_offset,
			    WM8958_FLL1_BYP |
			    WM8994_FLL1_REFCLK_DIV_MASK |
			    WM8994_FLL1_REFCLK_SRC_MASK,
			    (fll.clk_ref_div << WM8994_FLL1_REFCLK_DIV_SHIFT) |
			    (src - 1));

	/* Clear any pending completion from a previous failure */
	try_wait_for_completion(&wm8994->fll_locked[id]);

	/* Enable (with fractional mode if required) */
	if (freq_out) {
		/* Enable VMID if we need it */
		if (!was_enabled) {
			active_reference(codec);

			switch (control->type) {
			case WM8994:
				vmid_reference(codec);
				break;
			case WM8958:
				if (wm8994->revision < 1)
					vmid_reference(codec);
				break;
			default:
				break;
			}
		}

		if (fll.k)
			reg = WM8994_FLL1_ENA | WM8994_FLL1_FRAC;
		else
			reg = WM8994_FLL1_ENA;
		snd_soc_update_bits(codec, WM8994_FLL1_CONTROL_1 + reg_offset,
				    WM8994_FLL1_ENA | WM8994_FLL1_FRAC,
				    reg);

		if (wm8994->fll_locked_irq) {
			timeout = wait_for_completion_timeout(&wm8994->fll_locked[id],
							      msecs_to_jiffies(10));
			if (timeout == 0)
				dev_warn(codec->dev,
					 "Timed out waiting for FLL lock\n");
		} else {
			msleep(5);
		}
	} else {
		if (was_enabled) {
			switch (control->type) {
			case WM8994:
				vmid_dereference(codec);
				break;
			case WM8958:
				if (wm8994->revision < 1)
					vmid_dereference(codec);
				break;
			default:
				break;
			}

			active_dereference(codec);
		}
	}

out:
	wm8994->fll[id].in = freq_in;
	wm8994->fll[id].out = freq_out;
	wm8994->fll[id].src = src;

	configure_clock(codec);
	dev_info(codec->dev, "%s --\n", __func__);
	return 0;
}

static irqreturn_t wm8994_fll_locked_irq(int irq, void *data)
{
	struct completion *completion = data;

	complete(completion);

	return IRQ_HANDLED;
}

static int opclk_divs[] = { 10, 20, 30, 40, 55, 60, 80, 120, 160 };

static int wm8994_set_fll(struct snd_soc_dai *dai, int id, int src,
			  unsigned int freq_in, unsigned int freq_out)
{
	return _wm8994_set_fll(dai->codec, id, src, freq_in, freq_out);
}

static int wm8994_set_dai_sysclk(struct snd_soc_dai *dai,
		int clk_id, unsigned int freq, int dir)
{
	struct snd_soc_codec *codec = dai->codec;
	struct wm8994_priv *wm8994 = snd_soc_codec_get_drvdata(codec);
	int i;

	switch (dai->id) {
	case 1:
	case 2:
		break;

	default:
		/* AIF3 shares clocking with AIF1/2 */
		return -EINVAL;
	}

	switch (clk_id) {
	case WM8994_SYSCLK_MCLK1:
		wm8994->sysclk[dai->id - 1] = WM8994_SYSCLK_MCLK1;
		wm8994->mclk[0] = freq;
		dev_dbg(dai->dev, "AIF%d using MCLK1 at %uHz\n",
			dai->id, freq);
		break;

	case WM8994_SYSCLK_MCLK2:
		/* TODO: Set GPIO AF */
		wm8994->sysclk[dai->id - 1] = WM8994_SYSCLK_MCLK2;
		wm8994->mclk[1] = freq;
		dev_dbg(dai->dev, "AIF%d using MCLK2 at %uHz\n",
			dai->id, freq);
		break;

	case WM8994_SYSCLK_FLL1:
		wm8994->sysclk[dai->id - 1] = WM8994_SYSCLK_FLL1;
		dev_dbg(dai->dev, "AIF%d using FLL1\n", dai->id);
		break;

	case WM8994_SYSCLK_FLL2:
		wm8994->sysclk[dai->id - 1] = WM8994_SYSCLK_FLL2;
		dev_dbg(dai->dev, "AIF%d using FLL2\n", dai->id);
		break;

	case WM8994_SYSCLK_OPCLK:
		/* Special case - a division (times 10) is given and
		 * no effect on main clocking.
		 */
		if (freq) {
			for (i = 0; i < ARRAY_SIZE(opclk_divs); i++)
				if (opclk_divs[i] == freq)
					break;
			if (i == ARRAY_SIZE(opclk_divs))
				return -EINVAL;
			snd_soc_update_bits(codec, WM8994_CLOCKING_2,
					    WM8994_OPCLK_DIV_MASK, i);
			snd_soc_update_bits(codec, WM8994_POWER_MANAGEMENT_2,
					    WM8994_OPCLK_ENA, WM8994_OPCLK_ENA);
		} else {
			snd_soc_update_bits(codec, WM8994_POWER_MANAGEMENT_2,
					    WM8994_OPCLK_ENA, 0);
		}

	default:
		return -EINVAL;
	}

	configure_clock(codec);

	/*
	 * If SYSCLK will be less than 50kHz adjust AIFnCLK dividers
	 * for detection.
	 */
	if ((max(wm8994->aifclk[0], wm8994->aifclk[1]) < 50000) && (!wm8994->aifdiv[0])) {
		dev_dbg(codec->dev, "Configuring AIFs for 128fs\n");
		wm8994->aifdiv[0] = snd_soc_read(codec, WM8994_AIF1_RATE)
			& WM8994_AIF1CLK_RATE_MASK;
		wm8994->aifdiv[1] = snd_soc_read(codec, WM8994_AIF2_RATE)
			& WM8994_AIF1CLK_RATE_MASK;

		snd_soc_update_bits(codec, WM8994_AIF1_RATE,
				    WM8994_AIF1CLK_RATE_MASK, 0x1);
		snd_soc_update_bits(codec, WM8994_AIF2_RATE,
				    WM8994_AIF2CLK_RATE_MASK, 0x1);
	} else if ((max(wm8994->aifclk[0], wm8994->aifclk[1]) >= 50000) && wm8994->aifdiv[0]) {
		snd_soc_update_bits(codec, WM8994_AIF1_RATE,
				    WM8994_AIF1CLK_RATE_MASK,
				    wm8994->aifdiv[0]);
		snd_soc_update_bits(codec, WM8994_AIF2_RATE,
				    WM8994_AIF2CLK_RATE_MASK,
				    wm8994->aifdiv[1]);

		wm8994->aifdiv[0] = 0;
		wm8994->aifdiv[1] = 0;
	}

	return 0;
}

static int wm8994_set_bias_level(struct snd_soc_codec *codec,
				 enum snd_soc_bias_level level)
{
	struct wm8994 *control = codec->control_data;
	struct wm8994_priv *wm8994 = snd_soc_codec_get_drvdata(codec);

	wm_hubs_set_bias_level(codec, level);

	switch (level) {
	case SND_SOC_BIAS_ON:
		break;

	case SND_SOC_BIAS_PREPARE:
		/* MICBIAS into regulating mode */
		switch (control->type) {
		case WM8958:
		case WM1811:
			snd_soc_update_bits(codec, WM8958_MICBIAS1,
					    WM8958_MICB1_MODE, 0);
			snd_soc_update_bits(codec, WM8958_MICBIAS2,
					    WM8958_MICB2_MODE, 0);
			break;
		default:
			break;
		}

		if (codec->dapm.bias_level == SND_SOC_BIAS_STANDBY)
			active_reference(codec);
		break;

	case SND_SOC_BIAS_STANDBY:
		if (codec->dapm.bias_level == SND_SOC_BIAS_OFF) {
#if 0 /* To do */
			pm_runtime_get_sync(codec->dev);
#endif

			switch (control->type) {
			case WM8994:
				if (wm8994->revision < 4) {
					/* Tweak DC servo and DSP
					 * configuration for improved
					 * performance. */
					snd_soc_write(codec, 0x102, 0x3);
					snd_soc_write(codec, 0x56, 0x3);
					snd_soc_write(codec, 0x817, 0);
					snd_soc_write(codec, 0x102, 0);
				}
				break;

			case WM8958:
				if (wm8994->revision == 0) {
					/* Optimise performance for rev A */
					snd_soc_write(codec, 0x102, 0x3);
					snd_soc_write(codec, 0xcb, 0x81);
					snd_soc_write(codec, 0x817, 0);
					snd_soc_write(codec, 0x102, 0);

					snd_soc_update_bits(codec,
							    WM8958_CHARGE_PUMP_2,
							    WM8958_CP_DISCH,
							    WM8958_CP_DISCH);
				}
				break;

			case WM1811:
				snd_soc_write(codec, 0x102, 0x3);
				snd_soc_write(codec, 0x56, 0xc07);
				snd_soc_write(codec, 0x5d, 0x7e);
				snd_soc_write(codec, 0x5e, 0x0);
				snd_soc_write(codec, 0x102, 0x0);
				break;
			}

			/* Discharge LINEOUT1 & 2 */
			snd_soc_update_bits(codec, WM8994_ANTIPOP_1,
					    WM8994_LINEOUT1_DISCH |
					    WM8994_LINEOUT2_DISCH,
					    WM8994_LINEOUT1_DISCH |
					    WM8994_LINEOUT2_DISCH);
		}

		if (codec->dapm.bias_level == SND_SOC_BIAS_PREPARE)
			active_dereference(codec);

		/* MICBIAS into bypass mode on newer devices */
		switch (control->type) {
		case WM8958:
		case WM1811:
			snd_soc_update_bits(codec, WM8958_MICBIAS1,
					    WM8958_MICB1_MODE,
					    WM8958_MICB1_MODE);
			snd_soc_update_bits(codec, WM8958_MICBIAS2,
					    WM8958_MICB2_MODE,
					    WM8958_MICB2_MODE);
			break;
		default:
			break;
		}
		break;

	case SND_SOC_BIAS_OFF:
		if (codec->dapm.bias_level == SND_SOC_BIAS_STANDBY) {
			wm8994->cur_fw = NULL;

#if 0 /* To do */
			pm_runtime_put(codec->dev);
#endif
		}
		break;
	}

	codec->dapm.bias_level = level;

	return 0;
}

int wm8994_vmid_mode(struct snd_soc_codec *codec, enum wm8994_vmid_mode mode)
{
	struct wm8994_priv *wm8994 = snd_soc_codec_get_drvdata(codec);

	switch (mode) {
	case WM8994_VMID_NORMAL:
		if (wm8994->hubs.lineout1_se) {
			snd_soc_dapm_disable_pin(&codec->dapm,
						 "LINEOUT1N Driver");
			snd_soc_dapm_disable_pin(&codec->dapm,
						 "LINEOUT1P Driver");
		}
		if (wm8994->hubs.lineout2_se) {
			snd_soc_dapm_disable_pin(&codec->dapm,
						 "LINEOUT2N Driver");
			snd_soc_dapm_disable_pin(&codec->dapm,
						 "LINEOUT2P Driver");
		}

		/* Do the sync with the old mode to allow it to clean up */
		snd_soc_dapm_sync(&codec->dapm);
		wm8994->vmid_mode = mode;
		break;

	case WM8994_VMID_FORCE:
		if (wm8994->hubs.lineout1_se) {
			snd_soc_dapm_force_enable_pin(&codec->dapm,
						      "LINEOUT1N Driver");
			snd_soc_dapm_force_enable_pin(&codec->dapm,
						      "LINEOUT1P Driver");
		}
		if (wm8994->hubs.lineout2_se) {
			snd_soc_dapm_force_enable_pin(&codec->dapm,
						      "LINEOUT2N Driver");
			snd_soc_dapm_force_enable_pin(&codec->dapm,
						      "LINEOUT2P Driver");
		}

		wm8994->vmid_mode = mode;
		snd_soc_dapm_sync(&codec->dapm);
		break;

	default:
		return -EINVAL;
	}

	return 0;
}

static int wm8994_set_dai_fmt(struct snd_soc_dai *dai, unsigned int fmt)
{
	struct snd_soc_codec *codec = dai->codec;
	struct wm8994 *control = codec->control_data;
	int ms_reg;
	int aif1_reg;
	int ms = 0;
	int aif1 = 0;

	switch (dai->id) {
	case 1:
		ms_reg = WM8994_AIF1_MASTER_SLAVE;
		aif1_reg = WM8994_AIF1_CONTROL_1;
		break;
	case 2:
		ms_reg = WM8994_AIF2_MASTER_SLAVE;
		aif1_reg = WM8994_AIF2_CONTROL_1;
		break;
	default:
		return -EINVAL;
	}

	switch (fmt & SND_SOC_DAIFMT_MASTER_MASK) {
	case SND_SOC_DAIFMT_CBS_CFS:
		break;
	case SND_SOC_DAIFMT_CBM_CFM:
		ms = WM8994_AIF1_MSTR;
		break;
	default:
		return -EINVAL;
	}

	switch (fmt & SND_SOC_DAIFMT_FORMAT_MASK) {
	case SND_SOC_DAIFMT_DSP_B:
		aif1 |= WM8994_AIF1_LRCLK_INV;
	case SND_SOC_DAIFMT_DSP_A:
		aif1 |= 0x18;
		break;
	case SND_SOC_DAIFMT_I2S:
		aif1 |= 0x10;
		break;
	case SND_SOC_DAIFMT_RIGHT_J:
		break;
	case SND_SOC_DAIFMT_LEFT_J:
		aif1 |= 0x8;
		break;
	default:
		return -EINVAL;
	}

	switch (fmt & SND_SOC_DAIFMT_FORMAT_MASK) {
	case SND_SOC_DAIFMT_DSP_A:
	case SND_SOC_DAIFMT_DSP_B:
		/* frame inversion not valid for DSP modes */
		switch (fmt & SND_SOC_DAIFMT_INV_MASK) {
		case SND_SOC_DAIFMT_NB_NF:
			break;
		case SND_SOC_DAIFMT_IB_NF:
			aif1 |= WM8994_AIF1_BCLK_INV;
			break;
		default:
			return -EINVAL;
		}
		break;

	case SND_SOC_DAIFMT_I2S:
	case SND_SOC_DAIFMT_RIGHT_J:
	case SND_SOC_DAIFMT_LEFT_J:
		switch (fmt & SND_SOC_DAIFMT_INV_MASK) {
		case SND_SOC_DAIFMT_NB_NF:
			break;
		case SND_SOC_DAIFMT_IB_IF:
			aif1 |= WM8994_AIF1_BCLK_INV | WM8994_AIF1_LRCLK_INV;
			break;
		case SND_SOC_DAIFMT_IB_NF:
			aif1 |= WM8994_AIF1_BCLK_INV;
			break;
		case SND_SOC_DAIFMT_NB_IF:
			aif1 |= WM8994_AIF1_LRCLK_INV;
			break;
		default:
			return -EINVAL;
		}
		break;
	default:
		return -EINVAL;
	}

	/* The AIF2 format configuration needs to be mirrored to AIF3
	 * on WM8958 if it's in use so just do it all the time. */
	switch (control->type) {
	case WM1811:
	case WM8958:
		if (dai->id == 2)
			snd_soc_update_bits(codec, WM8958_AIF3_CONTROL_1,
					    WM8994_AIF1_LRCLK_INV |
					    WM8958_AIF3_FMT_MASK, aif1);
		break;

	default:
		break;
	}

	snd_soc_update_bits(codec, aif1_reg,
			    WM8994_AIF1_BCLK_INV | WM8994_AIF1_LRCLK_INV |
			    WM8994_AIF1_FMT_MASK,
			    aif1);
	snd_soc_update_bits(codec, ms_reg, WM8994_AIF1_MSTR,
			    ms);

	return 0;
}

static struct {
	int val, rate;
} srs[] = {
	{ 0,   8000 },
	{ 1,  11025 },
	{ 2,  12000 },
	{ 3,  16000 },
	{ 4,  22050 },
	{ 5,  24000 },
	{ 6,  32000 },
	{ 7,  44100 },
	{ 8,  48000 },
	{ 9,  88200 },
	{ 10, 96000 },
};

static int fs_ratios[] = {
	64, 128, 192, 256, 348, 512, 768, 1024, 1408, 1536
};

static int bclk_divs[] = {
	10, 15, 20, 30, 40, 50, 60, 80, 110, 120, 160, 220, 240, 320, 440, 480,
	640, 880, 960, 1280, 1760, 1920
};

static int wm8994_hw_params(struct snd_pcm_substream *substream,
			    struct snd_pcm_hw_params *params,
			    struct snd_soc_dai *dai)
{
	struct snd_soc_codec *codec = dai->codec;
	struct wm8994 *control = codec->control_data;
	struct wm8994_priv *wm8994 = snd_soc_codec_get_drvdata(codec);
	int aif1_reg;
	int aif2_reg;
	int bclk_reg;
	int lrclk_reg;
	int rate_reg;
	int aif1 = 0;
	int aif2 = 0;
	int bclk = 0;
	int lrclk = 0;
	int rate_val = 0;
	int id = dai->id - 1;

	int i, cur_val, best_val, bclk_rate, best;

	switch (dai->id) {
	case 1:
		aif1_reg = WM8994_AIF1_CONTROL_1;
		aif2_reg = WM8994_AIF1_CONTROL_2;
		bclk_reg = WM8994_AIF1_BCLK;
		rate_reg = WM8994_AIF1_RATE;
		if (substream->stream == SNDRV_PCM_STREAM_PLAYBACK ||
		    wm8994->lrclk_shared[0]) {
			lrclk_reg = WM8994_AIF1DAC_LRCLK;
		} else {
			lrclk_reg = WM8994_AIF1ADC_LRCLK;
			dev_dbg(codec->dev, "AIF1 using split LRCLK\n");
		}
		break;
	case 2:
		aif1_reg = WM8994_AIF2_CONTROL_1;
		aif2_reg = WM8994_AIF2_CONTROL_2;
		bclk_reg = WM8994_AIF2_BCLK;
		rate_reg = WM8994_AIF2_RATE;
		if (substream->stream == SNDRV_PCM_STREAM_PLAYBACK ||
		    wm8994->lrclk_shared[1]) {
			lrclk_reg = WM8994_AIF2DAC_LRCLK;
		} else {
			lrclk_reg = WM8994_AIF2ADC_LRCLK;
			dev_dbg(codec->dev, "AIF2 using split LRCLK\n");
		}
		break;
	case 3:
		switch (control->type) {
		case WM1811:
		case WM8958:
			aif1_reg = WM8958_AIF3_CONTROL_1;
			break;
		default:
			return 0;
		}
	default:
		return -EINVAL;
	}

	bclk_rate = params_rate(params) * 2;
	switch (params_format(params)) {
	case SNDRV_PCM_FORMAT_S16_LE:
		bclk_rate *= 16;
		break;
	case SNDRV_PCM_FORMAT_S20_3LE:
		bclk_rate *= 20;
		aif1 |= 0x20;
		break;
	case SNDRV_PCM_FORMAT_S24_LE:
		bclk_rate *= 24;
		aif1 |= 0x40;
		break;
	case SNDRV_PCM_FORMAT_S32_LE:
		bclk_rate *= 32;
		aif1 |= 0x60;
		break;
	default:
		return -EINVAL;
	}

	/* Try to find an appropriate sample rate; look for an exact match. */
	for (i = 0; i < ARRAY_SIZE(srs); i++)
		if (srs[i].rate == params_rate(params))
			break;
	if (i == ARRAY_SIZE(srs))
		return -EINVAL;
	rate_val |= srs[i].val << WM8994_AIF1_SR_SHIFT;

	dev_dbg(dai->dev, "Sample rate is %dHz\n", srs[i].rate);
	dev_dbg(dai->dev, "AIF%dCLK is %dHz, target BCLK %dHz\n",
		dai->id, wm8994->aifclk[id], bclk_rate);

	if (params_channels(params) == 1 &&
	    (snd_soc_read(codec, aif1_reg) & 0x18) == 0x18)
		aif2 |= WM8994_AIF1_MONO;

	if (wm8994->aifclk[id] == 0) {
		dev_err(dai->dev, "AIF%dCLK not configured\n", dai->id);
		return -EINVAL;
	}

	/* AIFCLK/fs ratio; look for a close match in either direction */
	best = 0;
	best_val = abs((fs_ratios[0] * params_rate(params))
		       - wm8994->aifclk[id]);
	for (i = 1; i < ARRAY_SIZE(fs_ratios); i++) {
		cur_val = abs((fs_ratios[i] * params_rate(params))
			      - wm8994->aifclk[id]);
		if (cur_val >= best_val)
			continue;
		best = i;
		best_val = cur_val;
	}
	dev_dbg(dai->dev, "Selected AIF%dCLK/fs = %d\n",
		dai->id, fs_ratios[best]);
	rate_val |= best;

	/* We may not get quite the right frequency if using
	 * approximate clocks so look for the closest match that is
	 * higher than the target (we need to ensure that there enough
	 * BCLKs to clock out the samples).
	 */
	best = 0;
	for (i = 0; i < ARRAY_SIZE(bclk_divs); i++) {
		cur_val = (wm8994->aifclk[id] * 10 / bclk_divs[i]) - bclk_rate;
		if (cur_val < 0) /* BCLK table is sorted */
			break;
		best = i;
	}
	bclk_rate = wm8994->aifclk[id] * 10 / bclk_divs[best];
	dev_dbg(dai->dev, "Using BCLK_DIV %d for actual BCLK %dHz\n",
		bclk_divs[best], bclk_rate);
	bclk |= best << WM8994_AIF1_BCLK_DIV_SHIFT;

	lrclk = bclk_rate / params_rate(params);
	dev_dbg(dai->dev, "Using LRCLK rate %d for actual LRCLK %dHz\n",
		lrclk, bclk_rate / lrclk);

	snd_soc_update_bits(codec, aif1_reg, WM8994_AIF1_WL_MASK, aif1);
	snd_soc_update_bits(codec, aif2_reg, WM8994_AIF1_MONO, aif2);
	snd_soc_update_bits(codec, bclk_reg, WM8994_AIF1_BCLK_DIV_MASK, bclk);
	snd_soc_update_bits(codec, lrclk_reg, WM8994_AIF1DAC_RATE_MASK,
			    lrclk);
	snd_soc_update_bits(codec, rate_reg, WM8994_AIF1_SR_MASK |
			    WM8994_AIF1CLK_RATE_MASK, rate_val);

	if (substream->stream == SNDRV_PCM_STREAM_PLAYBACK) {
		switch (dai->id) {
		case 1:
			wm8994->dac_rates[0] = params_rate(params);
			wm8994_set_retune_mobile(codec, 0);
			wm8994_set_retune_mobile(codec, 1);
			break;
		case 2:
			wm8994->dac_rates[1] = params_rate(params);
			wm8994_set_retune_mobile(codec, 2);
			break;
		}
	}

	return 0;
}

static int wm8994_aif3_hw_params(struct snd_pcm_substream *substream,
				 struct snd_pcm_hw_params *params,
				 struct snd_soc_dai *dai)
{
	struct snd_soc_codec *codec = dai->codec;
	struct wm8994 *control = codec->control_data;
	int aif1_reg;
	int aif1 = 0;

	switch (dai->id) {
	case 3:
		switch (control->type) {
		case WM1811:
		case WM8958:
			aif1_reg = WM8958_AIF3_CONTROL_1;
			break;
		default:
			return 0;
		}
	default:
		return 0;
	}

	switch (params_format(params)) {
	case SNDRV_PCM_FORMAT_S16_LE:
		break;
	case SNDRV_PCM_FORMAT_S20_3LE:
		aif1 |= 0x20;
		break;
	case SNDRV_PCM_FORMAT_S24_LE:
		aif1 |= 0x40;
		break;
	case SNDRV_PCM_FORMAT_S32_LE:
		aif1 |= 0x60;
		break;
	default:
		return -EINVAL;
	}

	return snd_soc_update_bits(codec, aif1_reg, WM8994_AIF1_WL_MASK, aif1);
}

static int wm8994_aif_mute(struct snd_soc_dai *codec_dai, int mute)
{
	struct snd_soc_codec *codec = codec_dai->codec;
	int mute_reg;
	int reg;

	switch (codec_dai->id) {
	case 1:
		mute_reg = WM8994_AIF1_DAC1_FILTERS_1;
		break;
	case 2:
		mute_reg = WM8994_AIF2_DAC_FILTERS_1;
		if (snd_soc_read(codec, WM8994_POWER_MANAGEMENT_6)
			& WM8994_AIF2_DACDAT_SRC)
			mute = 0;
		break;
	default:
		return -EINVAL;
	}

	if (mute)
		reg = WM8994_AIF1DAC1_MUTE;
	else
		reg = 0;

	snd_soc_update_bits(codec, mute_reg, WM8994_AIF1DAC1_MUTE, reg);

	return 0;
}

static int wm8994_set_tristate(struct snd_soc_dai *codec_dai, int tristate)
{
	struct snd_soc_codec *codec = codec_dai->codec;
	int reg, val, mask;

	switch (codec_dai->id) {
	case 1:
		reg = WM8994_AIF1_MASTER_SLAVE;
		mask = WM8994_AIF1_TRI;
		break;
	case 2:
		reg = WM8994_AIF2_MASTER_SLAVE;
		mask = WM8994_AIF2_TRI;
		break;
	case 3:
		reg = WM8994_POWER_MANAGEMENT_6;
		mask = WM8994_AIF3_TRI;
		break;
	default:
		return -EINVAL;
	}

	if (tristate)
		val = mask;
	else
		val = 0;

	return snd_soc_update_bits(codec, reg, mask, val);
}

static int wm8994_aif2_probe(struct snd_soc_dai *dai)
{
	struct snd_soc_codec *codec = dai->codec;

	/* Disable the pulls on the AIF if we're using it to save power. */
	snd_soc_update_bits(codec, WM8994_GPIO_3,
			    WM8994_GPN_PU | WM8994_GPN_PD, 0);
	snd_soc_update_bits(codec, WM8994_GPIO_4,
			    WM8994_GPN_PU | WM8994_GPN_PD, 0);
	snd_soc_update_bits(codec, WM8994_GPIO_5,
			    WM8994_GPN_PU | WM8994_GPN_PD, 0);

	return 0;
}

#define WM8994_RATES SNDRV_PCM_RATE_8000_96000

#define WM8994_FORMATS (SNDRV_PCM_FMTBIT_S16_LE | SNDRV_PCM_FMTBIT_S20_3LE |\
			SNDRV_PCM_FMTBIT_S24_LE | SNDRV_PCM_FMTBIT_S32_LE)

static struct snd_soc_dai_ops wm8994_aif1_dai_ops = {
	.set_sysclk	= wm8994_set_dai_sysclk,
	.set_fmt	= wm8994_set_dai_fmt,
	.hw_params	= wm8994_hw_params,
	.digital_mute	= wm8994_aif_mute,
	.set_pll	= wm8994_set_fll,
	.set_tristate	= wm8994_set_tristate,
};

static struct snd_soc_dai_ops wm8994_aif2_dai_ops = {
	.set_sysclk	= wm8994_set_dai_sysclk,
	.set_fmt	= wm8994_set_dai_fmt,
	.hw_params	= wm8994_hw_params,
	.digital_mute   = wm8994_aif_mute,
	.set_pll	= wm8994_set_fll,
	.set_tristate	= wm8994_set_tristate,
};

static struct snd_soc_dai_ops wm8994_aif3_dai_ops = {
	.hw_params	= wm8994_aif3_hw_params,
	.set_tristate	= wm8994_set_tristate,
};

static struct snd_soc_dai_driver wm8994_dai[] = {
	{
		.name = "wm8994-aif1",
		.id = 1,
		.playback = {
			.stream_name = "AIF1 Playback",
			.channels_min = 1,
			.channels_max = 2,
			.rates = WM8994_RATES,
			.formats = WM8994_FORMATS,
		},
		.capture = {
			.stream_name = "AIF1 Capture",
			.channels_min = 1,
			.channels_max = 2,
			.rates = WM8994_RATES,
			.formats = WM8994_FORMATS,
		 },
		.ops = &wm8994_aif1_dai_ops,
	},
	{
		.name = "wm8994-aif2",
		.id = 2,
		.playback = {
			.stream_name = "AIF2 Playback",
			.channels_min = 1,
			.channels_max = 2,
			.rates = WM8994_RATES,
			.formats = WM8994_FORMATS,
		},
		.capture = {
			.stream_name = "AIF2 Capture",
			.channels_min = 1,
			.channels_max = 2,
			.rates = WM8994_RATES,
			.formats = WM8994_FORMATS,
		},
		.probe = wm8994_aif2_probe,
		.ops = &wm8994_aif2_dai_ops,
	},
	{
		.name = "wm8994-aif3",
		.id = 3,
		.playback = {
			.stream_name = "AIF3 Playback",
			.channels_min = 1,
			.channels_max = 2,
			.rates = WM8994_RATES,
			.formats = WM8994_FORMATS,
		},
		.capture = {
			.stream_name = "AIF3 Capture",
			.channels_min = 1,
			.channels_max = 2,
			.rates = WM8994_RATES,
			.formats = WM8994_FORMATS,
		},
		.ops = &wm8994_aif3_dai_ops,
	}
};

#ifdef CONFIG_PM
static int wm8994_codec_suspend(struct snd_soc_codec *codec, pm_message_t state)
{
	struct wm8994_priv *wm8994 = snd_soc_codec_get_drvdata(codec);
	struct wm8994 *control = codec->control_data;
	int i, ret;

	switch (control->type) {
	case WM8994:
		snd_soc_update_bits(codec, WM8994_MICBIAS, WM8994_MICD_ENA, 0);
		break;
	case WM1811:
		snd_soc_update_bits(codec, WM8994_ANTIPOP_2,
				    WM1811_JACKDET_MODE_MASK, 0);
		/* Fall through */
	case WM8958:
		snd_soc_update_bits(codec, WM8958_MIC_DETECT_1,
				    WM8958_MICD_ENA, 0);
		break;
	}

	for (i = 0; i < ARRAY_SIZE(wm8994->fll); i++) {
		memcpy(&wm8994->fll_suspend[i], &wm8994->fll[i],
		       sizeof(struct wm8994_fll_config));
		ret = _wm8994_set_fll(codec, i + 1, 0, 0, 0);
		if (ret < 0)
			dev_warn(codec->dev, "Failed to stop FLL%d: %d\n",
				 i + 1, ret);
	}

	wm8994_set_bias_level(codec, SND_SOC_BIAS_OFF);

	return 0;
}

static int wm8994_codec_resume(struct snd_soc_codec *codec)
{
	struct wm8994_priv *wm8994 = snd_soc_codec_get_drvdata(codec);
	struct wm8994 *control = codec->control_data;
	int i, ret;
	unsigned int val, mask;

	if (wm8994->revision < 4) {
		/* force a HW read */
		val = wm8994_reg_read(codec->control_data,
				      WM8994_POWER_MANAGEMENT_5);
		if (val < 0)
			dev_err(codec->dev, "[%s]Failed to read PM5 ret[%d]\n",
					__func__, val);

		/* modify the cache only */
		codec->cache_only = 1;
		mask =  WM8994_DAC1R_ENA | WM8994_DAC1L_ENA |
			WM8994_DAC2R_ENA | WM8994_DAC2L_ENA;
		val &= mask;
		snd_soc_update_bits(codec, WM8994_POWER_MANAGEMENT_5,
				    mask, val);
		codec->cache_only = 0;
	}

	/* Restore the registers */
	ret = snd_soc_cache_sync(codec);
	if (ret != 0)
		dev_err(codec->dev, "Failed to sync cache: %d\n", ret);

	wm8994_set_bias_level(codec, SND_SOC_BIAS_STANDBY);

	for (i = 0; i < ARRAY_SIZE(wm8994->fll); i++) {
		if (!wm8994->fll_suspend[i].out)
			continue;

		ret = _wm8994_set_fll(codec, i + 1,
				     wm8994->fll_suspend[i].src,
				     wm8994->fll_suspend[i].in,
				     wm8994->fll_suspend[i].out);
		if (ret < 0)
			dev_warn(codec->dev, "Failed to restore FLL%d: %d\n",
				 i + 1, ret);
	}

	return 0;
}
#else
#define wm8994_codec_suspend NULL
#define wm8994_codec_resume NULL
#endif

static void wm8994_handle_retune_mobile_pdata(struct wm8994_priv *wm8994)
{
	struct snd_soc_codec *codec = wm8994->codec;
	struct wm8994_pdata *pdata = wm8994->pdata;
	struct snd_kcontrol_new controls[] = {
		SOC_ENUM_EXT("AIF1.1 EQ Mode",
			     wm8994->retune_mobile_enum,
			     wm8994_get_retune_mobile_enum,
			     wm8994_put_retune_mobile_enum),
		SOC_ENUM_EXT("AIF1.2 EQ Mode",
			     wm8994->retune_mobile_enum,
			     wm8994_get_retune_mobile_enum,
			     wm8994_put_retune_mobile_enum),
		SOC_ENUM_EXT("AIF2 EQ Mode",
			     wm8994->retune_mobile_enum,
			     wm8994_get_retune_mobile_enum,
			     wm8994_put_retune_mobile_enum),
	};
	int ret, i, j;
	const char **t;

	/* We need an array of texts for the enum API but the number
	 * of texts is likely to be less than the number of
	 * configurations due to the sample rate dependency of the
	 * configurations. */
	wm8994->num_retune_mobile_texts = 0;
	wm8994->retune_mobile_texts = NULL;
	for (i = 0; i < pdata->num_retune_mobile_cfgs; i++) {
		for (j = 0; j < wm8994->num_retune_mobile_texts; j++) {
			if (strcmp(pdata->retune_mobile_cfgs[i].name,
				   wm8994->retune_mobile_texts[j]) == 0)
				break;
		}

		if (j != wm8994->num_retune_mobile_texts)
			continue;

		/* Expand the array... */
		t = krealloc(wm8994->retune_mobile_texts,
			     sizeof(char *) *
			     (wm8994->num_retune_mobile_texts + 1),
			     GFP_KERNEL);
		if (t == NULL)
			continue;

		/* ...store the new entry... */
		t[wm8994->num_retune_mobile_texts] =
			pdata->retune_mobile_cfgs[i].name;

		/* ...and remember the new version. */
		wm8994->num_retune_mobile_texts++;
		wm8994->retune_mobile_texts = t;
	}

	dev_dbg(codec->dev, "Allocated %d unique ReTune Mobile names\n",
		wm8994->num_retune_mobile_texts);

	wm8994->retune_mobile_enum.max = wm8994->num_retune_mobile_texts;
	wm8994->retune_mobile_enum.texts = wm8994->retune_mobile_texts;

	ret = snd_soc_add_controls(wm8994->codec, controls,
				   ARRAY_SIZE(controls));
	if (ret != 0)
		dev_err(wm8994->codec->dev,
			"Failed to add ReTune Mobile controls: %d\n", ret);
}

static void wm8994_handle_pdata(struct wm8994_priv *wm8994)
{
	struct snd_soc_codec *codec = wm8994->codec;
	struct wm8994_pdata *pdata = wm8994->pdata;
	int ret, i;

	if (!pdata)
		return;

	wm_hubs_handle_analogue_pdata(codec, pdata->lineout1_diff,
				      pdata->lineout2_diff,
				      pdata->lineout1fb,
				      pdata->lineout2fb,
				      pdata->jd_scthr,
				      pdata->jd_thr,
				      pdata->micbias1_lvl,
				      pdata->micbias2_lvl);

	dev_dbg(codec->dev, "%d DRC configurations\n", pdata->num_drc_cfgs);

	if (pdata->num_drc_cfgs) {
		struct snd_kcontrol_new controls[] = {
			SOC_ENUM_EXT("AIF1DRC1 Mode", wm8994->drc_enum,
				     wm8994_get_drc_enum, wm8994_put_drc_enum),
			SOC_ENUM_EXT("AIF1DRC2 Mode", wm8994->drc_enum,
				     wm8994_get_drc_enum, wm8994_put_drc_enum),
			SOC_ENUM_EXT("AIF2DRC Mode", wm8994->drc_enum,
				     wm8994_get_drc_enum, wm8994_put_drc_enum),
		};

		/* We need an array of texts for the enum API */
		wm8994->drc_texts = kmalloc(sizeof(char *)
					    * pdata->num_drc_cfgs, GFP_KERNEL);
		if (!wm8994->drc_texts) {
			dev_err(wm8994->codec->dev,
				"Failed to allocate %d DRC config texts\n",
				pdata->num_drc_cfgs);
			return;
		}

		for (i = 0; i < pdata->num_drc_cfgs; i++)
			wm8994->drc_texts[i] = pdata->drc_cfgs[i].name;

		wm8994->drc_enum.max = pdata->num_drc_cfgs;
		wm8994->drc_enum.texts = wm8994->drc_texts;

		ret = snd_soc_add_controls(wm8994->codec, controls,
					   ARRAY_SIZE(controls));
		if (ret != 0)
			dev_err(wm8994->codec->dev,
				"Failed to add DRC mode controls: %d\n", ret);

		for (i = 0; i < WM8994_NUM_DRC; i++)
			wm8994_set_drc(codec, i);
	}

	dev_dbg(codec->dev, "%d ReTune Mobile configurations\n",
		pdata->num_retune_mobile_cfgs);

	if (pdata->num_retune_mobile_cfgs)
		wm8994_handle_retune_mobile_pdata(wm8994);
	else
		snd_soc_add_controls(wm8994->codec, wm8994_eq_controls,
				     ARRAY_SIZE(wm8994_eq_controls));

	for (i = 0; i < ARRAY_SIZE(pdata->micbias); i++) {
		if (pdata->micbias[i]) {
			snd_soc_write(codec, WM8958_MICBIAS1 + i,
				pdata->micbias[i] & 0xffff);
		}
	}
}

/**
 * wm8994_mic_detect - Enable microphone detection via the WM8994 IRQ
 *
 * @codec:   WM8994 codec
 * @jack:    jack to report detection events on
 * @micbias: microphone bias to detect on
 * @det:     value to report for presence detection
 * @shrt:    value to report for short detection
 *
 * Enable microphone detection via IRQ on the WM8994.  If GPIOs are
 * being used to bring out signals to the processor then only platform
 * data configuration is needed for WM8994 and processor GPIOs should
 * be configured using snd_soc_jack_add_gpios() instead.
 *
 * Configuration of detection levels is available via the micbias1_lvl
 * and micbias2_lvl platform data members.
 */
int wm8994_mic_detect(struct snd_soc_codec *codec, struct snd_soc_jack *jack,
		      int micbias, int det, int shrt)
{
	struct wm8994_priv *wm8994 = snd_soc_codec_get_drvdata(codec);
	struct wm8994_micdet *micdet;
	struct wm8994 *control = codec->control_data;
	int reg;

	if (control->type != WM8994)
		return -EINVAL;

	switch (micbias) {
	case 1:
		micdet = &wm8994->micdet[0];
		break;
	case 2:
		micdet = &wm8994->micdet[1];
		break;
	default:
		return -EINVAL;
	}

	dev_dbg(codec->dev, "Configuring microphone detection on %d: %x %x\n",
		micbias, det, shrt);

	/* Store the configuration */
	micdet->jack = jack;
	micdet->det = det;
	micdet->shrt = shrt;

	/* If either of the jacks is set up then enable detection */
	if (wm8994->micdet[0].jack || wm8994->micdet[1].jack)
		reg = WM8994_MICD_ENA;
	else
		reg = 0;

	snd_soc_update_bits(codec, WM8994_MICBIAS, WM8994_MICD_ENA, reg);

	return 0;
}
EXPORT_SYMBOL_GPL(wm8994_mic_detect);

static irqreturn_t wm8994_mic_irq(int irq, void *data)
{
	struct wm8994_priv *priv = data;
	struct snd_soc_codec *codec = priv->codec;
	int reg;
	int report;

#ifndef CONFIG_SND_SOC_WM8994_MODULE
	trace_snd_soc_jack_irq(dev_name(codec->dev));
#endif

	reg = snd_soc_read(codec, WM8994_INTERRUPT_RAW_STATUS_2);
	if (reg < 0) {
		dev_err(codec->dev, "Failed to read microphone status: %d\n",
			reg);
		return IRQ_HANDLED;
	}

	dev_dbg(codec->dev, "Microphone status: %x\n", reg);

	report = 0;
	if (reg & WM8994_MIC1_DET_STS)
		report |= priv->micdet[0].det;
	if (reg & WM8994_MIC1_SHRT_STS)
		report |= priv->micdet[0].shrt;
	snd_soc_jack_report(priv->micdet[0].jack, report,
			    priv->micdet[0].det | priv->micdet[0].shrt);

	report = 0;
	if (reg & WM8994_MIC2_DET_STS)
		report |= priv->micdet[1].det;
	if (reg & WM8994_MIC2_SHRT_STS)
		report |= priv->micdet[1].shrt;
	snd_soc_jack_report(priv->micdet[1].jack, report,
			    priv->micdet[1].det | priv->micdet[1].shrt);

	return IRQ_HANDLED;
}

static void wm1811_micd_stop(struct snd_soc_codec *codec)
{
	struct wm8994_priv *wm8994 = snd_soc_codec_get_drvdata(codec);

	if (!wm8994->jackdet)
		return;

	mutex_lock(&wm8994->accdet_lock);

	snd_soc_update_bits(codec, WM8958_MIC_DETECT_1, WM8958_MICD_ENA, 0);

	wm1811_jackdet_set_mode(codec, WM1811_JACKDET_MODE_JACK);

	mutex_unlock(&wm8994->accdet_lock);

	if (wm8994->pdata->jd_ext_cap)
		snd_soc_dapm_disable_pin(&codec->dapm,
					 "MICBIAS2");
}

static void wm8958_button_det(struct snd_soc_codec *codec, u16 status)
{
	struct wm8994_priv *wm8994 = snd_soc_codec_get_drvdata(codec);
	int report;

	report = 0;
	if (status & 0x4)
		report |= SND_JACK_BTN_0;

	if (status & 0x8)
		report |= SND_JACK_BTN_1;

	if (status & 0x10)
		report |= SND_JACK_BTN_2;

	if (status & 0x20)
		report |= SND_JACK_BTN_3;

	if (status & 0x40)
		report |= SND_JACK_BTN_4;

	if (status & 0x80)
		report |= SND_JACK_BTN_5;

	snd_soc_jack_report(wm8994->micdet[0].jack, report,
		wm8994->btn_mask);
}

static void wm8958_mic_id(void *data, u16 status)
{
	struct snd_soc_codec *codec = data;
	struct wm8994_priv *wm8994 = snd_soc_codec_get_drvdata(codec);

	/* Either nothing present or just starting detection */
	if (!(status & WM8958_MICD_STS)) {
			/* If nothing present then clear our statuses */
			dev_dbg(codec->dev, "Detected open circuit\n");
			wm8994->jack_mic = false;
			wm8994->mic_detecting = true;

		wm1811_micd_stop(codec);

			wm8958_micd_set_rate(codec);

			snd_soc_jack_report(wm8994->micdet[0].jack, 0,
					    wm8994->btn_mask |
					     SND_JACK_HEADSET);
		return;
	}
	/* If the measurement is showing a high impedence we've got a
	 * microphone.
	 */
	if (status & 0x600) {
		dev_dbg(codec->dev, "Detected microphone\n");

		wm8994->mic_detecting = false;
		wm8994->jack_mic = true;

		wm8958_micd_set_rate(codec);

		snd_soc_jack_report(wm8994->micdet[0].jack, SND_JACK_HEADSET,
				    SND_JACK_HEADSET);
	}


	if (status & 0x4) {
		dev_dbg(codec->dev, "Detected headphone\n");
		wm8994->mic_detecting = false;

		wm8958_micd_set_rate(codec);

		snd_soc_jack_report(wm8994->micdet[0].jack, SND_JACK_HEADPHONE,
				    SND_JACK_HEADSET);

		/* If we have jackdet that will detect removal */
		wm1811_micd_stop(codec);

		snd_soc_jack_report(wm8994->micdet[0].jack, SND_JACK_HEADPHONE,
				    SND_JACK_HEADSET);
	}
}

static irqreturn_t wm1811_jackdet_irq(int irq, void *data)
{
	struct wm8994_priv *wm8994 = data;
	struct snd_soc_codec *codec = wm8994->codec;
	int reg;
	bool present;

	mutex_lock(&wm8994->accdet_lock);

	reg = snd_soc_read(codec, WM1811_JACKDET_CTRL);
	if (reg < 0) {
		dev_err(codec->dev, "Failed to read jack status: %d\n", reg);
		mutex_unlock(&wm8994->accdet_lock);
		return IRQ_NONE;
	}

	dev_dbg(codec->dev, "JACKDET %x\n", reg);

	present = reg & WM1811_JACKDET_LVL;

	if (present) {
		dev_dbg(codec->dev, "Jack detected\n");

		wm8958_micd_set_rate(codec);

		snd_soc_update_bits(codec, WM8958_MICBIAS2,
				    WM8958_MICB2_DISCH, 0);

		/* Disable debounce while inserted */
		snd_soc_update_bits(codec, WM1811_JACKDET_CTRL,
				    WM1811_JACKDET_DB, 0);

		/* If there's a callback it'll be called out of the lock */
		if (!wm8994->micd_cb) {
		/*
			 * Start off measument of microphone impedence to find out
			 * what's actually there.
		 */
		wm8994->mic_detecting = true;
		wm1811_jackdet_set_mode(codec, WM1811_JACKDET_MODE_MIC);

		snd_soc_update_bits(codec, WM8958_MIC_DETECT_1,
				    WM8958_MICD_ENA, WM8958_MICD_ENA);
		}
	} else {
		dev_dbg(codec->dev, "Jack not detected\n");

		snd_soc_update_bits(codec, WM8958_MICBIAS2,
				    WM8958_MICB2_DISCH, WM8958_MICB2_DISCH);

		/* Enable debounce while removed */
		snd_soc_update_bits(codec, WM1811_JACKDET_CTRL,
				    WM1811_JACKDET_DB, WM1811_JACKDET_DB);

		wm8994->mic_detecting = false;
		wm8994->jack_mic = false;
		snd_soc_update_bits(codec, WM8958_MIC_DETECT_1,
				    WM8958_MICD_ENA, 0);
		wm1811_jackdet_set_mode(codec, WM1811_JACKDET_MODE_JACK);
	}

	mutex_unlock(&wm8994->accdet_lock);

	/* If required for an external cap force MICBIAS on */
	if (wm8994->pdata->jd_ext_cap) {
		mutex_lock(&codec->mutex);

		if (present)
			snd_soc_dapm_force_enable_pin(&codec->dapm,
						      "MICBIAS2");
		else
			snd_soc_dapm_disable_pin(&codec->dapm, "MICBIAS2");

		snd_soc_dapm_sync(&codec->dapm);
		mutex_unlock(&codec->mutex);
	}

	if (present)
		snd_soc_jack_report(wm8994->micdet[0].jack,
				    SND_JACK_MECHANICAL, SND_JACK_MECHANICAL);
	else
		snd_soc_jack_report(wm8994->micdet[0].jack, 0,
				    SND_JACK_MECHANICAL | SND_JACK_HEADSET |
				    wm8994->btn_mask);

	/* Custom callbacks may reasonably wish to take the same locks */
	if (present && wm8994->micd_cb)
		wm8994->micd_cb(wm8994->micd_cb_data);

	return IRQ_HANDLED;
}

/**
 * wm8958_mic_detect - Enable microphone detection via the WM8958 IRQ
 *
 * @codec:   WM8958 codec
 * @jack:    jack to report detection events on
 *
 * Enable microphone detection functionality for the WM8958.  By
 * default simple detection which supports the detection of up to 6
 * buttons plus video and microphone functionality is supported.
 *
 * The WM8958 has an advanced jack detection facility which is able to
 * support complex accessory detection, especially when used in
 * conjunction with external circuitry.  In order to provide maximum
 * flexiblity a callback is provided which allows a completely custom
 * detection algorithm.
 */
int wm8958_mic_detect(struct snd_soc_codec *codec, struct snd_soc_jack *jack,
			wm1811_micdet_cb det_cb, void *det_cb_data,
			wm1811_mic_id_cb id_cb, void *id_cb_data)
{
	struct wm8994_priv *wm8994 = snd_soc_codec_get_drvdata(codec);
	struct wm8994 *control = codec->control_data;
	u16 micd_lvl_sel;

	switch (control->type) {
	case WM1811:
	case WM8958:
		break;
	default:
		return -EINVAL;
	}

	if (jack) {
		snd_soc_dapm_force_enable_pin(&codec->dapm, "CLK_SYS");
		snd_soc_dapm_sync(&codec->dapm);

		wm8994->micdet[0].jack = jack;

		if (det_cb) {
			wm8994->micd_cb = det_cb;
			wm8994->micd_cb_data = det_cb_data;
		} else {
		wm8994->mic_detecting = true;
		wm8994->jack_mic = false;
		}

		if (id_cb) {
			wm8994->mic_id_cb = id_cb;
			wm8994->mic_id_cb_data = id_cb_data;
		} else {
			wm8994->mic_id_cb = wm8958_mic_id;
			wm8994->mic_id_cb_data = codec;
		}

		wm8958_micd_set_rate(codec);

		/* Detect microphones and short circuits by default */
		if (wm8994->pdata->micd_lvl_sel)
			micd_lvl_sel = wm8994->pdata->micd_lvl_sel;
		else
			micd_lvl_sel = 0x41;

		wm8994->btn_mask = SND_JACK_BTN_0 | SND_JACK_BTN_1 |
			SND_JACK_BTN_2 | SND_JACK_BTN_3 |
			SND_JACK_BTN_4 | SND_JACK_BTN_5;

		snd_soc_update_bits(codec, WM8958_MIC_DETECT_2,
				    WM8958_MICD_LVL_SEL_MASK, micd_lvl_sel);

		WARN_ON(codec->dapm.bias_level > SND_SOC_BIAS_STANDBY);

		/*
		 * If we can use jack detection start off with that,
		 * otherwise jump straight to microphone detection.
		 */
		if (wm8994->jackdet) {
			snd_soc_update_bits(codec, WM8958_MICBIAS2,
					    WM8958_MICB2_DISCH,
					    WM8958_MICB2_DISCH);
			snd_soc_update_bits(codec, WM8994_LDO_1,
					    WM8994_LDO1_DISCH, 0);
			wm1811_jackdet_set_mode(codec,
						WM1811_JACKDET_MODE_JACK);
		} else {
			snd_soc_update_bits(codec, WM8958_MIC_DETECT_1,
					    WM8958_MICD_ENA, WM8958_MICD_ENA);
		}

	} else {
		snd_soc_update_bits(codec, WM8958_MIC_DETECT_1,
				    WM8958_MICD_ENA, 0);
		wm1811_jackdet_set_mode(codec, WM1811_JACKDET_MODE_NONE);
		snd_soc_dapm_disable_pin(&codec->dapm, "CLK_SYS");
		snd_soc_dapm_sync(&codec->dapm);
	}

	return 0;
}
EXPORT_SYMBOL_GPL(wm8958_mic_detect);

static irqreturn_t wm8958_mic_irq(int irq, void *data)
{
	struct wm8994_priv *wm8994 = data;
	struct snd_soc_codec *codec = wm8994->codec;
	int reg, count, ret;

	/*
	 * Jack detection may have detected a removal simulataneously
	 * with an update of the MICDET status; if so it will have
	 * stopped detection and we can ignore this interrupt.
	 */
	if (!(snd_soc_read(codec, WM8958_MIC_DETECT_1) & WM8958_MICD_ENA))
		return IRQ_HANDLED;

	/* We may occasionally read a detection without an impedence
	 * range being provided - if that happens loop again.
	 */
	count = 10;
	do {
		reg = snd_soc_read(codec, WM8958_MIC_DETECT_3);
		if (reg < 0) {
			dev_err(codec->dev,
				"Failed to read mic detect status: %d\n",
				reg);
			return IRQ_NONE;
		}

		if (!(reg & WM8958_MICD_VALID)) {
			dev_dbg(codec->dev, "Mic detect data not valid\n");
			goto out;
		}

		if (!(reg & WM8958_MICD_STS) || (reg & WM8958_MICD_LVL_MASK))
			break;

		msleep(1);
	} while (count--);

	if (count == 0)
		dev_warn(codec->dev, "No impedence range reported for jack\n");

#ifndef CONFIG_SND_SOC_WM8994_MODULE
	trace_snd_soc_jack_irq(dev_name(codec->dev));
#endif

	/* Avoid a transient report when the accessory is being removed */
	if (wm8994->jackdet) {
		ret = snd_soc_read(codec, WM1811_JACKDET_CTRL);
		if (ret < 0) {
			dev_err(codec->dev, "Failed to read jack status: %d\n",
				ret);
		} else if (!(ret & WM1811_JACKDET_LVL)) {
			dev_dbg(codec->dev, "Ignoring removed jack\n");
			return IRQ_HANDLED;
		}
	}

	if (wm8994->mic_detecting)
		wm8994->mic_id_cb(wm8994->mic_id_cb_data, reg);
	else
		wm8958_button_det(codec, reg);

out:
	return IRQ_HANDLED;
}

static irqreturn_t wm8994_fifo_error(int irq, void *data)
{
	struct snd_soc_codec *codec = data;

	dev_err(codec->dev, "FIFO error\n");

	return IRQ_HANDLED;
}

static irqreturn_t wm8994_temp_warn(int irq, void *data)
{
	struct snd_soc_codec *codec = data;

	dev_err(codec->dev, "Thermal warning\n");

	return IRQ_HANDLED;
}

static irqreturn_t wm8994_temp_shut(int irq, void *data)
{
	struct snd_soc_codec *codec = data;

	dev_crit(codec->dev, "Thermal shutdown\n");

	return IRQ_HANDLED;
}

static int wm8994_codec_probe(struct snd_soc_codec *codec)
{
	struct wm8994 *control;
	struct wm8994_priv *wm8994;
	struct snd_soc_dapm_context *dapm = &codec->dapm;
	int ret, i;

	codec->control_data = dev_get_drvdata(codec->dev->parent);
	control = codec->control_data;

	wm8994 = kzalloc(sizeof(struct wm8994_priv), GFP_KERNEL);
	if (wm8994 == NULL)
		return -ENOMEM;
	snd_soc_codec_set_drvdata(codec, wm8994);

	wm8994->pdata = dev_get_platdata(codec->dev->parent);
	wm8994->codec = codec;

	mutex_init(&wm8994->accdet_lock);

	for (i = 0; i < ARRAY_SIZE(wm8994->fll_locked); i++)
		init_completion(&wm8994->fll_locked[i]);

	if (wm8994->pdata && wm8994->pdata->micdet_irq)
		wm8994->micdet_irq = wm8994->pdata->micdet_irq;
	else if (wm8994->pdata && wm8994->pdata->irq_base)
		wm8994->micdet_irq = wm8994->pdata->irq_base +
				     WM8994_IRQ_MIC1_DET;

#if 0 /* To do */
	pm_runtime_enable(codec->dev);
	pm_runtime_resume(codec->dev);
#endif

	/* Read our current status back from the chip - we don't want to
	 * reset as this may interfere with the GPIO or LDO operation. */
	for (i = 0; i < WM8994_CACHE_SIZE; i++) {
		if (!wm8994_readable(codec, i) || wm8994_volatile(codec, i))
			continue;

		ret = wm8994_reg_read(codec->control_data, i);
		if (ret <= 0)
			continue;

		ret = snd_soc_cache_write(codec, i, ret);
		if (ret != 0) {
			dev_err(codec->dev,
				"Failed to initialise cache for 0x%x: %d\n",
				i, ret);
			goto err;
		}
	}

	/* By default use idle_bias_off, will override for WM8994 */
	codec->dapm.idle_bias_off = 1;

	/* Set revision-specific configuration */
	wm8994->revision = snd_soc_read(codec, WM8994_CHIP_REVISION);
	switch (control->type) {
	case WM8994:
		/* Single ended line outputs should have VMID on. */
		if (!wm8994->pdata->lineout1_diff ||
		    !wm8994->pdata->lineout2_diff)
			codec->dapm.idle_bias_off = 0;

		switch (wm8994->revision) {
		case 2:
		case 3:
			wm8994->hubs.dcs_codes_l = -5;
			wm8994->hubs.dcs_codes_r = -5;
			wm8994->hubs.hp_startup_mode = 1;
			wm8994->hubs.dcs_readback_mode = 1;
			wm8994->hubs.series_startup = 1;
			break;
		default:
			wm8994->hubs.dcs_readback_mode = 2;
			break;
		}
		break;

	case WM8958:
		wm8994->hubs.dcs_readback_mode = 1;
		wm8994->hubs.hp_startup_mode = 1;

		switch (wm8994->revision) {
		case 0:
			break;
		default:
			wm8994->fll_byp = true;
			break;
		}
		break;

	case WM1811:
		wm8994->hubs.dcs_readback_mode = 2;
		wm8994->hubs.no_series_update = 1;
		wm8994->hubs.hp_startup_mode = 1;
		wm8994->hubs.no_cache_dac_hp_direct = true;
		wm8994->fll_byp = true;
		wm8994->hubs.dcs_codes_l = -11;
		wm8994->hubs.dcs_codes_r = -9;

		snd_soc_update_bits(codec, WM8994_ANALOGUE_HP_1,
				    WM1811_HPOUT1_ATTN, WM1811_HPOUT1_ATTN);
		break;

	default:
		break;
	}

#if 0 /* To do */
	wm8994_request_irq(codec->control_data, WM8994_IRQ_FIFOS_ERR,
			   wm8994_fifo_error, "FIFO error", codec);
	wm8994_request_irq(codec->control_data, WM8994_IRQ_TEMP_WARN,
			   wm8994_temp_warn, "Thermal warning", codec);
	wm8994_request_irq(codec->control_data, WM8994_IRQ_TEMP_SHUT,
			   wm8994_temp_shut, "Thermal shutdown", codec);

	ret = wm8994_request_irq(codec->control_data, WM8994_IRQ_DCS_DONE,
				 wm_hubs_dcs_done, "DC servo done",
				 &wm8994->hubs);
	if (ret == 0)
		wm8994->hubs.dcs_done_irq = true;
#endif

	switch (control->type) {
	case WM8994:
		if (wm8994->micdet_irq) {
			ret = request_threaded_irq(wm8994->micdet_irq, NULL,
						   wm8994_mic_irq,
						   IRQF_TRIGGER_RISING,
						   "Mic1 detect",
						   wm8994);
			if (ret != 0)
				dev_warn(codec->dev,
					 "Failed to request Mic1 detect IRQ: %d\n",
					 ret);
		}

		ret = wm8994_request_irq(codec->control_data,
					 WM8994_IRQ_MIC1_SHRT,
					 wm8994_mic_irq, "Mic 1 short",
					 wm8994);
		if (ret != 0)
			dev_warn(codec->dev,
				 "Failed to request Mic1 short IRQ: %d\n",
				 ret);

		ret = wm8994_request_irq(codec->control_data,
					 WM8994_IRQ_MIC2_DET,
					 wm8994_mic_irq, "Mic 2 detect",
					 wm8994);
		if (ret != 0)
			dev_warn(codec->dev,
				 "Failed to request Mic2 detect IRQ: %d\n",
				 ret);

		ret = wm8994_request_irq(codec->control_data,
					 WM8994_IRQ_MIC2_SHRT,
					 wm8994_mic_irq, "Mic 2 short",
					 wm8994);
		if (ret != 0)
			dev_warn(codec->dev,
				 "Failed to request Mic2 short IRQ: %d\n",
				 ret);
		break;

	case WM8958:
	case WM1811:
		if (wm8994->micdet_irq) {
			ret = request_threaded_irq(wm8994->micdet_irq, NULL,
						   wm8958_mic_irq,
						   IRQF_TRIGGER_RISING,
						   "Mic detect",
						   wm8994);
			if (ret != 0) {
				dev_warn(codec->dev,
					 "Failed to request Mic detect IRQ: %d\n",
					 ret);
			}
		}
	}

	switch (control->type) {
	case WM1811:
		if (wm8994->revision > 1) {
			ret = wm8994_request_irq(codec->control_data,
						 WM8994_IRQ_GPIO(6),
						 wm1811_jackdet_irq, "JACKDET",
						 wm8994);
			if (ret == 0) {
				wm8994->jackdet = true;
			} else {
				dev_warn(codec->dev,
					 "Failed to request Jack detect IRQ: %d\n",
					 ret);
			}
		}
		break;
	default:
		break;
	}

#if 0 /* To do */
	wm8994->fll_locked_irq = true;
	for (i = 0; i < ARRAY_SIZE(wm8994->fll_locked); i++) {
		ret = wm8994_request_irq(codec->control_data,
					 WM8994_IRQ_FLL1_LOCK + i,
					 wm8994_fll_locked_irq, "FLL lock",
					 &wm8994->fll_locked[i]);
		if (ret != 0)
			wm8994->fll_locked_irq = false;
	}
#endif

	/* Remember if AIFnLRCLK is configured as a GPIO.  This should be
	 * configured on init - if a system wants to do this dynamically
	 * at runtime we can deal with that then.
	 */
	ret = wm8994_reg_read(codec->control_data, WM8994_GPIO_1);
	if (ret < 0) {
		dev_err(codec->dev, "Failed to read GPIO1 state: %d\n", ret);
		goto err_irq;
	}
	if ((ret & WM8994_GPN_FN_MASK) != WM8994_GP_FN_PIN_SPECIFIC) {
		wm8994->lrclk_shared[0] = 1;
		wm8994_dai[0].symmetric_rates = 1;
	} else {
		wm8994->lrclk_shared[0] = 0;
	}

	ret = wm8994_reg_read(codec->control_data, WM8994_GPIO_6);
	if (ret < 0) {
		dev_err(codec->dev, "Failed to read GPIO6 state: %d\n", ret);
		goto err_irq;
	}
	if ((ret & WM8994_GPN_FN_MASK) != WM8994_GP_FN_PIN_SPECIFIC) {
		wm8994->lrclk_shared[1] = 1;
		wm8994_dai[1].symmetric_rates = 1;
	} else {
		wm8994->lrclk_shared[1] = 0;
	}

	wm8994_set_bias_level(codec, SND_SOC_BIAS_STANDBY);

	/* Latch volume updates (right only; we always do left then right). */
	snd_soc_update_bits(codec, WM8994_AIF1_DAC1_LEFT_VOLUME,
			    WM8994_AIF1DAC1_VU, WM8994_AIF1DAC1_VU);
	snd_soc_update_bits(codec, WM8994_AIF1_DAC1_RIGHT_VOLUME,
			    WM8994_AIF1DAC1_VU, WM8994_AIF1DAC1_VU);
	snd_soc_update_bits(codec, WM8994_AIF1_DAC2_LEFT_VOLUME,
			    WM8994_AIF1DAC2_VU, WM8994_AIF1DAC2_VU);
	snd_soc_update_bits(codec, WM8994_AIF1_DAC2_RIGHT_VOLUME,
			    WM8994_AIF1DAC2_VU, WM8994_AIF1DAC2_VU);
	snd_soc_update_bits(codec, WM8994_AIF2_DAC_LEFT_VOLUME,
			    WM8994_AIF2DAC_VU, WM8994_AIF2DAC_VU);
	snd_soc_update_bits(codec, WM8994_AIF2_DAC_RIGHT_VOLUME,
			    WM8994_AIF2DAC_VU, WM8994_AIF2DAC_VU);
	snd_soc_update_bits(codec, WM8994_AIF1_ADC1_LEFT_VOLUME,
			    WM8994_AIF1ADC1_VU, WM8994_AIF1ADC1_VU);
	snd_soc_update_bits(codec, WM8994_AIF1_ADC1_RIGHT_VOLUME,
			    WM8994_AIF1ADC1_VU, WM8994_AIF1ADC1_VU);
	snd_soc_update_bits(codec, WM8994_AIF1_ADC2_LEFT_VOLUME,
			    WM8994_AIF1ADC2_VU, WM8994_AIF1ADC2_VU);
	snd_soc_update_bits(codec, WM8994_AIF1_ADC2_RIGHT_VOLUME,
			    WM8994_AIF1ADC2_VU, WM8994_AIF1ADC2_VU);
	snd_soc_update_bits(codec, WM8994_AIF2_ADC_LEFT_VOLUME,
			    WM8994_AIF2ADC_VU, WM8994_AIF1ADC2_VU);
	snd_soc_update_bits(codec, WM8994_AIF2_ADC_RIGHT_VOLUME,
			    WM8994_AIF2ADC_VU, WM8994_AIF1ADC2_VU);
	snd_soc_update_bits(codec, WM8994_DAC1_LEFT_VOLUME,
			    WM8994_DAC1_VU, WM8994_DAC1_VU);
	snd_soc_update_bits(codec, WM8994_DAC1_RIGHT_VOLUME,
			    WM8994_DAC1_VU, WM8994_DAC1_VU);
	snd_soc_update_bits(codec, WM8994_DAC2_LEFT_VOLUME,
			    WM8994_DAC2_VU, WM8994_DAC2_VU);
	snd_soc_update_bits(codec, WM8994_DAC2_RIGHT_VOLUME,
			    WM8994_DAC2_VU, WM8994_DAC2_VU);

	/* Set the low bit of the 3D stereo depth so TLV matches */
	snd_soc_update_bits(codec, WM8994_AIF1_DAC1_FILTERS_2,
			    1 << WM8994_AIF1DAC1_3D_GAIN_SHIFT,
			    1 << WM8994_AIF1DAC1_3D_GAIN_SHIFT);
	snd_soc_update_bits(codec, WM8994_AIF1_DAC2_FILTERS_2,
			    1 << WM8994_AIF1DAC2_3D_GAIN_SHIFT,
			    1 << WM8994_AIF1DAC2_3D_GAIN_SHIFT);
	snd_soc_update_bits(codec, WM8994_AIF2_DAC_FILTERS_2,
			    1 << WM8994_AIF2DAC_3D_GAIN_SHIFT,
			    1 << WM8994_AIF2DAC_3D_GAIN_SHIFT);

	/* Unconditionally enable AIF1 ADC TDM mode on chips which can
	 * use this; it only affects behaviour on idle TDM clock
	 * cycles. */
	switch (control->type) {
	case WM8994:
	case WM8958:
		snd_soc_update_bits(codec, WM8994_AIF1_CONTROL_1,
				    WM8994_AIF1ADC_TDM, WM8994_AIF1ADC_TDM);
		break;
	default:
		break;
	}

	/* Put MICBIAS into bypass mode by default on newer devices */
	switch (control->type) {
	case WM8958:
	case WM1811:
		snd_soc_update_bits(codec, WM8958_MICBIAS1,
				    WM8958_MICB1_MODE, WM8958_MICB1_MODE);
		snd_soc_update_bits(codec, WM8958_MICBIAS2,
				    WM8958_MICB2_MODE, WM8958_MICB2_MODE);
		break;
	default:
		break;
	}

	wm8994->hubs.check_class_w_digital = wm8994_check_class_w_digital;
	wm_hubs_update_class_w(codec);

	wm8994_handle_pdata(wm8994);

	wm_hubs_add_analogue_controls(codec);
	snd_soc_add_controls(codec, wm8994_snd_controls,
			     ARRAY_SIZE(wm8994_snd_controls));
	snd_soc_dapm_new_controls(dapm, wm8994_dapm_widgets,
				  ARRAY_SIZE(wm8994_dapm_widgets));

	switch (control->type) {
	case WM8994:
		snd_soc_dapm_new_controls(dapm, wm8994_specific_dapm_widgets,
					  ARRAY_SIZE(wm8994_specific_dapm_widgets));
		if (wm8994->revision < 4) {
			snd_soc_dapm_new_controls(dapm, wm8994_lateclk_revd_widgets,
						  ARRAY_SIZE(wm8994_lateclk_revd_widgets));
			snd_soc_dapm_new_controls(dapm, wm8994_adc_revd_widgets,
						  ARRAY_SIZE(wm8994_adc_revd_widgets));
			snd_soc_dapm_new_controls(dapm, wm8994_dac_revd_widgets,
						  ARRAY_SIZE(wm8994_dac_revd_widgets));
		} else {
			snd_soc_dapm_new_controls(dapm, wm8994_lateclk_widgets,
						  ARRAY_SIZE(wm8994_lateclk_widgets));
			snd_soc_dapm_new_controls(dapm, wm8994_adc_widgets,
						  ARRAY_SIZE(wm8994_adc_widgets));
			snd_soc_dapm_new_controls(dapm, wm8994_dac_widgets,
						  ARRAY_SIZE(wm8994_dac_widgets));
		}
		break;
	case WM8958:
		snd_soc_add_controls(codec, wm8958_snd_controls,
				     ARRAY_SIZE(wm8958_snd_controls));
		snd_soc_dapm_new_controls(dapm, wm8958_dapm_widgets,
					  ARRAY_SIZE(wm8958_dapm_widgets));
		if (wm8994->revision < 1) {
			snd_soc_dapm_new_controls(dapm, wm8994_lateclk_revd_widgets,
						  ARRAY_SIZE(wm8994_lateclk_revd_widgets));
			snd_soc_dapm_new_controls(dapm, wm8994_adc_revd_widgets,
						  ARRAY_SIZE(wm8994_adc_revd_widgets));
			snd_soc_dapm_new_controls(dapm, wm8994_dac_revd_widgets,
						  ARRAY_SIZE(wm8994_dac_revd_widgets));
		} else {
			snd_soc_dapm_new_controls(dapm, wm8994_lateclk_widgets,
						  ARRAY_SIZE(wm8994_lateclk_widgets));
			snd_soc_dapm_new_controls(dapm, wm8994_adc_widgets,
						  ARRAY_SIZE(wm8994_adc_widgets));
			snd_soc_dapm_new_controls(dapm, wm8994_dac_widgets,
						  ARRAY_SIZE(wm8994_dac_widgets));
		}
		break;

	case WM1811:
		snd_soc_add_controls(codec, wm8958_snd_controls,
				     ARRAY_SIZE(wm8958_snd_controls));
		snd_soc_dapm_new_controls(dapm, wm8958_dapm_widgets,
					  ARRAY_SIZE(wm8958_dapm_widgets));
		snd_soc_dapm_new_controls(dapm, wm8994_lateclk_widgets,
					  ARRAY_SIZE(wm8994_lateclk_widgets));
		snd_soc_dapm_new_controls(dapm, wm8994_adc_widgets,
					  ARRAY_SIZE(wm8994_adc_widgets));
		snd_soc_dapm_new_controls(dapm, wm8994_dac_widgets,
					  ARRAY_SIZE(wm8994_dac_widgets));
		break;
	}


	wm_hubs_add_analogue_routes(codec, 0, 0);
	snd_soc_dapm_add_routes(dapm, intercon, ARRAY_SIZE(intercon));

	switch (control->type) {
	case WM8994:
		snd_soc_dapm_add_routes(dapm, wm8994_intercon,
					ARRAY_SIZE(wm8994_intercon));

		if (wm8994->revision < 4) {
			snd_soc_dapm_add_routes(dapm, wm8994_revd_intercon,
						ARRAY_SIZE(wm8994_revd_intercon));
			snd_soc_dapm_add_routes(dapm, wm8994_lateclk_revd_intercon,
						ARRAY_SIZE(wm8994_lateclk_revd_intercon));
		} else {
			snd_soc_dapm_add_routes(dapm, wm8994_lateclk_intercon,
						ARRAY_SIZE(wm8994_lateclk_intercon));
		}
		break;
	case WM8958:
		if (wm8994->revision < 1) {
			snd_soc_dapm_add_routes(dapm, wm8994_revd_intercon,
						ARRAY_SIZE(wm8994_revd_intercon));
			snd_soc_dapm_add_routes(dapm, wm8994_lateclk_revd_intercon,
						ARRAY_SIZE(wm8994_lateclk_revd_intercon));
		} else {
			snd_soc_dapm_add_routes(dapm, wm8994_lateclk_intercon,
						ARRAY_SIZE(wm8994_lateclk_intercon));
			snd_soc_dapm_add_routes(dapm, wm8958_intercon,
						ARRAY_SIZE(wm8958_intercon));
		}

		wm8958_dsp2_init(codec);
		break;
	case WM1811:
		snd_soc_dapm_add_routes(dapm, wm8994_lateclk_intercon,
					ARRAY_SIZE(wm8994_lateclk_intercon));
		snd_soc_dapm_add_routes(dapm, wm8958_intercon,
					ARRAY_SIZE(wm8958_intercon));
		break;
	}

	Boeffla_sound_hook_wm8994_pcm_probe(codec);

	return 0;

err_irq:
	if (wm8994->jackdet)
		wm8994_free_irq(wm8994->control_data, WM8994_IRQ_GPIO(6),
				wm8994);
	wm8994_free_irq(codec->control_data, WM8994_IRQ_MIC2_SHRT, wm8994);
	wm8994_free_irq(codec->control_data, WM8994_IRQ_MIC2_DET, wm8994);
	wm8994_free_irq(codec->control_data, WM8994_IRQ_MIC1_SHRT, wm8994);
	if (wm8994->micdet_irq)
		free_irq(wm8994->micdet_irq, wm8994);
	for (i = 0; i < ARRAY_SIZE(wm8994->fll_locked); i++)
		wm8994_free_irq(codec->control_data, WM8994_IRQ_FLL1_LOCK + i,
				&wm8994->fll_locked[i]);
	wm8994_free_irq(codec->control_data, WM8994_IRQ_DCS_DONE,
			&wm8994->hubs);
	wm8994_free_irq(codec->control_data, WM8994_IRQ_FIFOS_ERR, codec);
	wm8994_free_irq(codec->control_data, WM8994_IRQ_TEMP_SHUT, codec);
	wm8994_free_irq(codec->control_data, WM8994_IRQ_TEMP_WARN, codec);
err:
	kfree(wm8994);
	return ret;
}

static int  wm8994_codec_remove(struct snd_soc_codec *codec)
{
	struct wm8994_priv *wm8994 = snd_soc_codec_get_drvdata(codec);
	struct wm8994 *control = codec->control_data;
	int i;

	wm8994_set_bias_level(codec, SND_SOC_BIAS_OFF);

#if 0 /* To do */
	pm_runtime_disable(codec->dev);
#endif

	for (i = 0; i < ARRAY_SIZE(wm8994->fll_locked); i++)
		wm8994_free_irq(codec->control_data, WM8994_IRQ_FLL1_LOCK + i,
				&wm8994->fll_locked[i]);

	wm8994_free_irq(codec->control_data, WM8994_IRQ_DCS_DONE,
			&wm8994->hubs);
	wm8994_free_irq(codec->control_data, WM8994_IRQ_FIFOS_ERR, codec);
	wm8994_free_irq(codec->control_data, WM8994_IRQ_TEMP_SHUT, codec);
	wm8994_free_irq(codec->control_data, WM8994_IRQ_TEMP_WARN, codec);

	if (wm8994->jackdet)
		wm8994_free_irq(wm8994->control_data, WM8994_IRQ_GPIO(6),
				wm8994);

	switch (control->type) {
	case WM8994:
		if (wm8994->micdet_irq)
			free_irq(wm8994->micdet_irq, wm8994);
		wm8994_free_irq(codec->control_data, WM8994_IRQ_MIC2_DET,
				wm8994);
		wm8994_free_irq(codec->control_data, WM8994_IRQ_MIC1_SHRT,
				wm8994);
		wm8994_free_irq(codec->control_data, WM8994_IRQ_MIC1_DET,
				wm8994);
		break;

	case WM1811:
	case WM8958:
		if (wm8994->micdet_irq)
			free_irq(wm8994->micdet_irq, wm8994);
		break;
	}
	if (wm8994->mbc)
		release_firmware(wm8994->mbc);
	if (wm8994->mbc_vss)
		release_firmware(wm8994->mbc_vss);
	if (wm8994->enh_eq)
		release_firmware(wm8994->enh_eq);
	kfree(wm8994->retune_mobile_texts);
	kfree(wm8994->drc_texts);
	kfree(wm8994);

	return 0;
}

static struct snd_soc_codec_driver soc_codec_dev_wm8994 = {
	.probe =	wm8994_codec_probe,
	.remove =	wm8994_codec_remove,
	.suspend =	wm8994_codec_suspend,
	.resume =	wm8994_codec_resume,
	.read =		wm8994_read,
	.write =	wm8994_write,
	.readable_register = wm8994_readable,
	.volatile_register = wm8994_volatile,
	.set_bias_level = wm8994_set_bias_level,

	.max_register = WM8994_MAX_REGISTER,
	.reg_cache_size = WM8994_CACHE_SIZE,
	.reg_cache_default = wm8994_reg_defaults,
	.reg_word_size = 2,
	.compress_type = SND_SOC_RBTREE_COMPRESSION,
};

static int __devinit wm8994_probe(struct platform_device *pdev)
{
	return snd_soc_register_codec(&pdev->dev, &soc_codec_dev_wm8994,
			wm8994_dai, ARRAY_SIZE(wm8994_dai));
}

static int __devexit wm8994_remove(struct platform_device *pdev)
{
	snd_soc_unregister_codec(&pdev->dev);
	return 0;
}

#ifdef CONFIG_PM_SLEEP
static int wm8994_suspend(struct device *dev)
{
	struct wm8994_priv *wm8994 = dev_get_drvdata(dev);

	/* Drop down to power saving mode when system is suspended */
	if (wm8994->jackdet && !wm8994->active_refcount)
		snd_soc_update_bits(wm8994->codec, WM8994_ANTIPOP_2,
				    WM1811_JACKDET_MODE_MASK,
				    wm8994->jackdet_mode);

	return 0;
}

static int wm8994_resume(struct device *dev)
{
	struct wm8994_priv *wm8994 = dev_get_drvdata(dev);

	if (wm8994->jackdet && wm8994->jackdet_mode) {
		snd_soc_update_bits(wm8994->codec, WM8994_ANTIPOP_2,
				    WM1811_JACKDET_MODE_MASK,
				    WM1811_JACKDET_MODE_AUDIO);
		msleep(2);
	}

	return 0;
}
#endif

static struct dev_pm_ops wm8994_pm_ops = {
	SET_SYSTEM_SLEEP_PM_OPS(wm8994_suspend, wm8994_resume)
};

static struct platform_driver wm8994_codec_driver = {
	.driver = {
		.name = "wm8994-codec",
		.owner = THIS_MODULE,
		.pm = &wm8994_pm_ops,
	},
	.probe = wm8994_probe,
	.remove = __devexit_p(wm8994_remove),
};

static __init int wm8994_init(void)
{
	return platform_driver_register(&wm8994_codec_driver);
}
module_init(wm8994_init);

static __exit void wm8994_exit(void)
{
	platform_driver_unregister(&wm8994_codec_driver);
}
module_exit(wm8994_exit);


MODULE_DESCRIPTION("ASoC WM8994 driver");
MODULE_AUTHOR("Mark Brown <broonie@opensource.wolfsonmicro.com>");
MODULE_LICENSE("GPL");
MODULE_ALIAS("platform:wm8994-codec");