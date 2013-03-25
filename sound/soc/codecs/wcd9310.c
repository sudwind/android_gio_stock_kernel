/* Copyright (c) 2011, Code Aurora Forum. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */
#include <linux/module.h>
#include <linux/init.h>
#include <linux/firmware.h>
#include <linux/slab.h>
#include <linux/platform_device.h>
#include <linux/printk.h>
#include <linux/ratelimit.h>
#include <linux/debugfs.h>
#include <linux/mfd/wcd9310/core.h>
#include <linux/mfd/wcd9310/registers.h>
#include <linux/mfd/wcd9310/pdata.h>
#include <sound/jack.h>
#include <sound/soc.h>
#include <sound/soc-dapm.h>
#include <sound/tlv.h>
#include <linux/bitops.h>
#include <linux/delay.h>
#include "wcd9310.h"

#define WCD9310_RATES (SNDRV_PCM_RATE_8000|SNDRV_PCM_RATE_16000|\
			SNDRV_PCM_RATE_32000|SNDRV_PCM_RATE_48000)

#define NUM_DECIMATORS 10
#define NUM_INTERPOLATORS 7
#define BITS_PER_REG 8
#define TABLA_RX_DAI_ID 1
#define TABLA_TX_DAI_ID 2

static const DECLARE_TLV_DB_SCALE(digital_gain, 0, 1, 0);
static const DECLARE_TLV_DB_SCALE(line_gain, 0, 7, 1);
static const DECLARE_TLV_DB_SCALE(analog_gain, 0, 25, 1);

enum tabla_bandgap_type {
	TABLA_BANDGAP_OFF = 0,
	TABLA_BANDGAP_AUDIO_MODE,
	TABLA_BANDGAP_MBHC_MODE,
};

struct tabla_priv {
	struct snd_soc_codec *codec;
	u32 adc_count;
	u32 cfilt1_cnt;
	u32 cfilt2_cnt;
	u32 cfilt3_cnt;
	u32 rx_bias_count;
	enum tabla_bandgap_type bandgap_type;
	bool mclk_enabled;
	bool clock_active;
	bool config_mode_active;
	bool mbhc_polling_active;
	int buttons_pressed;

	struct tabla_mbhc_calibration *calibration;

	struct snd_soc_jack *headset_jack;
	struct snd_soc_jack *button_jack;

	struct tabla_pdata *pdata;
	u32 anc_slot;

	bool no_mic_headset_override;
	/* Delayed work to report long button press */
	struct delayed_work btn0_dwork;
};

#ifdef CONFIG_DEBUG_FS
struct tabla_priv *debug_tabla_priv;
#endif

static int tabla_codec_enable_charge_pump(struct snd_soc_dapm_widget *w,
		struct snd_kcontrol *kcontrol, int event)
{
	struct snd_soc_codec *codec = w->codec;

	pr_debug("%s %d\n", __func__, event);
	switch (event) {
	case SND_SOC_DAPM_POST_PMU:
		snd_soc_update_bits(codec, TABLA_A_CDC_CLK_OTHR_CTL, 0x01,
			0x01);
		snd_soc_update_bits(codec, TABLA_A_CDC_CLSG_CTL, 0x08, 0x08);
		usleep_range(200, 200);
		snd_soc_update_bits(codec, TABLA_A_CP_STATIC, 0x10, 0x00);
		break;
	case SND_SOC_DAPM_PRE_PMD:
		snd_soc_update_bits(codec, TABLA_A_CDC_CLK_OTHR_RESET_CTL, 0x10,
			0x10);
		usleep_range(20, 20);
		snd_soc_update_bits(codec, TABLA_A_CP_STATIC, 0x08, 0x08);
		snd_soc_update_bits(codec, TABLA_A_CP_STATIC, 0x10, 0x10);
		snd_soc_update_bits(codec, TABLA_A_CDC_CLSG_CTL, 0x08, 0x00);
		snd_soc_update_bits(codec, TABLA_A_CDC_CLK_OTHR_CTL, 0x01,
			0x00);
		snd_soc_update_bits(codec, TABLA_A_CP_STATIC, 0x08, 0x00);
		break;
	}
	return 0;
}

static int tabla_get_anc_slot(struct snd_kcontrol *kcontrol,
	struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_codec *codec = snd_kcontrol_chip(kcontrol);
	struct tabla_priv *tabla = snd_soc_codec_get_drvdata(codec);
	ucontrol->value.integer.value[0] = tabla->anc_slot;
	return 0;
}

static int tabla_put_anc_slot(struct snd_kcontrol *kcontrol,
	struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_codec *codec = snd_kcontrol_chip(kcontrol);
	struct tabla_priv *tabla = snd_soc_codec_get_drvdata(codec);
	tabla->anc_slot = ucontrol->value.integer.value[0];
	return 0;
}

static int tabla_pa_gain_get(struct snd_kcontrol *kcontrol,
				struct snd_ctl_elem_value *ucontrol)
{
	u8 ear_pa_gain;
	struct snd_soc_codec *codec = snd_kcontrol_chip(kcontrol);

	ear_pa_gain = snd_soc_read(codec, TABLA_A_RX_EAR_GAIN);

	ear_pa_gain = ear_pa_gain >> 5;

	if (ear_pa_gain == 0x00) {
		ucontrol->value.integer.value[0] = 0;
	} else if (ear_pa_gain == 0x04) {
		ucontrol->value.integer.value[0] = 1;
	} else  {
		pr_err("%s: ERROR: Unsupported Ear Gain = 0x%x\n",
				__func__, ear_pa_gain);
		return -EINVAL;
	}

	pr_debug("%s: ear_pa_gain = 0x%x\n", __func__, ear_pa_gain);

	return 0;
}

static int tabla_pa_gain_put(struct snd_kcontrol *kcontrol,
				struct snd_ctl_elem_value *ucontrol)
{
	u8 ear_pa_gain;
	struct snd_soc_codec *codec = snd_kcontrol_chip(kcontrol);

	pr_debug("%s: ucontrol->value.integer.value[0]  = %ld\n", __func__,
			ucontrol->value.integer.value[0]);

	switch (ucontrol->value.integer.value[0]) {
	case 0:
		ear_pa_gain = 0x00;
		break;
	case 1:
		ear_pa_gain = 0x80;
		break;
	default:
		return -EINVAL;
	}

	snd_soc_update_bits(codec, TABLA_A_RX_EAR_GAIN, 0xE0, ear_pa_gain);
	return 0;
}

static const char *tabla_ear_pa_gain_text[] = {"POS_6_DB", "POS_2_DB"};
static const struct soc_enum tabla_ear_pa_gain_enum[] = {
		SOC_ENUM_SINGLE_EXT(2, tabla_ear_pa_gain_text),
};

static const struct snd_kcontrol_new tabla_snd_controls[] = {

	SOC_ENUM_EXT("EAR PA Gain", tabla_ear_pa_gain_enum[0],
		tabla_pa_gain_get, tabla_pa_gain_put),

	SOC_SINGLE_TLV("LINEOUT1 Volume", TABLA_A_RX_LINE_1_GAIN, 0, 12, 1,
		line_gain),
	SOC_SINGLE_TLV("LINEOUT2 Volume", TABLA_A_RX_LINE_2_GAIN, 0, 12, 1,
		line_gain),
	SOC_SINGLE_TLV("LINEOUT3 Volume", TABLA_A_RX_LINE_3_GAIN, 0, 12, 1,
		line_gain),
	SOC_SINGLE_TLV("LINEOUT4 Volume", TABLA_A_RX_LINE_4_GAIN, 0, 12, 1,
		line_gain),
	SOC_SINGLE_TLV("LINEOUT5 Volume", TABLA_A_RX_LINE_5_GAIN, 0, 12, 1,
		line_gain),

	SOC_SINGLE("RX1 CHAIN INVERT Switch", TABLA_A_CDC_RX1_B6_CTL, 4, 1, 0),
	SOC_SINGLE("RX2 CHAIN INVERT Switch", TABLA_A_CDC_RX2_B6_CTL, 4, 1, 0),
	SOC_SINGLE("RX3 CHAIN INVERT Switch", TABLA_A_CDC_RX3_B6_CTL, 4, 1, 0),
	SOC_SINGLE("RX4 CHAIN INVERT Switch", TABLA_A_CDC_RX4_B6_CTL, 4, 1, 0),
	SOC_SINGLE("RX5 CHAIN INVERT Switch", TABLA_A_CDC_RX5_B6_CTL, 4, 1, 0),
	SOC_SINGLE("RX6 CHAIN INVERT Switch", TABLA_A_CDC_RX6_B6_CTL, 4, 1, 0),

	SOC_SINGLE_TLV("HPHL Volume", TABLA_A_RX_HPH_L_GAIN, 0, 12, 1,
		line_gain),
	SOC_SINGLE_TLV("HPHR Volume", TABLA_A_RX_HPH_R_GAIN, 0, 12, 1,
		line_gain),

	SOC_SINGLE_S8_TLV("RX1 Digital Volume", TABLA_A_CDC_RX1_VOL_CTL_B2_CTL,
		-84, 40, digital_gain),
	SOC_SINGLE_S8_TLV("RX2 Digital Volume", TABLA_A_CDC_RX2_VOL_CTL_B2_CTL,
		-84, 40, digital_gain),
	SOC_SINGLE_S8_TLV("RX3 Digital Volume", TABLA_A_CDC_RX3_VOL_CTL_B2_CTL,
		-84, 40, digital_gain),
	SOC_SINGLE_S8_TLV("RX4 Digital Volume", TABLA_A_CDC_RX4_VOL_CTL_B2_CTL,
		-84, 40, digital_gain),
	SOC_SINGLE_S8_TLV("RX5 Digital Volume", TABLA_A_CDC_RX5_VOL_CTL_B2_CTL,
		-84, 40, digital_gain),
	SOC_SINGLE_S8_TLV("RX6 Digital Volume", TABLA_A_CDC_RX6_VOL_CTL_B2_CTL,
		-84, 40, digital_gain),

	SOC_SINGLE_S8_TLV("DEC1 Volume", TABLA_A_CDC_TX1_VOL_CTL_GAIN, -84, 40,
		digital_gain),
	SOC_SINGLE_S8_TLV("DEC2 Volume", TABLA_A_CDC_TX2_VOL_CTL_GAIN, -84, 40,
		digital_gain),
	SOC_SINGLE_S8_TLV("DEC3 Volume", TABLA_A_CDC_TX3_VOL_CTL_GAIN, -84, 40,
		digital_gain),
	SOC_SINGLE_S8_TLV("DEC4 Volume", TABLA_A_CDC_TX4_VOL_CTL_GAIN, -84, 40,
		digital_gain),
	SOC_SINGLE_S8_TLV("DEC5 Volume", TABLA_A_CDC_TX5_VOL_CTL_GAIN, -84, 40,
		digital_gain),
	SOC_SINGLE_S8_TLV("DEC6 Volume", TABLA_A_CDC_TX6_VOL_CTL_GAIN, -84, 40,
		digital_gain),
	SOC_SINGLE_S8_TLV("DEC7 Volume", TABLA_A_CDC_TX7_VOL_CTL_GAIN, -84, 40,
		digital_gain),
	SOC_SINGLE_S8_TLV("DEC8 Volume", TABLA_A_CDC_TX8_VOL_CTL_GAIN, -84, 40,
		digital_gain),
	SOC_SINGLE_S8_TLV("DEC9 Volume", TABLA_A_CDC_TX9_VOL_CTL_GAIN, -84, 40,
		digital_gain),
	SOC_SINGLE_S8_TLV("DEC10 Volume", TABLA_A_CDC_TX10_VOL_CTL_GAIN, -84,
		40, digital_gain),

	SOC_SINGLE_TLV("ADC1 Volume", TABLA_A_TX_1_2_EN, 5, 3, 0, analog_gain),
	SOC_SINGLE_TLV("ADC2 Volume", TABLA_A_TX_1_2_EN, 1, 3, 0, analog_gain),
	SOC_SINGLE_TLV("ADC3 Volume", TABLA_A_TX_3_4_EN, 5, 3, 0, analog_gain),
	SOC_SINGLE_TLV("ADC4 Volume", TABLA_A_TX_3_4_EN, 1, 3, 0, analog_gain),
	SOC_SINGLE_TLV("ADC5 Volume", TABLA_A_TX_5_6_EN, 5, 3, 0, analog_gain),
	SOC_SINGLE_TLV("ADC6 Volume", TABLA_A_TX_5_6_EN, 1, 3, 0, analog_gain),

	SOC_SINGLE("MICBIAS1 CAPLESS Switch", TABLA_A_MICB_1_CTL, 4, 1, 1),
	SOC_SINGLE("MICBIAS3 CAPLESS Switch", TABLA_A_MICB_3_CTL, 4, 1, 1),
	SOC_SINGLE("MICBIAS4 CAPLESS Switch", TABLA_A_MICB_4_CTL, 4, 1, 1),

	SOC_SINGLE_EXT("ANC Slot", SND_SOC_NOPM, 0, 0, 100, tabla_get_anc_slot,
		tabla_put_anc_slot),
};

static const char *rx_mix1_text[] = {
	"ZERO", "SRC1", "SRC2", "IIR1", "IIR2", "RX1", "RX2", "RX3", "RX4",
		"RX5", "RX6", "RX7"
};

static const char *sb_tx1_mux_text[] = {
	"ZERO", "RMIX1", "RMIX2", "RMIX3", "RMIX4", "RMIX5", "RMIX6", "RMIX7",
		"DEC1"
};

static const char *sb_tx5_mux_text[] = {
	"ZERO", "RMIX1", "RMIX2", "RMIX3", "RMIX4", "RMIX5", "RMIX6", "RMIX7",
		"DEC5"
};

static const char *sb_tx6_mux_text[] = {
	"ZERO", "RMIX1", "RMIX2", "RMIX3", "RMIX4", "RMIX5", "RMIX6", "RMIX7",
		"DEC6"
};

static const char const *sb_tx7_to_tx10_mux_text[] = {
	"ZERO", "RMIX1", "RMIX2", "RMIX3", "RMIX4", "RMIX5", "RMIX6", "RMIX7",
		"DEC1", "DEC2", "DEC3", "DEC4", "DEC5", "DEC6", "DEC7", "DEC8",
		"DEC9", "DEC10"
};

static const char *dec1_mux_text[] = {
	"ZERO", "DMIC1", "ADC6",
};

static const char *dec2_mux_text[] = {
	"ZERO", "DMIC2", "ADC5",
};

static const char *dec3_mux_text[] = {
	"ZERO", "DMIC3", "ADC4",
};

static const char *dec4_mux_text[] = {
	"ZERO", "DMIC4", "ADC3",
};

static const char *dec5_mux_text[] = {
	"ZERO", "DMIC5", "ADC2",
};

static const char *dec6_mux_text[] = {
	"ZERO", "DMIC6", "ADC1",
};

static const char const *dec7_mux_text[] = {
	"ZERO", "DMIC1", "DMIC6", "ADC1", "ADC6", "ANC1_FB", "ANC2_FB",
};

static const char *dec8_mux_text[] = {
	"ZERO", "DMIC2", "DMIC5", "ADC2", "ADC5",
};

static const char *dec9_mux_text[] = {
	"ZERO", "DMIC4", "DMIC5", "ADC2", "ADC3", "ADCMB", "ANC1_FB", "ANC2_FB",
};

static const char *dec10_mux_text[] = {
	"ZERO", "DMIC3", "DMIC6", "ADC1", "ADC4", "ADCMB", "ANC1_FB", "ANC2_FB",
};

static const char const *anc_mux_text[] = {
	"ZERO", "ADC1", "ADC2", "ADC3", "ADC4", "ADC5", "ADC6", "ADC_MB",
		"RSVD_1", "DMIC1", "DMIC2", "DMIC3", "DMIC4", "DMIC5", "DMIC6"
};

static const char const *anc1_fb_mux_text[] = {
	"ZERO", "EAR_HPH_L", "EAR_LINE_1",
};

static const char *iir1_inp1_text[] = {
	"ZERO", "DEC1", "DEC2", "DEC3", "DEC4", "DEC5", "DEC6", "DEC7", "DEC8",
	"DEC9", "DEC10", "RX1", "RX2", "RX3", "RX4", "RX5", "RX6", "RX7"
};

static const struct soc_enum rx_mix1_inp1_chain_enum =
	SOC_ENUM_SINGLE(TABLA_A_CDC_CONN_RX1_B1_CTL, 0, 12, rx_mix1_text);

static const struct soc_enum rx_mix1_inp2_chain_enum =
	SOC_ENUM_SINGLE(TABLA_A_CDC_CONN_RX1_B1_CTL, 4, 12, rx_mix1_text);

static const struct soc_enum rx2_mix1_inp1_chain_enum =
	SOC_ENUM_SINGLE(TABLA_A_CDC_CONN_RX2_B1_CTL, 0, 12, rx_mix1_text);

static const struct soc_enum rx2_mix1_inp2_chain_enum =
	SOC_ENUM_SINGLE(TABLA_A_CDC_CONN_RX2_B1_CTL, 4, 12, rx_mix1_text);

static const struct soc_enum rx3_mix1_inp1_chain_enum =
	SOC_ENUM_SINGLE(TABLA_A_CDC_CONN_RX3_B1_CTL, 0, 12, rx_mix1_text);

static const struct soc_enum rx3_mix1_inp2_chain_enum =
	SOC_ENUM_SINGLE(TABLA_A_CDC_CONN_RX3_B1_CTL, 4, 12, rx_mix1_text);

static const struct soc_enum rx4_mix1_inp1_chain_enum =
	SOC_ENUM_SINGLE(TABLA_A_CDC_CONN_RX4_B1_CTL, 0, 12, rx_mix1_text);

static const struct soc_enum rx4_mix1_inp2_chain_enum =
	SOC_ENUM_SINGLE(TABLA_A_CDC_CONN_RX4_B1_CTL, 4, 12, rx_mix1_text);

static const struct soc_enum rx5_mix1_inp1_chain_enum =
	SOC_ENUM_SINGLE(TABLA_A_CDC_CONN_RX5_B1_CTL, 0, 12, rx_mix1_text);

static const struct soc_enum rx5_mix1_inp2_chain_enum =
	SOC_ENUM_SINGLE(TABLA_A_CDC_CONN_RX5_B1_CTL, 4, 12, rx_mix1_text);

static const struct soc_enum rx6_mix1_inp1_chain_enum =
	SOC_ENUM_SINGLE(TABLA_A_CDC_CONN_RX6_B1_CTL, 0, 12, rx_mix1_text);

static const struct soc_enum rx6_mix1_inp2_chain_enum =
	SOC_ENUM_SINGLE(TABLA_A_CDC_CONN_RX6_B1_CTL, 4, 12, rx_mix1_text);

static const struct soc_enum rx7_mix1_inp1_chain_enum =
	SOC_ENUM_SINGLE(TABLA_A_CDC_CONN_RX7_B1_CTL, 0, 12, rx_mix1_text);

static const struct soc_enum rx7_mix1_inp2_chain_enum =
	SOC_ENUM_SINGLE(TABLA_A_CDC_CONN_RX7_B1_CTL, 4, 12, rx_mix1_text);

static const struct soc_enum sb_tx5_mux_enum =
	SOC_ENUM_SINGLE(TABLA_A_CDC_CONN_TX_SB_B5_CTL, 0, 9, sb_tx5_mux_text);

static const struct soc_enum sb_tx6_mux_enum =
	SOC_ENUM_SINGLE(TABLA_A_CDC_CONN_TX_SB_B6_CTL, 0, 9, sb_tx6_mux_text);

static const struct soc_enum sb_tx7_mux_enum =
	SOC_ENUM_SINGLE(TABLA_A_CDC_CONN_TX_SB_B7_CTL, 0, 18,
			sb_tx7_to_tx10_mux_text);

static const struct soc_enum sb_tx8_mux_enum =
	SOC_ENUM_SINGLE(TABLA_A_CDC_CONN_TX_SB_B8_CTL, 0, 18,
			sb_tx7_to_tx10_mux_text);

static const struct soc_enum sb_tx1_mux_enum =
	SOC_ENUM_SINGLE(TABLA_A_CDC_CONN_TX_SB_B1_CTL, 0, 9, sb_tx1_mux_text);

static const struct soc_enum dec1_mux_enum =
	SOC_ENUM_SINGLE(TABLA_A_CDC_CONN_TX_B1_CTL, 0, 3, dec1_mux_text);

static const struct soc_enum dec2_mux_enum =
	SOC_ENUM_SINGLE(TABLA_A_CDC_CONN_TX_B1_CTL, 2, 3, dec2_mux_text);

static const struct soc_enum dec3_mux_enum =
	SOC_ENUM_SINGLE(TABLA_A_CDC_CONN_TX_B1_CTL, 4, 3, dec3_mux_text);

static const struct soc_enum dec4_mux_enum =
	SOC_ENUM_SINGLE(TABLA_A_CDC_CONN_TX_B1_CTL, 6, 3, dec4_mux_text);

static const struct soc_enum dec5_mux_enum =
	SOC_ENUM_SINGLE(TABLA_A_CDC_CONN_TX_B2_CTL, 0, 3, dec5_mux_text);

static const struct soc_enum dec6_mux_enum =
	SOC_ENUM_SINGLE(TABLA_A_CDC_CONN_TX_B2_CTL, 2, 3, dec6_mux_text);

static const struct soc_enum dec7_mux_enum =
	SOC_ENUM_SINGLE(TABLA_A_CDC_CONN_TX_B2_CTL, 4, 7, dec7_mux_text);

static const struct soc_enum dec8_mux_enum =
	SOC_ENUM_SINGLE(TABLA_A_CDC_CONN_TX_B3_CTL, 0, 7, dec8_mux_text);

static const struct soc_enum dec9_mux_enum =
	SOC_ENUM_SINGLE(TABLA_A_CDC_CONN_TX_B3_CTL, 3, 8, dec9_mux_text);

static const struct soc_enum dec10_mux_enum =
	SOC_ENUM_SINGLE(TABLA_A_CDC_CONN_TX_B4_CTL, 0, 8, dec10_mux_text);

static const struct soc_enum anc1_mux_enum =
	SOC_ENUM_SINGLE(TABLA_A_CDC_CONN_ANC_B1_CTL, 0, 16, anc_mux_text);

static const struct soc_enum anc2_mux_enum =
	SOC_ENUM_SINGLE(TABLA_A_CDC_CONN_ANC_B1_CTL, 4, 16, anc_mux_text);

static const struct soc_enum anc1_fb_mux_enum =
	SOC_ENUM_SINGLE(TABLA_A_CDC_CONN_ANC_B2_CTL, 0, 3, anc1_fb_mux_text);

static const struct soc_enum iir1_inp1_mux_enum =
	SOC_ENUM_SINGLE(TABLA_A_CDC_CONN_EQ1_B1_CTL, 0, 18, iir1_inp1_text);

static const struct snd_kcontrol_new rx_mix1_inp1_mux =
	SOC_DAPM_ENUM("RX1 MIX1 INP1 Mux", rx_mix1_inp1_chain_enum);

static const struct snd_kcontrol_new rx_mix1_inp2_mux =
	SOC_DAPM_ENUM("RX1 MIX1 INP2 Mux", rx_mix1_inp2_chain_enum);

static const struct snd_kcontrol_new rx2_mix1_inp1_mux =
	SOC_DAPM_ENUM("RX2 MIX1 INP1 Mux", rx2_mix1_inp1_chain_enum);

static const struct snd_kcontrol_new rx2_mix1_inp2_mux =
	SOC_DAPM_ENUM("RX2 MIX1 INP2 Mux", rx2_mix1_inp2_chain_enum);

static const struct snd_kcontrol_new rx3_mix1_inp1_mux =
	SOC_DAPM_ENUM("RX3 MIX1 INP1 Mux", rx3_mix1_inp1_chain_enum);

static const struct snd_kcontrol_new rx3_mix1_inp2_mux =
	SOC_DAPM_ENUM("RX3 MIX1 INP2 Mux", rx3_mix1_inp2_chain_enum);

static const struct snd_kcontrol_new rx4_mix1_inp1_mux =
	SOC_DAPM_ENUM("RX4 MIX1 INP1 Mux", rx4_mix1_inp1_chain_enum);

static const struct snd_kcontrol_new rx4_mix1_inp2_mux =
	SOC_DAPM_ENUM("RX4 MIX1 INP2 Mux", rx4_mix1_inp2_chain_enum);

static const struct snd_kcontrol_new rx5_mix1_inp1_mux =
	SOC_DAPM_ENUM("RX5 MIX1 INP1 Mux", rx5_mix1_inp1_chain_enum);

static const struct snd_kcontrol_new rx5_mix1_inp2_mux =
	SOC_DAPM_ENUM("RX5 MIX1 INP2 Mux", rx5_mix1_inp2_chain_enum);

static const struct snd_kcontrol_new rx6_mix1_inp1_mux =
	SOC_DAPM_ENUM("RX6 MIX1 INP1 Mux", rx6_mix1_inp1_chain_enum);

static const struct snd_kcontrol_new rx6_mix1_inp2_mux =
	SOC_DAPM_ENUM("RX6 MIX1 INP2 Mux", rx6_mix1_inp2_chain_enum);

static const struct snd_kcontrol_new rx7_mix1_inp1_mux =
	SOC_DAPM_ENUM("RX7 MIX1 INP1 Mux", rx7_mix1_inp1_chain_enum);

static const struct snd_kcontrol_new rx7_mix1_inp2_mux =
	SOC_DAPM_ENUM("RX7 MIX1 INP2 Mux", rx7_mix1_inp2_chain_enum);

static const struct snd_kcontrol_new sb_tx5_mux =
	SOC_DAPM_ENUM("SLIM TX5 MUX Mux", sb_tx5_mux_enum);

static const struct snd_kcontrol_new sb_tx6_mux =
	SOC_DAPM_ENUM("SLIM TX6 MUX Mux", sb_tx6_mux_enum);

static const struct snd_kcontrol_new sb_tx7_mux =
	SOC_DAPM_ENUM("SLIM TX7 MUX Mux", sb_tx7_mux_enum);

static const struct snd_kcontrol_new sb_tx8_mux =
	SOC_DAPM_ENUM("SLIM TX8 MUX Mux", sb_tx8_mux_enum);

static const struct snd_kcontrol_new sb_tx1_mux =
	SOC_DAPM_ENUM("SLIM TX1 MUX Mux", sb_tx1_mux_enum);

static const struct snd_kcontrol_new dec1_mux =
	SOC_DAPM_ENUM("DEC1 MUX Mux", dec1_mux_enum);

static const struct snd_kcontrol_new dec2_mux =
	SOC_DAPM_ENUM("DEC2 MUX Mux", dec2_mux_enum);

static const struct snd_kcontrol_new dec3_mux =
	SOC_DAPM_ENUM("DEC3 MUX Mux", dec3_mux_enum);

static const struct snd_kcontrol_new dec4_mux =
	SOC_DAPM_ENUM("DEC4 MUX Mux", dec4_mux_enum);

static const struct snd_kcontrol_new dec5_mux =
	SOC_DAPM_ENUM("DEC5 MUX Mux", dec5_mux_enum);

static const struct snd_kcontrol_new dec6_mux =
	SOC_DAPM_ENUM("DEC6 MUX Mux", dec6_mux_enum);

static const struct snd_kcontrol_new dec7_mux =
	SOC_DAPM_ENUM("DEC7 MUX Mux", dec7_mux_enum);

static const struct snd_kcontrol_new anc1_mux =
	SOC_DAPM_ENUM("ANC1 MUX Mux", anc1_mux_enum);
static const struct snd_kcontrol_new dec8_mux =
	SOC_DAPM_ENUM("DEC8 MUX Mux", dec8_mux_enum);

static const struct snd_kcontrol_new dec9_mux =
	SOC_DAPM_ENUM("DEC9 MUX Mux", dec9_mux_enum);

static const struct snd_kcontrol_new dec10_mux =
	SOC_DAPM_ENUM("DEC10 MUX Mux", dec10_mux_enum);

static const struct snd_kcontrol_new iir1_inp1_mux =
	SOC_DAPM_ENUM("IIR1 INP1 Mux", iir1_inp1_mux_enum);

static const struct snd_kcontrol_new anc2_mux =
	SOC_DAPM_ENUM("ANC2 MUX Mux", anc2_mux_enum);

static const struct snd_kcontrol_new anc1_fb_mux =
	SOC_DAPM_ENUM("ANC1 FB MUX Mux", anc1_fb_mux_enum);

static const struct snd_kcontrol_new dac1_switch[] = {
	SOC_DAPM_SINGLE("Switch", TABLA_A_RX_EAR_EN, 5, 1, 0)
};
static const struct snd_kcontrol_new hphl_switch[] = {
	SOC_DAPM_SINGLE("Switch", TABLA_A_RX_HPH_L_DAC_CTL, 6, 1, 0)
};
static const struct snd_kcontrol_new hphr_switch[] = {
	SOC_DAPM_SINGLE("Switch", TABLA_A_RX_HPH_R_DAC_CTL, 6, 1, 0)
};
static const struct snd_kcontrol_new lineout1_switch[] = {
	SOC_DAPM_SINGLE("Switch", TABLA_A_RX_LINE_1_DAC_CTL, 6, 1, 0)
};
static const struct snd_kcontrol_new lineout2_switch[] = {
	SOC_DAPM_SINGLE("Switch", TABLA_A_RX_LINE_2_DAC_CTL, 6, 1, 0)
};
static const struct snd_kcontrol_new lineout3_switch[] = {
	SOC_DAPM_SINGLE("Switch", TABLA_A_RX_LINE_3_DAC_CTL, 6, 1, 0)
};
static const struct snd_kcontrol_new lineout4_switch[] = {
	SOC_DAPM_SINGLE("Switch", TABLA_A_RX_LINE_4_DAC_CTL, 6, 1, 0)
};
static const struct snd_kcontrol_new lineout5_switch[] = {
	SOC_DAPM_SINGLE("Switch", TABLA_A_RX_LINE_5_DAC_CTL, 6, 1, 0)
};

static void tabla_codec_enable_adc_block(struct snd_soc_codec *codec,
	int enable)
{
	struct tabla_priv *tabla = snd_soc_codec_get_drvdata(codec);

	pr_debug("%s %d\n", __func__, enable);

	if (enable) {
		tabla->adc_count++;
		snd_soc_update_bits(codec, TABLA_A_TX_COM_BIAS, 0xE0, 0xE0);
		snd_soc_update_bits(codec, TABLA_A_CDC_CLK_OTHR_CTL, 0x2, 0x2);
	} else {
		tabla->adc_count--;
		if (!tabla->adc_count) {
			snd_soc_update_bits(codec, TABLA_A_CDC_CLK_OTHR_CTL,
				0x2, 0x0);
			if (!tabla->mbhc_polling_active)
				snd_soc_update_bits(codec, TABLA_A_TX_COM_BIAS,
					0xE0, 0x0);
		}
	}
}

static int tabla_codec_enable_adc(struct snd_soc_dapm_widget *w,
	struct snd_kcontrol *kcontrol, int event)
{
	struct snd_soc_codec *codec = w->codec;
	u16 adc_reg;

	pr_debug("%s %d\n", __func__, event);

	if (w->reg == TABLA_A_TX_1_2_EN)
		adc_reg = TABLA_A_TX_1_2_TEST_CTL;
	else if (w->reg == TABLA_A_TX_3_4_EN)
		adc_reg = TABLA_A_TX_3_4_TEST_CTL;
	else if (w->reg == TABLA_A_TX_5_6_EN)
		adc_reg = TABLA_A_TX_5_6_TEST_CTL;
	else {
		pr_err("%s: Error, invalid adc register\n", __func__);
		return -EINVAL;
	}

	switch (event) {
	case SND_SOC_DAPM_PRE_PMU:
		tabla_codec_enable_adc_block(codec, 1);
		break;
	case SND_SOC_DAPM_POST_PMU:
		snd_soc_update_bits(codec, adc_reg, 1 << w->shift,
			1 << w->shift);
		usleep_range(1000, 1000);
		snd_soc_update_bits(codec, adc_reg, 1 << w->shift, 0x00);
		usleep_range(1000, 1000);
		break;
	case SND_SOC_DAPM_POST_PMD:
		tabla_codec_enable_adc_block(codec, 0);
		break;
	}
	return 0;
}

static int tabla_codec_enable_lineout(struct snd_soc_dapm_widget *w,
		struct snd_kcontrol *kcontrol, int event)
{
	struct snd_soc_codec *codec = w->codec;
	u16 lineout_gain_reg;

	pr_debug("%s %d %s\n", __func__, event, w->name);

	switch (w->shift) {
	case 0:
		lineout_gain_reg = TABLA_A_RX_LINE_1_GAIN;
		break;
	case 1:
		lineout_gain_reg = TABLA_A_RX_LINE_2_GAIN;
		break;
	case 2:
		lineout_gain_reg = TABLA_A_RX_LINE_3_GAIN;
		break;
	case 3:
		lineout_gain_reg = TABLA_A_RX_LINE_4_GAIN;
		break;
	case 4:
		lineout_gain_reg = TABLA_A_RX_LINE_5_GAIN;
		break;
	default:
		pr_err("%s: Error, incorrect lineout register value\n",
			__func__);
		return -EINVAL;
	}

	switch (event) {
	case SND_SOC_DAPM_PRE_PMU:
		snd_soc_update_bits(codec, lineout_gain_reg, 0x40, 0x40);
		break;
	case SND_SOC_DAPM_POST_PMU:
		pr_debug("%s: sleeping 40 ms after %s PA turn on\n",
				__func__, w->name);
		usleep_range(40000, 40000);
		break;
	case SND_SOC_DAPM_POST_PMD:
		snd_soc_update_bits(codec, lineout_gain_reg, 0x40, 0x00);
		break;
	}
	return 0;
}


static int tabla_codec_enable_dmic(struct snd_soc_dapm_widget *w,
	struct snd_kcontrol *kcontrol, int event)
{
	struct snd_soc_codec *codec = w->codec;
	u16 tx_mux_ctl_reg, tx_dmic_ctl_reg;
	u8 dmic_clk_sel, dmic_clk_en;
	unsigned int dmic;
	int ret;

	ret = kstrtouint(strpbrk(w->name, "123456"), 10, &dmic);
	if (ret < 0) {
		pr_err("%s: Invalid DMIC line on the codec\n", __func__);
		return -EINVAL;
	}

	switch (dmic) {
	case 1:
	case 2:
		dmic_clk_sel = 0x02;
		dmic_clk_en = 0x01;
		break;

	case 3:
	case 4:
		dmic_clk_sel = 0x08;
		dmic_clk_en = 0x04;
		break;

	case 5:
	case 6:
		dmic_clk_sel = 0x20;
		dmic_clk_en = 0x10;
		break;

	default:
		pr_err("%s: Invalid DMIC Selection\n", __func__);
		return -EINVAL;
	}

	tx_mux_ctl_reg = TABLA_A_CDC_TX1_MUX_CTL + 8 * (dmic - 1);
	tx_dmic_ctl_reg = TABLA_A_CDC_TX1_DMIC_CTL + 8 * (dmic - 1);

	pr_debug("%s %d\n", __func__, event);

	switch (event) {
	case SND_SOC_DAPM_PRE_PMU:
		snd_soc_update_bits(codec, tx_mux_ctl_reg, 0x1, 0x1);

		snd_soc_update_bits(codec, TABLA_A_CDC_CLK_DMIC_CTL,
				dmic_clk_sel, dmic_clk_sel);

		snd_soc_update_bits(codec, tx_dmic_ctl_reg, 0x1, 0x1);

		snd_soc_update_bits(codec, TABLA_A_CDC_CLK_DMIC_CTL,
				dmic_clk_en, dmic_clk_en);
		break;
	case SND_SOC_DAPM_POST_PMD:
		snd_soc_update_bits(codec, TABLA_A_CDC_CLK_DMIC_CTL,
				dmic_clk_en, 0);
		break;
	}
	return 0;
}

static int tabla_codec_enable_anc(struct snd_soc_dapm_widget *w,
	struct snd_kcontrol *kcontrol, int event)
{
	struct snd_soc_codec *codec = w->codec;
	const char *filename;
	const struct firmware *fw;
	int i;
	int ret;
	int num_anc_slots;
	struct anc_header *anc_head;
	struct tabla_priv *tabla = snd_soc_codec_get_drvdata(codec);
	u32 anc_writes_size = 0;
	int anc_size_remaining;
	u32 *anc_ptr;
	u16 reg;
	u8 mask, val, old_val;

	pr_debug("%s %d\n", __func__, event);
	switch (event) {
	case SND_SOC_DAPM_PRE_PMU:

		filename = "wcd9310/wcd9310_anc.bin";

		ret = request_firmware(&fw, filename, codec->dev);
		if (ret != 0) {
			dev_err(codec->dev, "Failed to acquire ANC data: %d\n",
				ret);
			return -ENODEV;
		}

		if (fw->size < sizeof(struct anc_header)) {
			dev_err(codec->dev, "Not enough data\n");
			release_firmware(fw);
			return -ENOMEM;
		}

		/* First number is the number of register writes */
		anc_head = (struct anc_header *)(fw->data);
		anc_ptr = (u32 *)((u32)fw->data + sizeof(struct anc_header));
		anc_size_remaining = fw->size - sizeof(struct anc_header);
		num_anc_slots = anc_head->num_anc_slots;

		if (tabla->anc_slot >= num_anc_slots) {
			dev_err(codec->dev, "Invalid ANC slot selected\n");
			release_firmware(fw);
			return -EINVAL;
		}

		for (i = 0; i < num_anc_slots; i++) {

			if (anc_size_remaining < TABLA_PACKED_REG_SIZE) {
				dev_err(codec->dev, "Invalid register format\n");
				release_firmware(fw);
				return -EINVAL;
			}
			anc_writes_size = (u32)(*anc_ptr);
			anc_size_remaining -= sizeof(u32);
			anc_ptr += 1;

			if (anc_writes_size * TABLA_PACKED_REG_SIZE
				> anc_size_remaining) {
				dev_err(codec->dev, "Invalid register format\n");
				release_firmware(fw);
				return -ENOMEM;
			}

			if (tabla->anc_slot == i)
				break;

			anc_size_remaining -= (anc_writes_size *
				TABLA_PACKED_REG_SIZE);
			anc_ptr += anc_writes_size;
		}
		if (i == num_anc_slots) {
			dev_err(codec->dev, "Selected ANC slot not present\n");
			release_firmware(fw);
			return -ENOMEM;
		}

		for (i = 0; i < anc_writes_size; i++) {
			TABLA_CODEC_UNPACK_ENTRY(anc_ptr[i], reg,
				mask, val);
			old_val = snd_soc_read(codec, reg);
			snd_soc_write(codec, reg, (old_val & ~mask) |
				(val & mask));
		}
		release_firmware(fw);

		break;
	case SND_SOC_DAPM_POST_PMD:
		snd_soc_write(codec, TABLA_A_CDC_CLK_ANC_RESET_CTL, 0xFF);
		snd_soc_write(codec, TABLA_A_CDC_CLK_ANC_CLK_EN_CTL, 0);
		break;
	}
	return 0;
}

static void tabla_codec_update_cfilt_usage(struct snd_soc_codec *codec,
					   u8 cfilt_sel, int inc)
{
	struct tabla_priv *tabla = snd_soc_codec_get_drvdata(codec);
	u32 *cfilt_cnt_ptr = NULL;
	u16 micb_cfilt_reg;

	switch (cfilt_sel) {
	case TABLA_CFILT1_SEL:
		cfilt_cnt_ptr = &tabla->cfilt1_cnt;
		micb_cfilt_reg = TABLA_A_MICB_CFILT_1_CTL;
		break;
	case TABLA_CFILT2_SEL:
		cfilt_cnt_ptr = &tabla->cfilt2_cnt;
		micb_cfilt_reg = TABLA_A_MICB_CFILT_2_CTL;
		break;
	case TABLA_CFILT3_SEL:
		cfilt_cnt_ptr = &tabla->cfilt3_cnt;
		micb_cfilt_reg = TABLA_A_MICB_CFILT_3_CTL;
		break;
	default:
		return; /* should not happen */
	}

	if (inc) {
		if (!(*cfilt_cnt_ptr)++)
			snd_soc_update_bits(codec, micb_cfilt_reg, 0x80, 0x80);
	} else {
		/* check if count not zero, decrement
		 * then check if zero, go ahead disable cfilter
		 */
		if ((*cfilt_cnt_ptr) && !--(*cfilt_cnt_ptr))
			snd_soc_update_bits(codec, micb_cfilt_reg, 0x80, 0);
	}
}

static void tabla_codec_disable_button_presses(struct snd_soc_codec *codec)
{
	snd_soc_write(codec, TABLA_A_CDC_MBHC_VOLT_B4_CTL, 0x80);
	snd_soc_write(codec, TABLA_A_CDC_MBHC_VOLT_B3_CTL, 0x00);
}

static void tabla_codec_start_hs_polling(struct snd_soc_codec *codec)
{
	struct tabla_priv *tabla = snd_soc_codec_get_drvdata(codec);

	snd_soc_write(codec, TABLA_A_MBHC_SCALING_MUX_1, 0x84);
	tabla_enable_irq(codec->control_data, TABLA_IRQ_MBHC_REMOVAL);
	if (!tabla->no_mic_headset_override) {
		tabla_enable_irq(codec->control_data, TABLA_IRQ_MBHC_POTENTIAL);
		tabla_enable_irq(codec->control_data, TABLA_IRQ_MBHC_RELEASE);
	} else {
		tabla_codec_disable_button_presses(codec);
	}
	snd_soc_write(codec, TABLA_A_CDC_MBHC_EN_CTL, 0x1);
	snd_soc_update_bits(codec, TABLA_A_CDC_MBHC_CLK_CTL, 0x8, 0x0);
	snd_soc_write(codec, TABLA_A_CDC_MBHC_EN_CTL, 0x1);
}

static void tabla_codec_pause_hs_polling(struct snd_soc_codec *codec)
{
	struct tabla_priv *tabla = snd_soc_codec_get_drvdata(codec);

	snd_soc_update_bits(codec, TABLA_A_CDC_MBHC_CLK_CTL, 0x8, 0x8);
	tabla_disable_irq(codec->control_data, TABLA_IRQ_MBHC_REMOVAL);
	if (!tabla->no_mic_headset_override) {
		tabla_disable_irq(codec->control_data,
			TABLA_IRQ_MBHC_POTENTIAL);
		tabla_disable_irq(codec->control_data, TABLA_IRQ_MBHC_RELEASE);
	}
}

static int tabla_codec_enable_micbias(struct snd_soc_dapm_widget *w,
	struct snd_kcontrol *kcontrol, int event)
{
	struct snd_soc_codec *codec = w->codec;
	struct tabla_priv *tabla = snd_soc_codec_get_drvdata(codec);
	u16 micb_int_reg;
	int micb_line;
	u8 cfilt_sel_val = 0;
	char *internal1_text = "Internal1";
	char *internal2_text = "Internal2";
	char *internal3_text = "Internal3";

	pr_debug("%s %d\n", __func__, event);
	switch (w->reg) {
	case TABLA_A_MICB_1_CTL:
		micb_int_reg = TABLA_A_MICB_1_INT_RBIAS;
		cfilt_sel_val = tabla->pdata->micbias.bias1_cfilt_sel;
		micb_line = TABLA_MICBIAS1;
		break;
	case TABLA_A_MICB_2_CTL:
		micb_int_reg = TABLA_A_MICB_2_INT_RBIAS;
		cfilt_sel_val = tabla->pdata->micbias.bias2_cfilt_sel;
		micb_line = TABLA_MICBIAS2;
		break;
	case TABLA_A_MICB_3_CTL:
		micb_int_reg = TABLA_A_MICB_3_INT_RBIAS;
		cfilt_sel_val = tabla->pdata->micbias.bias3_cfilt_sel;
		micb_line = TABLA_MICBIAS3;
		break;
	case TABLA_A_MICB_4_CTL:
		micb_int_reg = TABLA_A_MICB_4_INT_RBIAS;
		cfilt_sel_val = tabla->pdata->micbias.bias4_cfilt_sel;
		micb_line = TABLA_MICBIAS4;
		break;
	default:
		pr_err("%s: Error, invalid micbias register\n", __func__);
		return -EINVAL;
	}

	switch (event) {
	case SND_SOC_DAPM_PRE_PMU:
		snd_soc_update_bits(codec, w->reg, 0x0E, 0x0A);
		tabla_codec_update_cfilt_usage(codec, cfilt_sel_val, 1);

		if (strnstr(w->name, internal1_text, 30))
			snd_soc_update_bits(codec, micb_int_reg, 0xE0, 0xE0);
		else if (strnstr(w->name, internal2_text, 30))
			snd_soc_update_bits(codec, micb_int_reg, 0x1C, 0x1C);
		else if (strnstr(w->name, internal3_text, 30))
			snd_soc_update_bits(codec, micb_int_reg, 0x3, 0x3);

		break;
	case SND_SOC_DAPM_POST_PMU:
		if (tabla->mbhc_polling_active &&
			(tabla->calibration->bias == micb_line)) {
			tabla_codec_pause_hs_polling(codec);
			tabla_codec_start_hs_polling(codec);
		}
		break;
	case SND_SOC_DAPM_POST_PMD:
		if (strnstr(w->name, internal1_text, 30))
			snd_soc_update_bits(codec, micb_int_reg, 0x80, 0x00);
		else if (strnstr(w->name, internal2_text, 30))
			snd_soc_update_bits(codec, micb_int_reg, 0x10, 0x00);
		else if (strnstr(w->name, internal3_text, 30))
			snd_soc_update_bits(codec, micb_int_reg, 0x2, 0x0);

		tabla_codec_update_cfilt_usage(codec, cfilt_sel_val, 0);
		break;
	}

	return 0;
}

static int tabla_codec_enable_dec(struct snd_soc_dapm_widget *w,
	struct snd_kcontrol *kcontrol, int event)
{
	struct snd_soc_codec *codec = w->codec;
	u16 dec_reset_reg;

	pr_debug("%s %d\n", __func__, event);

	if (w->reg == TABLA_A_CDC_CLK_TX_CLK_EN_B1_CTL)
		dec_reset_reg = TABLA_A_CDC_CLK_TX_RESET_B1_CTL;
	else if (w->reg == TABLA_A_CDC_CLK_TX_CLK_EN_B2_CTL)
		dec_reset_reg = TABLA_A_CDC_CLK_TX_RESET_B2_CTL;
	else {
		pr_err("%s: Error, incorrect dec\n", __func__);
		return -EINVAL;
	}

	switch (event) {
	case SND_SOC_DAPM_PRE_PMU:
		snd_soc_update_bits(codec, dec_reset_reg, 1 << w->shift,
			1 << w->shift);
		snd_soc_update_bits(codec, dec_reset_reg, 1 << w->shift, 0x0);
		break;
	}
	return 0;
}

static int tabla_codec_reset_interpolator(struct snd_soc_dapm_widget *w,
	struct snd_kcontrol *kcontrol, int event)
{
	struct snd_soc_codec *codec = w->codec;

	switch (event) {
	case SND_SOC_DAPM_PRE_PMU:
		snd_soc_update_bits(codec, TABLA_A_CDC_CLK_RX_RESET_CTL,
			1 << w->shift, 1 << w->shift);
		snd_soc_update_bits(codec, TABLA_A_CDC_CLK_RX_RESET_CTL,
			1 << w->shift, 0x0);
		break;
	}
	return 0;
}

static int tabla_codec_enable_ldo_h(struct snd_soc_dapm_widget *w,
	struct snd_kcontrol *kcontrol, int event)
{
	switch (event) {
	case SND_SOC_DAPM_POST_PMU:
	case SND_SOC_DAPM_POST_PMD:
		usleep_range(1000, 1000);
		break;
	}
	return 0;
}


static void tabla_enable_rx_bias(struct snd_soc_codec *codec, u32  enable)
{
	struct tabla_priv *tabla = snd_soc_codec_get_drvdata(codec);

	if (enable) {
		tabla->rx_bias_count++;
		if (tabla->rx_bias_count == 1)
			snd_soc_update_bits(codec, TABLA_A_RX_COM_BIAS,
				0x80, 0x80);
	} else {
		tabla->rx_bias_count--;
		if (!tabla->rx_bias_count)
			snd_soc_update_bits(codec, TABLA_A_RX_COM_BIAS,
				0x80, 0x00);
	}
}

static int tabla_codec_enable_rx_bias(struct snd_soc_dapm_widget *w,
	struct snd_kcontrol *kcontrol, int event)
{
	struct snd_soc_codec *codec = w->codec;

	pr_debug("%s %d\n", __func__, event);

	switch (event) {
	case SND_SOC_DAPM_PRE_PMU:
		tabla_enable_rx_bias(codec, 1);
		break;
	case SND_SOC_DAPM_POST_PMD:
		tabla_enable_rx_bias(codec, 0);
		break;
	}
	return 0;
}


static int tabla_hph_pa_event(struct snd_soc_dapm_widget *w,
	struct snd_kcontrol *kcontrol, int event)
{

	pr_debug("%s: event = %d\n", __func__, event);

	switch (event) {
	case SND_SOC_DAPM_POST_PMD:

		pr_debug("%s: sleep 10 ms after %s PA disable.\n", __func__,
				w->name);
		usleep_range(10000, 10000);

		break;
	}
	return 0;
}

static const struct snd_soc_dapm_widget tabla_dapm_widgets[] = {
	/*RX stuff */
	SND_SOC_DAPM_OUTPUT("EAR"),

	SND_SOC_DAPM_PGA("EAR PA", TABLA_A_RX_EAR_EN, 4, 0, NULL, 0),

	SND_SOC_DAPM_MIXER("DAC1", TABLA_A_RX_EAR_EN, 6, 0, dac1_switch,
		ARRAY_SIZE(dac1_switch)),

	SND_SOC_DAPM_AIF_IN("SLIM RX1", "AIF1 Playback", 0, SND_SOC_NOPM, 0, 0),
	SND_SOC_DAPM_AIF_IN("SLIM RX2", "AIF1 Playback", 0, SND_SOC_NOPM, 0, 0),

	/* Headphone */
	SND_SOC_DAPM_OUTPUT("HEADPHONE"),
	SND_SOC_DAPM_PGA_E("HPHL", TABLA_A_RX_HPH_CNP_EN, 5, 0, NULL, 0,
		tabla_hph_pa_event, SND_SOC_DAPM_POST_PMD),

	SND_SOC_DAPM_MIXER("HPHL DAC", TABLA_A_RX_HPH_L_DAC_CTL, 7, 0,
		hphl_switch, ARRAY_SIZE(hphl_switch)),

	SND_SOC_DAPM_PGA_E("HPHR", TABLA_A_RX_HPH_CNP_EN, 4, 0, NULL, 0,
		tabla_hph_pa_event, SND_SOC_DAPM_POST_PMD),

	SND_SOC_DAPM_MIXER("HPHR DAC", TABLA_A_RX_HPH_R_DAC_CTL, 7, 0,
		hphr_switch, ARRAY_SIZE(hphr_switch)),

	/* Speaker */
	SND_SOC_DAPM_OUTPUT("LINEOUT1"),
	SND_SOC_DAPM_OUTPUT("LINEOUT2"),
	SND_SOC_DAPM_OUTPUT("LINEOUT3"),
	SND_SOC_DAPM_OUTPUT("LINEOUT4"),
	SND_SOC_DAPM_OUTPUT("LINEOUT5"),

	SND_SOC_DAPM_PGA_E("LINEOUT1 PA", TABLA_A_RX_LINE_CNP_EN, 0, 0, NULL,
			0, tabla_codec_enable_lineout, SND_SOC_DAPM_PRE_PMU |
			SND_SOC_DAPM_POST_PMU | SND_SOC_DAPM_POST_PMD),
	SND_SOC_DAPM_PGA_E("LINEOUT2 PA", TABLA_A_RX_LINE_CNP_EN, 1, 0, NULL,
			0, tabla_codec_enable_lineout, SND_SOC_DAPM_PRE_PMU |
			SND_SOC_DAPM_POST_PMU | SND_SOC_DAPM_POST_PMD),
	SND_SOC_DAPM_PGA_E("LINEOUT3 PA", TABLA_A_RX_LINE_CNP_EN, 2, 0, NULL,
			0, tabla_codec_enable_lineout, SND_SOC_DAPM_PRE_PMU |
			SND_SOC_DAPM_POST_PMU | SND_SOC_DAPM_POST_PMD),
	SND_SOC_DAPM_PGA_E("LINEOUT4 PA", TABLA_A_RX_LINE_CNP_EN, 3, 0, NULL,
			0, tabla_codec_enable_lineout, SND_SOC_DAPM_PRE_PMU |
			SND_SOC_DAPM_POST_PMU | SND_SOC_DAPM_POST_PMD),
	SND_SOC_DAPM_PGA_E("LINEOUT5 PA", TABLA_A_RX_LINE_CNP_EN, 4, 0, NULL, 0,
		tabla_codec_enable_lineout, SND_SOC_DAPM_PRE_PMU |
		SND_SOC_DAPM_POST_PMU | SND_SOC_DAPM_POST_PMD),

	SND_SOC_DAPM_MIXER("LINEOUT1 DAC", TABLA_A_RX_LINE_1_DAC_CTL, 7, 0,
		lineout1_switch, ARRAY_SIZE(lineout1_switch)),
	SND_SOC_DAPM_MIXER("LINEOUT2 DAC", TABLA_A_RX_LINE_2_DAC_CTL, 7, 0,
		lineout2_switch, ARRAY_SIZE(lineout2_switch)),
	SND_SOC_DAPM_MIXER("LINEOUT3 DAC", TABLA_A_RX_LINE_3_DAC_CTL, 7, 0,
		lineout3_switch, ARRAY_SIZE(lineout3_switch)),
	SND_SOC_DAPM_MIXER("LINEOUT4 DAC", TABLA_A_RX_LINE_4_DAC_CTL, 7, 0,
		lineout4_switch, ARRAY_SIZE(lineout4_switch)),
	SND_SOC_DAPM_MIXER("LINEOUT5 DAC", TABLA_A_RX_LINE_5_DAC_CTL, 7, 0,
		lineout5_switch, ARRAY_SIZE(lineout5_switch)),

	SND_SOC_DAPM_MIXER_E("RX1 MIX1", TABLA_A_CDC_CLK_RX_B1_CTL, 0, 0, NULL,
		0, tabla_codec_reset_interpolator, SND_SOC_DAPM_PRE_PMU),
	SND_SOC_DAPM_MIXER_E("RX2 MIX1", TABLA_A_CDC_CLK_RX_B1_CTL, 1, 0, NULL,
		0, tabla_codec_reset_interpolator, SND_SOC_DAPM_PRE_PMU),
	SND_SOC_DAPM_MIXER_E("RX3 MIX1", TABLA_A_CDC_CLK_RX_B1_CTL, 2, 0, NULL,
		0, tabla_codec_reset_interpolator, SND_SOC_DAPM_PRE_PMU),
	SND_SOC_DAPM_MIXER_E("RX4 MIX1", TABLA_A_CDC_CLK_RX_B1_CTL, 3, 0, NULL,
		0, tabla_codec_reset_interpolator, SND_SOC_DAPM_PRE_PMU),
	SND_SOC_DAPM_MIXER_E("RX5 MIX1", TABLA_A_CDC_CLK_RX_B1_CTL, 4, 0, NULL,
		0, tabla_codec_reset_interpolator, SND_SOC_DAPM_PRE_PMU),
	SND_SOC_DAPM_MIXER_E("RX6 MIX1", TABLA_A_CDC_CLK_RX_B1_CTL, 5, 0, NULL,
		0, tabla_codec_reset_interpolator, SND_SOC_DAPM_PRE_PMU),
	SND_SOC_DAPM_MIXER_E("RX7 MIX1", TABLA_A_CDC_CLK_RX_B1_CTL, 6, 0, NULL,
		0, tabla_codec_reset_interpolator, SND_SOC_DAPM_PRE_PMU),

	SND_SOC_DAPM_MIXER("RX1 CHAIN", TABLA_A_CDC_RX1_B6_CTL, 5, 0, NULL, 0),
	SND_SOC_DAPM_MIXER("RX2 CHAIN", TABLA_A_CDC_RX2_B6_CTL, 5, 0, NULL, 0),

	SND_SOC_DAPM_MUX("RX1 MIX1 INP1", SND_SOC_NOPM, 0, 0,
		&rx_mix1_inp1_mux),
	SND_SOC_DAPM_MUX("RX1 MIX1 INP2", SND_SOC_NOPM, 0, 0,
		&rx_mix1_inp2_mux),
	SND_SOC_DAPM_MUX("RX2 MIX1 INP1", SND_SOC_NOPM, 0, 0,
		&rx2_mix1_inp1_mux),
	SND_SOC_DAPM_MUX("RX2 MIX1 INP2", SND_SOC_NOPM, 0, 0,
		&rx2_mix1_inp2_mux),
	SND_SOC_DAPM_MUX("RX3 MIX1 INP1", SND_SOC_NOPM, 0, 0,
		&rx3_mix1_inp1_mux),
	SND_SOC_DAPM_MUX("RX3 MIX1 INP2", SND_SOC_NOPM, 0, 0,
		&rx3_mix1_inp2_mux),
	SND_SOC_DAPM_MUX("RX4 MIX1 INP1", SND_SOC_NOPM, 0, 0,
		&rx4_mix1_inp1_mux),
	SND_SOC_DAPM_MUX("RX4 MIX1 INP2", SND_SOC_NOPM, 0, 0,
		&rx4_mix1_inp2_mux),
	SND_SOC_DAPM_MUX("RX5 MIX1 INP1", SND_SOC_NOPM, 0, 0,
		&rx5_mix1_inp1_mux),
	SND_SOC_DAPM_MUX("RX5 MIX1 INP2", SND_SOC_NOPM, 0, 0,
		&rx5_mix1_inp2_mux),
	SND_SOC_DAPM_MUX("RX6 MIX1 INP1", SND_SOC_NOPM, 0, 0,
		&rx6_mix1_inp1_mux),
	SND_SOC_DAPM_MUX("RX6 MIX1 INP2", SND_SOC_NOPM, 0, 0,
		&rx6_mix1_inp2_mux),
	SND_SOC_DAPM_MUX("RX7 MIX1 INP1", SND_SOC_NOPM, 0, 0,
		&rx7_mix1_inp1_mux),
	SND_SOC_DAPM_MUX("RX7 MIX1 INP2", SND_SOC_NOPM, 0, 0,
		&rx7_mix1_inp2_mux),

	SND_SOC_DAPM_SUPPLY("CP", TABLA_A_CP_EN, 0, 0,
		tabla_codec_enable_charge_pump, SND_SOC_DAPM_POST_PMU |
		SND_SOC_DAPM_PRE_PMD),

	SND_SOC_DAPM_SUPPLY("RX_BIAS", SND_SOC_NOPM, 0, 0,
		tabla_codec_enable_rx_bias, SND_SOC_DAPM_PRE_PMU |
		SND_SOC_DAPM_POST_PMD),

	/* TX */

	SND_SOC_DAPM_SUPPLY("CDC_CONN", TABLA_A_CDC_CLK_OTHR_CTL, 2, 0, NULL,
		0),

	SND_SOC_DAPM_SUPPLY("LDO_H", TABLA_A_LDO_H_MODE_1, 7, 0,
		tabla_codec_enable_ldo_h, SND_SOC_DAPM_POST_PMU),

	SND_SOC_DAPM_INPUT("AMIC1"),
	SND_SOC_DAPM_MICBIAS_E("MIC BIAS1 External", TABLA_A_MICB_1_CTL, 7, 0,
		tabla_codec_enable_micbias, SND_SOC_DAPM_PRE_PMU |
		SND_SOC_DAPM_POST_PMU | SND_SOC_DAPM_POST_PMD),
	SND_SOC_DAPM_MICBIAS_E("MIC BIAS1 Internal1", TABLA_A_MICB_1_CTL, 7, 0,
		tabla_codec_enable_micbias, SND_SOC_DAPM_PRE_PMU |
		SND_SOC_DAPM_POST_PMU | SND_SOC_DAPM_POST_PMD),
	SND_SOC_DAPM_MICBIAS_E("MIC BIAS1 Internal2", TABLA_A_MICB_1_CTL, 7, 0,
		tabla_codec_enable_micbias, SND_SOC_DAPM_PRE_PMU |
		SND_SOC_DAPM_POST_PMU | SND_SOC_DAPM_POST_PMD),
	SND_SOC_DAPM_ADC_E("ADC1", NULL, TABLA_A_TX_1_2_EN, 7, 0,
		tabla_codec_enable_adc, SND_SOC_DAPM_PRE_PMU |
		SND_SOC_DAPM_POST_PMU | SND_SOC_DAPM_POST_PMD),

	SND_SOC_DAPM_INPUT("AMIC3"),
	SND_SOC_DAPM_ADC_E("ADC3", NULL, TABLA_A_TX_3_4_EN, 7, 0,
		tabla_codec_enable_adc, SND_SOC_DAPM_PRE_PMU |
		SND_SOC_DAPM_POST_PMU | SND_SOC_DAPM_POST_PMD),

	SND_SOC_DAPM_INPUT("AMIC4"),
	SND_SOC_DAPM_ADC_E("ADC4", NULL, TABLA_A_TX_3_4_EN, 3, 0,
		tabla_codec_enable_adc, SND_SOC_DAPM_PRE_PMU |
		SND_SOC_DAPM_POST_PMU | SND_SOC_DAPM_POST_PMD),

	SND_SOC_DAPM_MICBIAS_E("MIC BIAS4 External", TABLA_A_MICB_4_CTL, 7, 0,
		tabla_codec_enable_micbias, SND_SOC_DAPM_PRE_PMU |
		SND_SOC_DAPM_POST_PMU | SND_SOC_DAPM_POST_PMD),

	SND_SOC_DAPM_INPUT("AMIC5"),
	SND_SOC_DAPM_ADC_E("ADC5", NULL, TABLA_A_TX_5_6_EN, 7, 0,
		tabla_codec_enable_adc, SND_SOC_DAPM_POST_PMU),

	SND_SOC_DAPM_INPUT("AMIC6"),
	SND_SOC_DAPM_ADC_E("ADC6", NULL, TABLA_A_TX_5_6_EN, 3, 0,
		tabla_codec_enable_adc, SND_SOC_DAPM_POST_PMU),

	SND_SOC_DAPM_MUX_E("DEC1 MUX", TABLA_A_CDC_CLK_TX_CLK_EN_B1_CTL, 0, 0,
		&dec1_mux, tabla_codec_enable_dec, SND_SOC_DAPM_PRE_PMU),

	SND_SOC_DAPM_MUX_E("DEC2 MUX", TABLA_A_CDC_CLK_TX_CLK_EN_B1_CTL, 1, 0,
		&dec2_mux, tabla_codec_enable_dec, SND_SOC_DAPM_PRE_PMU),

	SND_SOC_DAPM_MUX_E("DEC3 MUX", TABLA_A_CDC_CLK_TX_CLK_EN_B1_CTL, 2, 0,
		&dec3_mux, tabla_codec_enable_dec, SND_SOC_DAPM_PRE_PMU),

	SND_SOC_DAPM_MUX_E("DEC4 MUX", TABLA_A_CDC_CLK_TX_CLK_EN_B1_CTL, 3, 0,
		&dec4_mux, tabla_codec_enable_dec, SND_SOC_DAPM_PRE_PMU),

	SND_SOC_DAPM_MUX_E("DEC5 MUX", TABLA_A_CDC_CLK_TX_CLK_EN_B1_CTL, 4, 0,
		&dec5_mux, tabla_codec_enable_dec, SND_SOC_DAPM_PRE_PMU),

	SND_SOC_DAPM_MUX_E("DEC6 MUX", TABLA_A_CDC_CLK_TX_CLK_EN_B1_CTL, 5, 0,
		&dec6_mux, tabla_codec_enable_dec, SND_SOC_DAPM_PRE_PMU),

	SND_SOC_DAPM_MUX_E("DEC7 MUX", TABLA_A_CDC_CLK_TX_CLK_EN_B1_CTL, 6, 0,
		&dec7_mux, tabla_codec_enable_dec, SND_SOC_DAPM_PRE_PMU),

	SND_SOC_DAPM_MUX_E("DEC8 MUX", TABLA_A_CDC_CLK_TX_CLK_EN_B1_CTL, 7, 0,
		&dec8_mux, tabla_codec_enable_dec, SND_SOC_DAPM_PRE_PMU),

	SND_SOC_DAPM_MUX_E("DEC9 MUX", TABLA_A_CDC_CLK_TX_CLK_EN_B2_CTL, 0, 0,
		&dec9_mux, tabla_codec_enable_dec, SND_SOC_DAPM_PRE_PMU),

	SND_SOC_DAPM_MUX_E("DEC10 MUX", TABLA_A_CDC_CLK_TX_CLK_EN_B2_CTL, 1, 0,
		&dec10_mux, tabla_codec_enable_dec, SND_SOC_DAPM_PRE_PMU),

	SND_SOC_DAPM_MUX("ANC1 MUX", SND_SOC_NOPM, 0, 0, &anc1_mux),
	SND_SOC_DAPM_MUX("ANC2 MUX", SND_SOC_NOPM, 0, 0, &anc2_mux),

	SND_SOC_DAPM_MIXER_E("ANC", SND_SOC_NOPM, 0, 0, NULL, 0,
		tabla_codec_enable_anc, SND_SOC_DAPM_PRE_PMU |
		SND_SOC_DAPM_POST_PMD),

	SND_SOC_DAPM_MUX("ANC1 FB MUX", SND_SOC_NOPM, 0, 0, &anc1_fb_mux),

	SND_SOC_DAPM_INPUT("AMIC2"),
	SND_SOC_DAPM_MICBIAS_E("MIC BIAS2 External", TABLA_A_MICB_2_CTL, 7, 0,
		tabla_codec_enable_micbias, SND_SOC_DAPM_PRE_PMU |
		SND_SOC_DAPM_POST_PMU |	SND_SOC_DAPM_POST_PMD),
	SND_SOC_DAPM_MICBIAS_E("MIC BIAS2 Internal1", TABLA_A_MICB_2_CTL, 7, 0,
		tabla_codec_enable_micbias, SND_SOC_DAPM_PRE_PMU |
		SND_SOC_DAPM_POST_PMU | SND_SOC_DAPM_POST_PMD),
	SND_SOC_DAPM_MICBIAS_E("MIC BIAS2 Internal2", TABLA_A_MICB_2_CTL, 7, 0,
		tabla_codec_enable_micbias, SND_SOC_DAPM_PRE_PMU |
		SND_SOC_DAPM_POST_PMU | SND_SOC_DAPM_POST_PMD),
	SND_SOC_DAPM_MICBIAS_E("MIC BIAS2 Internal3", TABLA_A_MICB_2_CTL, 7, 0,
		tabla_codec_enable_micbias, SND_SOC_DAPM_PRE_PMU |
		SND_SOC_DAPM_POST_PMU | SND_SOC_DAPM_POST_PMD),
	SND_SOC_DAPM_MICBIAS_E("MIC BIAS3 External", TABLA_A_MICB_3_CTL, 7, 0,
		tabla_codec_enable_micbias, SND_SOC_DAPM_PRE_PMU |
		SND_SOC_DAPM_POST_PMU | SND_SOC_DAPM_POST_PMD),
	SND_SOC_DAPM_MICBIAS_E("MIC BIAS3 Internal1", TABLA_A_MICB_3_CTL, 7, 0,
		tabla_codec_enable_micbias, SND_SOC_DAPM_PRE_PMU |
		SND_SOC_DAPM_POST_PMU | SND_SOC_DAPM_POST_PMD),
	SND_SOC_DAPM_MICBIAS_E("MIC BIAS3 Internal2", TABLA_A_MICB_3_CTL, 7, 0,
		tabla_codec_enable_micbias, SND_SOC_DAPM_PRE_PMU |
		SND_SOC_DAPM_POST_PMU | SND_SOC_DAPM_POST_PMD),
	SND_SOC_DAPM_ADC_E("ADC2", NULL, TABLA_A_TX_1_2_EN, 3, 0,
		tabla_codec_enable_adc, SND_SOC_DAPM_PRE_PMU |
		SND_SOC_DAPM_POST_PMU | SND_SOC_DAPM_POST_PMD),

	SND_SOC_DAPM_MUX("SLIM TX1 MUX", SND_SOC_NOPM, 0, 0, &sb_tx1_mux),
	SND_SOC_DAPM_AIF_OUT("SLIM TX1", "AIF1 Capture", NULL, SND_SOC_NOPM,
			0, 0),

	SND_SOC_DAPM_MUX("SLIM TX5 MUX", SND_SOC_NOPM, 0, 0, &sb_tx5_mux),
	SND_SOC_DAPM_AIF_OUT("SLIM TX5", "AIF1 Capture", NULL, SND_SOC_NOPM,
			4, 0),

	SND_SOC_DAPM_MUX("SLIM TX6 MUX", SND_SOC_NOPM, 0, 0, &sb_tx6_mux),
	SND_SOC_DAPM_AIF_OUT("SLIM TX6", "AIF1 Capture", NULL, SND_SOC_NOPM,
			5, 0),

	SND_SOC_DAPM_MUX("SLIM TX7 MUX", SND_SOC_NOPM, 0, 0, &sb_tx7_mux),
	SND_SOC_DAPM_AIF_OUT("SLIM TX7", "AIF1 Capture", NULL, SND_SOC_NOPM,
			0, 0),

	SND_SOC_DAPM_MUX("SLIM TX8 MUX", SND_SOC_NOPM, 0, 0, &sb_tx8_mux),
	SND_SOC_DAPM_AIF_OUT("SLIM TX8", "AIF1 Capture", NULL, SND_SOC_NOPM,
			0, 0),

	/* Digital Mic Inputs */
	SND_SOC_DAPM_ADC_E("DMIC1", NULL, SND_SOC_NOPM, 0, 0,
		tabla_codec_enable_dmic, SND_SOC_DAPM_PRE_PMU |
		SND_SOC_DAPM_POST_PMD),

	SND_SOC_DAPM_ADC_E("DMIC2", NULL, SND_SOC_NOPM, 0, 0,
		tabla_codec_enable_dmic, SND_SOC_DAPM_PRE_PMU |
		SND_SOC_DAPM_POST_PMD),

	SND_SOC_DAPM_ADC_E("DMIC3", NULL, SND_SOC_NOPM, 0, 0,
		tabla_codec_enable_dmic, SND_SOC_DAPM_PRE_PMU |
		SND_SOC_DAPM_POST_PMD),

	SND_SOC_DAPM_ADC_E("DMIC4", NULL, SND_SOC_NOPM, 0, 0,
		tabla_codec_enable_dmic, SND_SOC_DAPM_PRE_PMU |
		SND_SOC_DAPM_POST_PMD),

	SND_SOC_DAPM_ADC_E("DMIC5", NULL, SND_SOC_NOPM, 0, 0,
		tabla_codec_enable_dmic, SND_SOC_DAPM_PRE_PMU |
		SND_SOC_DAPM_POST_PMD),

	SND_SOC_DAPM_ADC_E("DMIC6", NULL, SND_SOC_NOPM, 0, 0,
		tabla_codec_enable_dmic, SND_SOC_DAPM_PRE_PMU |
		SND_SOC_DAPM_POST_PMD),

	/* Sidetone */
	SND_SOC_DAPM_MUX("IIR1 INP1 MUX", SND_SOC_NOPM, 0, 0, &iir1_inp1_mux),
	SND_SOC_DAPM_PGA("IIR1", TABLA_A_CDC_CLK_SD_CTL, 0, 0, NULL, 0),
};

static const struct snd_soc_dapm_route audio_map[] = {
	/* SLIMBUS Connections */

	{"SLIM TX1", NULL, "SLIM TX1 MUX"},
	{"SLIM TX1 MUX", "DEC1", "DEC1 MUX"},

	{"SLIM TX5", NULL, "SLIM TX5 MUX"},
	{"SLIM TX5 MUX", "DEC5", "DEC5 MUX"},

	{"SLIM TX6", NULL, "SLIM TX6 MUX"},
	{"SLIM TX6 MUX", "DEC6", "DEC6 MUX"},

	{"SLIM TX7", NULL, "SLIM TX7 MUX"},
	{"SLIM TX7 MUX", "DEC1", "DEC1 MUX"},
	{"SLIM TX7 MUX", "DEC2", "DEC2 MUX"},
	{"SLIM TX7 MUX", "DEC3", "DEC3 MUX"},
	{"SLIM TX7 MUX", "DEC4", "DEC4 MUX"},
	{"SLIM TX7 MUX", "DEC5", "DEC5 MUX"},
	{"SLIM TX7 MUX", "DEC6", "DEC6 MUX"},
	{"SLIM TX7 MUX", "DEC7", "DEC7 MUX"},
	{"SLIM TX7 MUX", "DEC8", "DEC8 MUX"},
	{"SLIM TX7 MUX", "DEC9", "DEC9 MUX"},
	{"SLIM TX7 MUX", "DEC10", "DEC10 MUX"},

	{"SLIM TX8", NULL, "SLIM TX8 MUX"},
	{"SLIM TX8 MUX", "DEC1", "DEC1 MUX"},
	{"SLIM TX8 MUX", "DEC2", "DEC2 MUX"},
	{"SLIM TX8 MUX", "DEC3", "DEC3 MUX"},
	{"SLIM TX8 MUX", "DEC4", "DEC4 MUX"},
	{"SLIM TX8 MUX", "DEC5", "DEC5 MUX"},
	{"SLIM TX8 MUX", "DEC6", "DEC6 MUX"},

	/* Earpiece (RX MIX1) */
	{"EAR", NULL, "EAR PA"},
	{"EAR PA", NULL, "DAC1"},
	{"DAC1", NULL, "CP"},

	{"ANC1 FB MUX", "EAR_HPH_L", "RX1 MIX1"},
	{"ANC1 FB MUX", "EAR_LINE_1", "RX2 MIX1"},
	{"ANC", NULL, "ANC1 FB MUX"},

	/* Headset (RX MIX1 and RX MIX2) */
	{"HEADPHONE", NULL, "HPHL"},
	{"HEADPHONE", NULL, "HPHR"},

	{"HPHL", NULL, "HPHL DAC"},
	{"HPHR", NULL, "HPHR DAC"},

	{"HPHL DAC", NULL, "CP"},
	{"HPHR DAC", NULL, "CP"},

	{"ANC", NULL, "ANC1 MUX"},
	{"ANC", NULL, "ANC2 MUX"},
	{"ANC1 MUX", "ADC1", "ADC1"},
	{"ANC1 MUX", "ADC2", "ADC2"},
	{"ANC1 MUX", "ADC3", "ADC3"},
	{"ANC1 MUX", "ADC4", "ADC4"},
	{"ANC2 MUX", "ADC1", "ADC1"},
	{"ANC2 MUX", "ADC2", "ADC2"},
	{"ANC2 MUX", "ADC3", "ADC3"},
	{"ANC2 MUX", "ADC4", "ADC4"},

	{"ANC", NULL, "CDC_CONN"},

	{"DAC1", "Switch", "RX1 CHAIN"},
	{"HPHL DAC", "Switch", "RX1 CHAIN"},
	{"HPHR DAC", "Switch", "RX2 CHAIN"},

	{"LINEOUT1", NULL, "LINEOUT1 PA"},
	{"LINEOUT2", NULL, "LINEOUT2 PA"},
	{"LINEOUT3", NULL, "LINEOUT3 PA"},
	{"LINEOUT4", NULL, "LINEOUT4 PA"},
	{"LINEOUT5", NULL, "LINEOUT5 PA"},

	{"LINEOUT1 PA", NULL, "LINEOUT1 DAC"},
	{"LINEOUT2 PA", NULL, "LINEOUT2 DAC"},
	{"LINEOUT3 PA", NULL, "LINEOUT3 DAC"},
	{"LINEOUT4 PA", NULL, "LINEOUT4 DAC"},
	{"LINEOUT5 PA", NULL, "LINEOUT5 DAC"},

	{"RX1 CHAIN", NULL, "RX1 MIX1"},
	{"RX2 CHAIN", NULL, "RX2 MIX1"},
	{"RX1 CHAIN", NULL, "ANC"},
	{"RX2 CHAIN", NULL, "ANC"},
	{"LINEOUT1 DAC", "Switch", "RX3 MIX1"},
	{"LINEOUT2 DAC", "Switch", "RX4 MIX1"},
	{"LINEOUT3 DAC", "Switch", "RX3 MIX1"},
	{"LINEOUT4 DAC", "Switch", "RX4 MIX1"},
	{"LINEOUT5 DAC", "Switch", "RX7 MIX1"},

	{"CP", NULL, "RX_BIAS"},
	{"LINEOUT1 DAC", NULL, "RX_BIAS"},
	{"LINEOUT2 DAC", NULL, "RX_BIAS"},
	{"LINEOUT3 DAC", NULL, "RX_BIAS"},
	{"LINEOUT4 DAC", NULL, "RX_BIAS"},

	{"RX1 MIX1", NULL, "RX1 MIX1 INP1"},
	{"RX1 MIX1", NULL, "RX1 MIX1 INP2"},
	{"RX2 MIX1", NULL, "RX2 MIX1 INP1"},
	{"RX2 MIX1", NULL, "RX2 MIX1 INP2"},
	{"RX3 MIX1", NULL, "RX3 MIX1 INP1"},
	{"RX3 MIX1", NULL, "RX3 MIX1 INP2"},
	{"RX4 MIX1", NULL, "RX4 MIX1 INP1"},
	{"RX4 MIX1", NULL, "RX4 MIX1 INP2"},
	{"RX5 MIX1", NULL, "RX5 MIX1 INP1"},
	{"RX5 MIX1", NULL, "RX5 MIX1 INP2"},
	{"RX6 MIX1", NULL, "RX6 MIX1 INP1"},
	{"RX6 MIX1", NULL, "RX6 MIX1 INP2"},
	{"RX7 MIX1", NULL, "RX7 MIX1 INP1"},
	{"RX7 MIX1", NULL, "RX7 MIX1 INP2"},

	{"RX1 MIX1 INP1", "RX1", "SLIM RX1"},
	{"RX1 MIX1 INP1", "RX2", "SLIM RX2"},
	{"RX1 MIX1 INP1", "IIR1", "IIR1"},
	{"RX1 MIX1 INP2", "RX1", "SLIM RX1"},
	{"RX1 MIX1 INP2", "RX2", "SLIM RX2"},
	{"RX1 MIX1 INP2", "IIR1", "IIR1"},
	{"RX2 MIX1 INP1", "RX1", "SLIM RX1"},
	{"RX2 MIX1 INP1", "RX2", "SLIM RX2"},
	{"RX2 MIX1 INP2", "RX1", "SLIM RX1"},
	{"RX2 MIX1 INP2", "RX2", "SLIM RX2"},
	{"RX3 MIX1 INP1", "RX1", "SLIM RX1"},
	{"RX3 MIX1 INP1", "RX2", "SLIM RX2"},
	{"RX3 MIX1 INP2", "RX1", "SLIM RX1"},
	{"RX3 MIX1 INP2", "RX2", "SLIM RX2"},
	{"RX4 MIX1 INP1", "RX1", "SLIM RX1"},
	{"RX4 MIX1 INP1", "RX2", "SLIM RX2"},
	{"RX4 MIX1 INP2", "RX1", "SLIM RX1"},
	{"RX4 MIX1 INP2", "RX2", "SLIM RX2"},
	{"RX5 MIX1 INP1", "RX1", "SLIM RX1"},
	{"RX5 MIX1 INP1", "RX2", "SLIM RX2"},
	{"RX5 MIX1 INP2", "RX1", "SLIM RX1"},
	{"RX5 MIX1 INP2", "RX2", "SLIM RX2"},
	{"RX6 MIX1 INP1", "RX1", "SLIM RX1"},
	{"RX6 MIX1 INP1", "RX2", "SLIM RX2"},
	{"RX6 MIX1 INP2", "RX1", "SLIM RX1"},
	{"RX6 MIX1 INP2", "RX2", "SLIM RX2"},
	{"RX7 MIX1 INP1", "RX1", "SLIM RX1"},
	{"RX7 MIX1 INP1", "RX2", "SLIM RX2"},
	{"RX7 MIX1 INP2", "RX1", "SLIM RX1"},
	{"RX7 MIX1 INP2", "RX2", "SLIM RX2"},

	/* Decimator Inputs */
	{"DEC1 MUX", "DMIC1", "DMIC1"},
	{"DEC1 MUX", "ADC6", "ADC6"},
	{"DEC1 MUX", NULL, "CDC_CONN"},
	{"DEC2 MUX", "DMIC2", "DMIC2"},
	{"DEC2 MUX", "ADC5", "ADC5"},
	{"DEC2 MUX", NULL, "CDC_CONN"},
	{"DEC3 MUX", "DMIC3", "DMIC3"},
	{"DEC3 MUX", "ADC4", "ADC4"},
	{"DEC3 MUX", NULL, "CDC_CONN"},
	{"DEC4 MUX", "DMIC4", "DMIC4"},
	{"DEC4 MUX", "ADC3", "ADC3"},
	{"DEC4 MUX", NULL, "CDC_CONN"},
	{"DEC5 MUX", "DMIC5", "DMIC5"},
	{"DEC5 MUX", "ADC2", "ADC2"},
	{"DEC5 MUX", NULL, "CDC_CONN"},
	{"DEC6 MUX", "DMIC6", "DMIC6"},
	{"DEC6 MUX", "ADC1", "ADC1"},
	{"DEC6 MUX", NULL, "CDC_CONN"},
	{"DEC7 MUX", "DMIC1", "DMIC1"},
	{"DEC7 MUX", "ADC6", "ADC6"},
	{"DEC7 MUX", NULL, "CDC_CONN"},
	{"DEC8 MUX", "ADC5", "ADC5"},
	{"DEC8 MUX", NULL, "CDC_CONN"},
	{"DEC9 MUX", "ADC3", "ADC3"},
	{"DEC9 MUX", NULL, "CDC_CONN"},
	{"DEC10 MUX", "ADC4", "ADC4"},
	{"DEC10 MUX", NULL, "CDC_CONN"},

	/* ADC Connections */
	{"ADC1", NULL, "AMIC1"},
	{"ADC2", NULL, "AMIC2"},
	{"ADC3", NULL, "AMIC3"},
	{"ADC4", NULL, "AMIC4"},
	{"ADC5", NULL, "AMIC5"},
	{"ADC6", NULL, "AMIC6"},

	{"IIR1", NULL, "IIR1 INP1 MUX"},
	{"IIR1 INP1 MUX", "DEC6", "DEC6 MUX"},

	{"MIC BIAS1 Internal1", NULL, "LDO_H"},
	{"MIC BIAS1 Internal2", NULL, "LDO_H"},
	{"MIC BIAS1 External", NULL, "LDO_H"},
	{"MIC BIAS2 Internal1", NULL, "LDO_H"},
	{"MIC BIAS2 Internal2", NULL, "LDO_H"},
	{"MIC BIAS2 Internal3", NULL, "LDO_H"},
	{"MIC BIAS2 External", NULL, "LDO_H"},
	{"MIC BIAS3 Internal1", NULL, "LDO_H"},
	{"MIC BIAS3 Internal2", NULL, "LDO_H"},
	{"MIC BIAS3 External", NULL, "LDO_H"},
	{"MIC BIAS4 External", NULL, "LDO_H"},
};

static int tabla_readable(unsigned int reg)
{
	return tabla_reg_readable[reg];
}

static int tabla_volatile(unsigned int reg)
{
	/* Registers lower than 0x100 are top level registers which can be
	 * written by the Tabla core driver.
	 */

	if ((reg >= TABLA_A_CDC_MBHC_EN_CTL) || (reg < 0x100))
		return 1;

	return 0;
}

#define TABLA_FORMATS (SNDRV_PCM_FMTBIT_S16_LE)
static int tabla_write(struct snd_soc_codec *codec, unsigned int reg,
	unsigned int value)
{
	int ret;
	pr_debug("%s: write reg %x val %x\n", __func__, reg, value);

	BUG_ON(reg > TABLA_MAX_REGISTER);

	if (!tabla_volatile(reg)) {
		pr_debug("writing to cache\n");
		ret = snd_soc_cache_write(codec, reg, value);
		if (ret != 0)
			dev_err(codec->dev, "Cache write to %x failed: %d\n",
				reg, ret);
	}

	return tabla_reg_write(codec->control_data, reg, value);
}
static unsigned int tabla_read(struct snd_soc_codec *codec,
				unsigned int reg)
{
	unsigned int val;
	int ret;

	BUG_ON(reg > TABLA_MAX_REGISTER);

	if (!tabla_volatile(reg) && tabla_readable(reg) &&
		reg < codec->driver->reg_cache_size) {
		pr_debug("reading from cache\n");
		ret = snd_soc_cache_read(codec, reg, &val);
		if (ret >= 0) {
			pr_debug("register %x, value %x\n", reg, val);
			return val;
		} else
			dev_err(codec->dev, "Cache read from %x failed: %d\n",
				reg, ret);
	}

	val = tabla_reg_read(codec->control_data, reg);
	pr_debug("%s: read reg %x val %x\n", __func__, reg, val);
	return val;
}

static void tabla_codec_enable_audio_mode_bandgap(struct snd_soc_codec *codec)
{
	snd_soc_write(codec, TABLA_A_BIAS_REF_CTL, 0x1C);
	snd_soc_update_bits(codec, TABLA_A_BIAS_CENTRAL_BG_CTL, 0x80,
		0x80);
	snd_soc_update_bits(codec, TABLA_A_BIAS_CENTRAL_BG_CTL, 0x04,
		0x04);
	snd_soc_update_bits(codec, TABLA_A_BIAS_CENTRAL_BG_CTL, 0x01,
		0x01);
	usleep_range(1000, 1000);
	snd_soc_update_bits(codec, TABLA_A_BIAS_CENTRAL_BG_CTL, 0x80,
		0x00);
}

static void tabla_codec_enable_bandgap(struct snd_soc_codec *codec,
	enum tabla_bandgap_type choice)
{
	struct tabla_priv *tabla = snd_soc_codec_get_drvdata(codec);

	/* TODO lock resources accessed by audio streams and threaded
	 * interrupt handlers
	 */

	pr_debug("%s, choice is %d, current is %d\n", __func__, choice,
		tabla->bandgap_type);

	if (tabla->bandgap_type == choice)
		return;

	if ((tabla->bandgap_type == TABLA_BANDGAP_OFF) &&
		(choice == TABLA_BANDGAP_AUDIO_MODE)) {
		tabla_codec_enable_audio_mode_bandgap(codec);
	} else if ((tabla->bandgap_type == TABLA_BANDGAP_AUDIO_MODE) &&
		(choice == TABLA_BANDGAP_MBHC_MODE)) {
		snd_soc_update_bits(codec, TABLA_A_BIAS_CENTRAL_BG_CTL, 0x2,
			0x2);
		snd_soc_update_bits(codec, TABLA_A_BIAS_CENTRAL_BG_CTL, 0x80,
			0x80);
		snd_soc_update_bits(codec, TABLA_A_BIAS_CENTRAL_BG_CTL, 0x4,
			0x4);
		usleep_range(1000, 1000);
		snd_soc_update_bits(codec, TABLA_A_BIAS_CENTRAL_BG_CTL, 0x80,
			0x00);
	} else if ((tabla->bandgap_type == TABLA_BANDGAP_MBHC_MODE) &&
		(choice == TABLA_BANDGAP_AUDIO_MODE)) {
		snd_soc_write(codec, TABLA_A_BIAS_CENTRAL_BG_CTL, 0x00);
		usleep_range(100, 100);
		tabla_codec_enable_audio_mode_bandgap(codec);
	} else if (choice == TABLA_BANDGAP_OFF) {
		snd_soc_write(codec, TABLA_A_BIAS_CENTRAL_BG_CTL, 0x00);
	} else {
		pr_err("%s: Error, Invalid bandgap settings\n", __func__);
	}
	tabla->bandgap_type = choice;
}

static int tabla_codec_enable_config_mode(struct snd_soc_codec *codec,
	int enable)
{
	struct tabla_priv *tabla = snd_soc_codec_get_drvdata(codec);

	if (enable) {
		snd_soc_update_bits(codec, TABLA_A_CONFIG_MODE_FREQ, 0x10, 0);
		snd_soc_write(codec, TABLA_A_BIAS_CONFIG_MODE_BG_CTL, 0x17);
		usleep_range(5, 5);
		snd_soc_update_bits(codec, TABLA_A_CONFIG_MODE_FREQ, 0x80,
			0x80);
		snd_soc_update_bits(codec, TABLA_A_CONFIG_MODE_TEST, 0x80,
			0x80);
		usleep_range(10, 10);
		snd_soc_update_bits(codec, TABLA_A_CONFIG_MODE_TEST, 0x80, 0);
		usleep_range(20, 20);
		snd_soc_update_bits(codec, TABLA_A_CLK_BUFF_EN1, 0x08, 0x08);
	} else {
		snd_soc_update_bits(codec, TABLA_A_BIAS_CONFIG_MODE_BG_CTL, 0x1,
			0);
		snd_soc_update_bits(codec, TABLA_A_CONFIG_MODE_FREQ, 0x80, 0);
	}
	tabla->config_mode_active = enable ? true : false;

	return 0;
}

static int tabla_codec_enable_clock_block(struct snd_soc_codec *codec,
	int config_mode)
{
	struct tabla_priv *tabla = snd_soc_codec_get_drvdata(codec);

	pr_debug("%s\n", __func__);

	if (config_mode) {
		tabla_codec_enable_config_mode(codec, 1);
		snd_soc_write(codec, TABLA_A_CLK_BUFF_EN2, 0x00);
		snd_soc_write(codec, TABLA_A_CLK_BUFF_EN2, 0x02);
		snd_soc_write(codec, TABLA_A_CLK_BUFF_EN1, 0x0D);
		usleep_range(1000, 1000);
	} else
		snd_soc_update_bits(codec, TABLA_A_CLK_BUFF_EN1, 0x08, 0x00);

	if (!config_mode && tabla->mbhc_polling_active) {
		snd_soc_write(codec, TABLA_A_CLK_BUFF_EN2, 0x02);
		tabla_codec_enable_config_mode(codec, 0);

	}

	snd_soc_update_bits(codec, TABLA_A_CLK_BUFF_EN1, 0x05, 0x05);
	snd_soc_update_bits(codec, TABLA_A_CLK_BUFF_EN2, 0x02, 0x00);
	snd_soc_update_bits(codec, TABLA_A_CLK_BUFF_EN2, 0x04, 0x04);
	snd_soc_update_bits(codec, TABLA_A_CDC_CLK_MCLK_CTL, 0x01, 0x01);
	usleep_range(50, 50);
	tabla->clock_active = true;
	return 0;
}
static void tabla_codec_disable_clock_block(struct snd_soc_codec *codec)
{
	struct tabla_priv *tabla = snd_soc_codec_get_drvdata(codec);
	pr_debug("%s\n", __func__);
	snd_soc_update_bits(codec, TABLA_A_CLK_BUFF_EN2, 0x04, 0x00);
	ndelay(160);
	snd_soc_update_bits(codec, TABLA_A_CLK_BUFF_EN2, 0x02, 0x02);
	snd_soc_update_bits(codec, TABLA_A_CLK_BUFF_EN1, 0x05, 0x00);
	tabla->clock_active = false;
}

static void tabla_codec_calibrate_hs_polling(struct snd_soc_codec *codec)
{
	/* TODO store register values in calibration */

	snd_soc_write(codec, TABLA_A_CDC_MBHC_VOLT_B10_CTL, 0xFF);
	snd_soc_write(codec, TABLA_A_CDC_MBHC_VOLT_B9_CTL, 0x00);

	snd_soc_write(codec, TABLA_A_CDC_MBHC_VOLT_B4_CTL, 0x08);
	snd_soc_write(codec, TABLA_A_CDC_MBHC_VOLT_B3_CTL, 0xEE);
	snd_soc_write(codec, TABLA_A_CDC_MBHC_VOLT_B2_CTL, 0xFC);
	snd_soc_write(codec, TABLA_A_CDC_MBHC_VOLT_B1_CTL, 0xCE);

	snd_soc_write(codec, TABLA_A_CDC_MBHC_TIMER_B1_CTL, 3);
	snd_soc_write(codec, TABLA_A_CDC_MBHC_TIMER_B2_CTL, 9);
	snd_soc_write(codec, TABLA_A_CDC_MBHC_TIMER_B3_CTL, 30);
	snd_soc_write(codec, TABLA_A_CDC_MBHC_TIMER_B6_CTL, 120);
	snd_soc_update_bits(codec, TABLA_A_CDC_MBHC_TIMER_B1_CTL, 0x78, 0x58);
	snd_soc_write(codec, TABLA_A_CDC_MBHC_B2_CTL, 11);
}

static int tabla_startup(struct snd_pcm_substream *substream,
		struct snd_soc_dai *dai)
{
	pr_debug("%s(): substream = %s  stream = %d\n" , __func__,
		 substream->name, substream->stream);

	return 0;
}

static void tabla_shutdown(struct snd_pcm_substream *substream,
		struct snd_soc_dai *dai)
{
	pr_debug("%s(): substream = %s  stream = %d\n" , __func__,
		 substream->name, substream->stream);
}

int tabla_mclk_enable(struct snd_soc_codec *codec, int mclk_enable)
{
	struct tabla_priv *tabla = snd_soc_codec_get_drvdata(codec);

	pr_debug("%s() mclk_enable = %u\n", __func__, mclk_enable);

	if (mclk_enable) {
		tabla->mclk_enabled = true;

		if (tabla->mbhc_polling_active && (tabla->mclk_enabled)) {
			tabla_codec_pause_hs_polling(codec);
			tabla_codec_enable_bandgap(codec,
					TABLA_BANDGAP_AUDIO_MODE);
			tabla_codec_enable_clock_block(codec, 0);
			tabla_codec_calibrate_hs_polling(codec);
			tabla_codec_start_hs_polling(codec);
		}
	} else {

		if (!tabla->mclk_enabled) {
			pr_err("Error, MCLK already diabled\n");
			return -EINVAL;
		}
		tabla->mclk_enabled = false;

		if (tabla->mbhc_polling_active) {
			if (!tabla->mclk_enabled) {
				tabla_codec_pause_hs_polling(codec);
				tabla_codec_enable_bandgap(codec,
					TABLA_BANDGAP_MBHC_MODE);
				tabla_enable_rx_bias(codec, 1);
				tabla_codec_enable_clock_block(codec, 1);
				tabla_codec_calibrate_hs_polling(codec);
				tabla_codec_start_hs_polling(codec);
			}
			snd_soc_update_bits(codec, TABLA_A_CLK_BUFF_EN1,
					0x05, 0x01);
		}
	}
	return 0;
}

static int tabla_digital_mute(struct snd_soc_dai *codec_dai, int mute)
{
	struct snd_soc_codec *codec = codec_dai->codec;

	pr_debug("%s %d\n", __func__, mute);

	/* TODO mute TX */
	if (mute)
		snd_soc_update_bits(codec, TABLA_A_CDC_RX1_B6_CTL, 0x01, 0x01);
	else
		snd_soc_update_bits(codec, TABLA_A_CDC_RX1_B6_CTL, 0x01, 0x00);

	return 0;
}

static int tabla_set_dai_sysclk(struct snd_soc_dai *dai,
		int clk_id, unsigned int freq, int dir)
{
	pr_debug("%s\n", __func__);
	return 0;
}

static int tabla_set_dai_fmt(struct snd_soc_dai *dai, unsigned int fmt)
{
	pr_debug("%s\n", __func__);
	return 0;
}

static int tabla_hw_params(struct snd_pcm_substream *substream,
			    struct snd_pcm_hw_params *params,
			    struct snd_soc_dai *dai)
{
	struct snd_soc_codec *codec = dai->codec;
	u8 path, shift;
	u16 tx_fs_reg, rx_fs_reg;
	u8 tx_fs_rate, rx_fs_rate, rx_state, tx_state;

	pr_debug("%s: DAI-ID %x\n", __func__, dai->id);

	switch (params_rate(params)) {
	case 8000:
		tx_fs_rate = 0x00;
		rx_fs_rate = 0x00;
		break;
	case 16000:
		tx_fs_rate = 0x01;
		rx_fs_rate = 0x20;
		break;
	case 32000:
		tx_fs_rate = 0x02;
		rx_fs_rate = 0x40;
		break;
	case 48000:
		tx_fs_rate = 0x03;
		rx_fs_rate = 0x60;
		break;
	default:
		pr_err("%s: Invalid sampling rate %d\n", __func__,
				params_rate(params));
		return -EINVAL;
	}


	/**
	 * If current dai is a tx dai, set sample rate to
	 * all the txfe paths that are currently not active
	 */
	if (dai->id == TABLA_TX_DAI_ID) {

		tx_state = snd_soc_read(codec,
				TABLA_A_CDC_CLK_TX_CLK_EN_B1_CTL);

		for (path = 1, shift = 0;
				path <= NUM_DECIMATORS; path++, shift++) {

			if (path == BITS_PER_REG + 1) {
				shift = 0;
				tx_state = snd_soc_read(codec,
					TABLA_A_CDC_CLK_TX_CLK_EN_B2_CTL);
			}

			if (!(tx_state & (1 << shift))) {
				tx_fs_reg = TABLA_A_CDC_TX1_CLK_FS_CTL
						+ (BITS_PER_REG*(path-1));
				snd_soc_update_bits(codec, tx_fs_reg,
							0x03, tx_fs_rate);
			}
		}
	}

	/**
	 * TODO: Need to handle case where same RX chain takes 2 or more inputs
	 * with varying sample rates
	 */

	/**
	 * If current dai is a rx dai, set sample rate to
	 * all the rx paths that are currently not active
	 */
	if (dai->id == TABLA_RX_DAI_ID) {

		rx_state = snd_soc_read(codec,
			TABLA_A_CDC_CLK_RX_B1_CTL);

		for (path = 1, shift = 0;
				path <= NUM_INTERPOLATORS; path++, shift++) {

			if (!(rx_state & (1 << shift))) {
				rx_fs_reg = TABLA_A_CDC_RX1_B5_CTL
						+ (BITS_PER_REG*(path-1));
				snd_soc_update_bits(codec, rx_fs_reg,
						0xE0, rx_fs_rate);
			}
		}
	}

	return 0;
}

static struct snd_soc_dai_ops tabla_dai_ops = {
	.startup = tabla_startup,
	.shutdown = tabla_shutdown,
	.hw_params = tabla_hw_params,
	.set_sysclk = tabla_set_dai_sysclk,
	.set_fmt = tabla_set_dai_fmt,
	.digital_mute = tabla_digital_mute,
};

static struct snd_soc_dai_driver tabla_dai[] = {
	{
		.name = "tabla_rx1",
		.id = 1,
		.playback = {
			.stream_name = "AIF1 Playback",
			.rates = WCD9310_RATES,
			.formats = TABLA_FORMATS,
			.rate_max = 48000,
			.rate_min = 8000,
			.channels_min = 1,
			.channels_max = 2,
		},
		.ops = &tabla_dai_ops,
	},
	{
		.name = "tabla_tx1",
		.id = 2,
		.capture = {
			.stream_name = "AIF1 Capture",
			.rates = WCD9310_RATES,
			.formats = TABLA_FORMATS,
			.rate_max = 48000,
			.rate_min = 8000,
			.channels_min = 1,
			.channels_max = 2,
		},
		.ops = &tabla_dai_ops,
	},
};
static short tabla_codec_read_sta_result(struct snd_soc_codec *codec)
{
	u8 bias_msb, bias_lsb;
	short bias_value;

	bias_msb = snd_soc_read(codec, TABLA_A_CDC_MBHC_B3_STATUS);
	bias_lsb = snd_soc_read(codec, TABLA_A_CDC_MBHC_B2_STATUS);
	bias_value = (bias_msb << 8) | bias_lsb;
	return bias_value;
}

static short tabla_codec_read_dce_result(struct snd_soc_codec *codec)
{
	u8 bias_msb, bias_lsb;
	short bias_value;

	bias_msb = snd_soc_read(codec, TABLA_A_CDC_MBHC_B5_STATUS);
	bias_lsb = snd_soc_read(codec, TABLA_A_CDC_MBHC_B4_STATUS);
	bias_value = (bias_msb << 8) | bias_lsb;
	return bias_value;
}

static short tabla_codec_measure_micbias_voltage(struct snd_soc_codec *codec,
	int dce)
{
	short bias_value;

	if (dce) {
		snd_soc_update_bits(codec, TABLA_A_CDC_MBHC_CLK_CTL, 0x8, 0x8);
		snd_soc_write(codec, TABLA_A_CDC_MBHC_EN_CTL, 0x4);
		snd_soc_update_bits(codec, TABLA_A_CDC_MBHC_CLK_CTL, 0x8, 0x0);
		snd_soc_write(codec, TABLA_A_CDC_MBHC_EN_CTL, 0x4);
		usleep_range(60000, 60000);
		bias_value = tabla_codec_read_dce_result(codec);
	} else {
		snd_soc_update_bits(codec, TABLA_A_CDC_MBHC_CLK_CTL, 0x8, 0x8);
		snd_soc_write(codec, TABLA_A_CDC_MBHC_EN_CTL, 0x2);
		snd_soc_update_bits(codec, TABLA_A_CDC_MBHC_CLK_CTL, 0x8, 0x0);
		usleep_range(5000, 5000);
		snd_soc_write(codec, TABLA_A_CDC_MBHC_EN_CTL, 0x2);
		usleep_range(50, 50);
		bias_value = tabla_codec_read_sta_result(codec);
		snd_soc_update_bits(codec, TABLA_A_CDC_MBHC_CLK_CTL, 0x8, 0x8);
		snd_soc_write(codec, TABLA_A_CDC_MBHC_EN_CTL, 0x0);
	}

	pr_debug("read microphone bias value %x\n", bias_value);
	return bias_value;
}

static short tabla_codec_setup_hs_polling(struct snd_soc_codec *codec)
{
	struct tabla_priv *tabla = snd_soc_codec_get_drvdata(codec);
	struct tabla_mbhc_calibration *calibration = tabla->calibration;
	int micbias_ctl_reg, micbias_cfilt_ctl_reg,
		micbias_mbhc_reg;
	short bias_value;
	unsigned int cfilt_sel;

	if (!calibration) {
		pr_err("Error, no tabla calibration\n");
		return -ENODEV;
	}

	tabla->mbhc_polling_active = true;

	if (!tabla->mclk_enabled) {
		tabla_codec_enable_bandgap(codec, TABLA_BANDGAP_MBHC_MODE);
		tabla_enable_rx_bias(codec, 1);
		tabla_codec_enable_clock_block(codec, 1);
	}

	snd_soc_update_bits(codec, TABLA_A_CLK_BUFF_EN1, 0x05, 0x01);

	snd_soc_update_bits(codec, TABLA_A_TX_COM_BIAS, 0xE0, 0xE0);

	/* select cfilt separately from the micbias line in the platform data */
	switch (calibration->bias) {
	case TABLA_MICBIAS1:
		micbias_ctl_reg = TABLA_A_MICB_1_CTL;
		cfilt_sel = tabla->pdata->micbias.bias1_cfilt_sel;
		micbias_mbhc_reg = TABLA_A_MICB_1_MBHC;
		break;
	case TABLA_MICBIAS2:
		micbias_ctl_reg = TABLA_A_MICB_2_CTL;
		cfilt_sel = tabla->pdata->micbias.bias2_cfilt_sel;
		micbias_mbhc_reg = TABLA_A_MICB_2_MBHC;
		break;
	case TABLA_MICBIAS3:
		micbias_ctl_reg = TABLA_A_MICB_3_CTL;
		cfilt_sel = tabla->pdata->micbias.bias3_cfilt_sel;
		micbias_mbhc_reg = TABLA_A_MICB_3_MBHC;
		break;
	case TABLA_MICBIAS4:
		pr_err("%s: Error, microphone bias 4 not supported\n",
			__func__);
		return -EINVAL;
	default:
		pr_err("Error, invalid mic bias line\n");
		return -EINVAL;
	}

	switch (cfilt_sel) {
	case TABLA_CFILT1_SEL:
		micbias_cfilt_ctl_reg = TABLA_A_MICB_CFILT_1_CTL;
		break;
	case TABLA_CFILT2_SEL:
		micbias_cfilt_ctl_reg = TABLA_A_MICB_CFILT_2_CTL;
		break;
	case TABLA_CFILT3_SEL:
		micbias_cfilt_ctl_reg = TABLA_A_MICB_CFILT_3_CTL;
		break;
	default: /* default should not happen as check should have been done */
		return -EINVAL;
	}

	snd_soc_update_bits(codec, micbias_cfilt_ctl_reg, 0x70, 0x00);

	snd_soc_update_bits(codec, micbias_ctl_reg, 0x1F, 0x16);

	snd_soc_update_bits(codec, TABLA_A_CDC_MBHC_CLK_CTL, 0x2, 0x2);
	snd_soc_write(codec, TABLA_A_MBHC_SCALING_MUX_1, 0x84);

	snd_soc_update_bits(codec, TABLA_A_TX_7_MBHC_EN, 0x80, 0x80);
	snd_soc_update_bits(codec, TABLA_A_TX_7_MBHC_EN, 0x1F, 0x1C);
	snd_soc_update_bits(codec, TABLA_A_TX_7_MBHC_TEST_CTL, 0x40, 0x40);

	snd_soc_update_bits(codec, TABLA_A_TX_7_MBHC_EN, 0x80, 0x00);
	snd_soc_update_bits(codec, TABLA_A_CDC_MBHC_CLK_CTL, 0x8, 0x8);
	snd_soc_update_bits(codec, TABLA_A_CDC_MBHC_CLK_CTL, 0x8, 0x00);

	snd_soc_update_bits(codec, TABLA_A_CDC_MBHC_B1_CTL, 0x6, 0x6);
	snd_soc_update_bits(codec, TABLA_A_CDC_MBHC_CLK_CTL, 0x8, 0x8);

	snd_soc_update_bits(codec, micbias_mbhc_reg, 0x10, 0x10);
	snd_soc_update_bits(codec, TABLA_A_MBHC_HPH, 0x13, 0x01);

	tabla_codec_calibrate_hs_polling(codec);

	bias_value = tabla_codec_measure_micbias_voltage(codec, 0);
	snd_soc_update_bits(codec, micbias_cfilt_ctl_reg, 0x40, 0x40);
	snd_soc_update_bits(codec, TABLA_A_MBHC_HPH, 0x13, 0x00);

	return bias_value;
}

static int tabla_codec_enable_hs_detect(struct snd_soc_codec *codec,
		int insertion)
{
	struct tabla_priv *tabla = snd_soc_codec_get_drvdata(codec);
	struct tabla_mbhc_calibration *calibration = tabla->calibration;
	int central_bias_enabled = 0;
	int micbias_int_reg, micbias_ctl_reg, micbias_mbhc_reg;

	if (!calibration) {
		pr_err("Error, no tabla calibration\n");
		return -EINVAL;
	}

	snd_soc_update_bits(codec, TABLA_A_CDC_MBHC_INT_CTL, 0x1, 0);

	if (insertion)
		snd_soc_update_bits(codec, TABLA_A_CDC_MBHC_INT_CTL, 0x2, 0);
	else
		snd_soc_update_bits(codec, TABLA_A_CDC_MBHC_INT_CTL, 0x2, 0x2);

	if (snd_soc_read(codec, TABLA_A_CDC_MBHC_B1_CTL) & 0x4) {
		if (!(tabla->clock_active)) {
			tabla_codec_enable_config_mode(codec, 1);
			snd_soc_update_bits(codec, TABLA_A_CDC_MBHC_B1_CTL,
				0x06, 0);
			usleep_range(calibration->shutdown_plug_removal,
				calibration->shutdown_plug_removal);
			tabla_codec_enable_config_mode(codec, 0);
		} else
			snd_soc_update_bits(codec, TABLA_A_CDC_MBHC_B1_CTL,
				0x06, 0);
	}

	snd_soc_update_bits(codec, TABLA_A_MBHC_HPH, 0xC,
		calibration->hph_current << 2);

	/* Turn off HPH PAs during insertion detection to avoid false
	 * insertion interrupts
	 */
	snd_soc_update_bits(codec, TABLA_A_RX_HPH_CNP_EN, 0x30, 0x00);
	snd_soc_update_bits(codec, TABLA_A_MBHC_HPH, 0x13, 0x13);

	switch (calibration->bias) {
	case TABLA_MICBIAS1:
		micbias_mbhc_reg = TABLA_A_MICB_1_MBHC;
		micbias_int_reg = TABLA_A_MICB_1_INT_RBIAS;
		micbias_ctl_reg = TABLA_A_MICB_1_CTL;
		break;
	case TABLA_MICBIAS2:
		micbias_mbhc_reg = TABLA_A_MICB_2_MBHC;
		micbias_int_reg = TABLA_A_MICB_2_INT_RBIAS;
		micbias_ctl_reg = TABLA_A_MICB_2_CTL;
		break;
	case TABLA_MICBIAS3:
		micbias_mbhc_reg = TABLA_A_MICB_3_MBHC;
		micbias_int_reg = TABLA_A_MICB_3_INT_RBIAS;
		micbias_ctl_reg = TABLA_A_MICB_3_CTL;
		break;
	case TABLA_MICBIAS4:
		micbias_mbhc_reg = TABLA_A_MICB_4_MBHC;
		micbias_int_reg = TABLA_A_MICB_4_INT_RBIAS;
		micbias_ctl_reg = TABLA_A_MICB_4_CTL;
		break;
	default:
		pr_err("Error, invalid mic bias line\n");
		return -EINVAL;
	}
	snd_soc_update_bits(codec, micbias_int_reg, 0x80, 0);
	snd_soc_update_bits(codec, micbias_ctl_reg, 0x1, 0);

	/* If central bandgap disabled */
	if (!(snd_soc_read(codec, TABLA_A_PIN_CTL_OE1) & 1)) {
		snd_soc_update_bits(codec, TABLA_A_PIN_CTL_OE1, 0x3, 0x3);
		usleep_range(calibration->bg_fast_settle,
			calibration->bg_fast_settle);
		central_bias_enabled = 1;
	}

	/* If LDO_H disabled */
	if (snd_soc_read(codec, TABLA_A_PIN_CTL_OE0) & 0x80) {
		snd_soc_update_bits(codec, TABLA_A_PIN_CTL_OE0, 0x10, 0);
		snd_soc_update_bits(codec, TABLA_A_PIN_CTL_OE0, 0x80, 0x80);
		usleep_range(calibration->tldoh, calibration->tldoh);
		snd_soc_update_bits(codec, TABLA_A_PIN_CTL_OE0, 0x80, 0);

		if (central_bias_enabled)
			snd_soc_update_bits(codec, TABLA_A_PIN_CTL_OE1, 0x1, 0);
	}
	snd_soc_update_bits(codec, micbias_mbhc_reg, 0x60,
		calibration->mic_current << 5);
	snd_soc_update_bits(codec, micbias_mbhc_reg, 0x80, 0x80);
	usleep_range(calibration->mic_pid, calibration->mic_pid);

	snd_soc_update_bits(codec, micbias_mbhc_reg, 0x10, 0x10);

	snd_soc_update_bits(codec, TABLA_A_MICB_4_MBHC, 0x3, calibration->bias);

	tabla_enable_irq(codec->control_data, TABLA_IRQ_MBHC_INSERTION);
	snd_soc_update_bits(codec, TABLA_A_CDC_MBHC_INT_CTL, 0x1, 0x1);
	return 0;
}

static void btn0_lpress_fn(struct work_struct *work)
{
	struct delayed_work *delayed_work;
	struct tabla_priv *tabla;

	pr_debug("%s:\n", __func__);

	delayed_work = to_delayed_work(work);
	tabla = container_of(delayed_work, struct tabla_priv, btn0_dwork);

	if (tabla) {
		if (tabla->button_jack) {
			pr_debug("%s: Reporting long button press event\n",
					__func__);
			snd_soc_jack_report(tabla->button_jack, SND_JACK_BTN_0,
					SND_JACK_BTN_0);
		}
	} else {
		pr_err("%s: Bad tabla private data\n", __func__);
	}

}
int tabla_hs_detect(struct snd_soc_codec *codec,
	struct snd_soc_jack *headset_jack, struct snd_soc_jack *button_jack,
	struct tabla_mbhc_calibration *calibration)
{
	struct tabla_priv *tabla;
	if (!codec || !calibration) {
		pr_err("Error: no codec or calibration\n");
		return -EINVAL;
	}
	tabla = snd_soc_codec_get_drvdata(codec);
	tabla->headset_jack = headset_jack;
	tabla->button_jack = button_jack;
	tabla->calibration = calibration;

	INIT_DELAYED_WORK(&tabla->btn0_dwork, btn0_lpress_fn);
	return tabla_codec_enable_hs_detect(codec, 1);
}
EXPORT_SYMBOL_GPL(tabla_hs_detect);

static irqreturn_t tabla_dce_handler(int irq, void *data)
{
	struct tabla_priv *priv = data;
	struct snd_soc_codec *codec = priv->codec;
	short bias_value;

	tabla_disable_irq(codec->control_data, TABLA_IRQ_MBHC_REMOVAL);
	tabla_disable_irq(codec->control_data, TABLA_IRQ_MBHC_POTENTIAL);

	bias_value = tabla_codec_read_dce_result(codec);
	pr_debug("%s: button press interrupt, bias value is %d\n",
			__func__, bias_value);

	/*
	 * TODO: If button pressed is not button 0,
	 * report the button press event immediately.
	 */
	priv->buttons_pressed |= SND_JACK_BTN_0;

	snd_soc_write(codec, TABLA_A_CDC_MBHC_VOLT_B4_CTL, 0x09);
	msleep(100);

	schedule_delayed_work(&priv->btn0_dwork, msecs_to_jiffies(400));

	return IRQ_HANDLED;
}

static irqreturn_t tabla_release_handler(int irq, void *data)
{
	struct tabla_priv *priv = data;
	struct snd_soc_codec *codec = priv->codec;
	int ret, mic_voltage;

	pr_debug("%s\n", __func__);
	tabla_disable_irq(codec->control_data, TABLA_IRQ_MBHC_RELEASE);

	if (priv->buttons_pressed & SND_JACK_BTN_0) {
		ret = cancel_delayed_work(&priv->btn0_dwork);

		if (ret == 0) {

			pr_debug("%s: Reporting long button release event\n",
					__func__);
			if (priv->button_jack) {
				snd_soc_jack_report(priv->button_jack, 0,
					SND_JACK_BTN_0);
			}

		} else {

			mic_voltage =
				tabla_codec_measure_micbias_voltage(codec, 0);
			pr_debug("%s: Microphone Voltage on release = %d\n",
						__func__, mic_voltage);

			if (mic_voltage < -2000 || mic_voltage > -670) {
				pr_debug("%s: Fake buttton press interrupt\n",
						__func__);
			} else {

				if (priv->button_jack) {
					pr_debug("%s:reporting short button press and release\n",
							__func__);

					snd_soc_jack_report(priv->button_jack,
						SND_JACK_BTN_0, SND_JACK_BTN_0);
					snd_soc_jack_report(priv->button_jack,
						0, SND_JACK_BTN_0);
				}
			}
		}

		priv->buttons_pressed &= ~SND_JACK_BTN_0;
	}

	snd_soc_write(codec, TABLA_A_CDC_MBHC_VOLT_B4_CTL, 0x08);
	tabla_codec_start_hs_polling(codec);

	return IRQ_HANDLED;
}

static void tabla_codec_shutdown_hs_removal_detect(struct snd_soc_codec *codec)
{
	struct tabla_priv *tabla = snd_soc_codec_get_drvdata(codec);
	struct tabla_mbhc_calibration *calibration = tabla->calibration;
	int micbias_mbhc_reg;

	if (!tabla->mclk_enabled && !tabla->mbhc_polling_active)
		tabla_codec_enable_config_mode(codec, 1);

	snd_soc_update_bits(codec, TABLA_A_CDC_MBHC_CLK_CTL, 0x2, 0x2);
	snd_soc_update_bits(codec, TABLA_A_CDC_MBHC_B1_CTL, 0x6, 0x0);

	switch (calibration->bias) {
	case TABLA_MICBIAS1:
		micbias_mbhc_reg = TABLA_A_MICB_1_MBHC;
		break;
	case TABLA_MICBIAS2:
		micbias_mbhc_reg = TABLA_A_MICB_2_MBHC;
		break;
	case TABLA_MICBIAS3:
		micbias_mbhc_reg = TABLA_A_MICB_3_MBHC;
		break;
	case TABLA_MICBIAS4:
		micbias_mbhc_reg = TABLA_A_MICB_4_MBHC;
		break;
	default:
		pr_err("Error, invalid mic bias line\n");
		return;
	}
	snd_soc_update_bits(codec, micbias_mbhc_reg, 0x80, 0x00);
	usleep_range(calibration->shutdown_plug_removal,
		calibration->shutdown_plug_removal);

	snd_soc_update_bits(codec, TABLA_A_CDC_MBHC_CLK_CTL, 0xA, 0x8);
	if (!tabla->mclk_enabled && !tabla->mbhc_polling_active)
		tabla_codec_enable_config_mode(codec, 0);

	snd_soc_write(codec, TABLA_A_MBHC_SCALING_MUX_1, 0x00);
}

static void tabla_codec_shutdown_hs_polling(struct snd_soc_codec *codec)
{
	struct tabla_priv *tabla = snd_soc_codec_get_drvdata(codec);

	tabla_codec_shutdown_hs_removal_detect(codec);

	if (!tabla->mclk_enabled) {
		snd_soc_update_bits(codec, TABLA_A_TX_COM_BIAS, 0xE0, 0x00);
		tabla_codec_enable_bandgap(codec, TABLA_BANDGAP_AUDIO_MODE);
		tabla_codec_enable_clock_block(codec, 0);
	}

	tabla->mbhc_polling_active = false;
}

static irqreturn_t tabla_hs_insert_irq(int irq, void *data)
{
	struct tabla_priv *priv = data;
	struct snd_soc_codec *codec = priv->codec;
	int ldo_h_on, micb_cfilt_on;
	int micbias_cfilt_ctl_reg, cfilt_sel;
	short mic_voltage;
	short threshold_no_mic = 0xF7F6;
	short threshold_fake_insert = 0xFD30;


	pr_debug("%s\n", __func__);
	tabla_disable_irq(codec->control_data, TABLA_IRQ_MBHC_INSERTION);

	switch (priv->calibration->bias) {
	case TABLA_MICBIAS1:
		cfilt_sel = priv->pdata->micbias.bias1_cfilt_sel;
		break;
	case TABLA_MICBIAS2:
		cfilt_sel = priv->pdata->micbias.bias2_cfilt_sel;
		break;
	case TABLA_MICBIAS3:
		cfilt_sel = priv->pdata->micbias.bias3_cfilt_sel;
		break;
	default:
		pr_err("%s: Error, invalid mic bias line, bias value = %d\n",
			__func__, priv->calibration->bias);
		return IRQ_HANDLED;
	}

	switch (cfilt_sel) {
	case TABLA_CFILT1_SEL:
		micbias_cfilt_ctl_reg = TABLA_A_MICB_CFILT_1_CTL;
		break;
	case TABLA_CFILT2_SEL:
		micbias_cfilt_ctl_reg = TABLA_A_MICB_CFILT_2_CTL;
		break;
	case TABLA_CFILT3_SEL:
		micbias_cfilt_ctl_reg = TABLA_A_MICB_CFILT_3_CTL;
		break;
	default: /* default should not happen as check should have been done */
		pr_err("%s: Invalid cfilt select, cfilt_sel = %d\n",
			__func__, cfilt_sel);
		return IRQ_HANDLED;
	}

	ldo_h_on = snd_soc_read(codec, TABLA_A_LDO_H_MODE_1) & 0x80;
	micb_cfilt_on = snd_soc_read(codec, micbias_cfilt_ctl_reg) & 0x80;

	if (!ldo_h_on)
		snd_soc_update_bits(codec, TABLA_A_LDO_H_MODE_1, 0x80, 0x80);
	if (!micb_cfilt_on)
		snd_soc_update_bits(codec, micbias_cfilt_ctl_reg, 0x80, 0x80);

	usleep_range(priv->calibration->setup_plug_removal_delay,
		priv->calibration->setup_plug_removal_delay);

	if (snd_soc_read(codec, TABLA_A_CDC_MBHC_INT_CTL) & 0x02) {
		if (priv->headset_jack) {
			pr_debug("%s: Reporting removal\n", __func__);
			snd_soc_jack_report(priv->headset_jack, 0,
				SND_JACK_HEADSET);
		}
		tabla_codec_shutdown_hs_removal_detect(codec);
		tabla_codec_enable_hs_detect(codec, 1);
		return IRQ_HANDLED;
	}

	if (!ldo_h_on)
		snd_soc_update_bits(codec, TABLA_A_LDO_H_MODE_1, 0x80, 0x0);
	if (!micb_cfilt_on)
		snd_soc_update_bits(codec, micbias_cfilt_ctl_reg, 0x80, 0x0);

	mic_voltage = tabla_codec_setup_hs_polling(codec);

	if (mic_voltage > threshold_fake_insert) {
		pr_debug("%s: Fake insertion interrupt, mic_voltage = %x\n",
			__func__, mic_voltage);
		tabla_codec_enable_hs_detect(codec, 1);
	} else if (mic_voltage < threshold_no_mic) {
		pr_debug("%s: Headphone Detected, mic_voltage = %x\n",
			__func__, mic_voltage);

		if (priv->headset_jack) {
			pr_debug("%s: Reporting insertion %d\n", __func__,
				SND_JACK_HEADPHONE);
			snd_soc_jack_report(priv->headset_jack,
				SND_JACK_HEADPHONE, SND_JACK_HEADSET);
		}
		tabla_codec_shutdown_hs_polling(codec);
		tabla_codec_enable_hs_detect(codec, 0);

	} else {
		pr_debug("%s: Headset detected, mic_voltage = %x\n",
			__func__, mic_voltage);
		if (priv->headset_jack) {
			pr_debug("%s: Reporting insertion %d\n", __func__,
				SND_JACK_HEADSET);
			snd_soc_jack_report(priv->headset_jack,
				SND_JACK_HEADSET, SND_JACK_HEADSET);
		}
		tabla_codec_start_hs_polling(codec);
	}

	return IRQ_HANDLED;
}

static irqreturn_t tabla_hs_remove_irq(int irq, void *data)
{
	struct tabla_priv *priv = data;
	struct snd_soc_codec *codec = priv->codec;
	short bias_value;

	tabla_disable_irq(codec->control_data, TABLA_IRQ_MBHC_REMOVAL);
	tabla_disable_irq(codec->control_data, TABLA_IRQ_MBHC_POTENTIAL);
	tabla_disable_irq(codec->control_data, TABLA_IRQ_MBHC_RELEASE);

	usleep_range(priv->calibration->shutdown_plug_removal,
		priv->calibration->shutdown_plug_removal);

	bias_value = tabla_codec_measure_micbias_voltage(codec, 1);
	pr_debug("removal interrupt, bias value is %d\n", bias_value);

	if (bias_value < -90) {
		pr_debug("False alarm, headset not actually removed\n");
		tabla_codec_start_hs_polling(codec);
	} else {
		if (priv->headset_jack) {
			pr_debug("%s: Reporting removal\n", __func__);
			snd_soc_jack_report(priv->headset_jack, 0,
				SND_JACK_HEADSET);
		}
		tabla_codec_shutdown_hs_polling(codec);

		tabla_codec_enable_hs_detect(codec, 1);
	}
	return IRQ_HANDLED;
}

static unsigned long slimbus_value;

static irqreturn_t tabla_slimbus_irq(int irq, void *data)
{
	struct tabla_priv *priv = data;
	struct snd_soc_codec *codec = priv->codec;
	int i, j;
	u8 val;

	for (i = 0; i < TABLA_SLIM_NUM_PORT_REG; i++) {
		slimbus_value = tabla_interface_reg_read(codec->control_data,
			TABLA_SLIM_PGD_PORT_INT_STATUS0 + i);
		for_each_set_bit(j, &slimbus_value, BITS_PER_BYTE) {
			val = tabla_interface_reg_read(codec->control_data,
				TABLA_SLIM_PGD_PORT_INT_SOURCE0 + i*8 + j);
			if (val & 0x1)
				pr_err_ratelimited("overflow error on port %x,"
					" value %x\n", i*8 + j, val);
			if (val & 0x2)
				pr_err_ratelimited("underflow error on port %x,"
					" value %x\n", i*8 + j, val);
		}
		tabla_interface_reg_write(codec->control_data,
			TABLA_SLIM_PGD_PORT_INT_CLR0 + i, 0xFF);
	}

	return IRQ_HANDLED;
}

static int tabla_find_k_value(unsigned int ldoh_v, unsigned int cfilt_mv)
{
	int rc = -EINVAL;
	unsigned min_mv, max_mv;

	switch (ldoh_v) {
	case TABLA_LDOH_1P95_V:
		min_mv = 160;
		max_mv = 1800;
		break;
	case TABLA_LDOH_2P35_V:
		min_mv = 200;
		max_mv = 2200;
		break;
	case TABLA_LDOH_2P75_V:
		min_mv = 240;
		max_mv = 2600;
		break;
	case TABLA_LDOH_2P85_V:
		min_mv = 250;
		max_mv = 2700;
		break;
	default:
		goto done;
	}

	if (cfilt_mv < min_mv || cfilt_mv > max_mv)
		goto done;

	for (rc = 4; rc <= 44; rc++) {
		min_mv = max_mv * (rc) / 44;
		if (min_mv >= cfilt_mv) {
			rc -= 4;
			break;
		}
	}
done:
	return rc;
}

static int tabla_handle_pdata(struct tabla_priv *tabla)
{
	struct snd_soc_codec *codec = tabla->codec;
	struct tabla_pdata *pdata = tabla->pdata;
	int k1, k2, k3, rc = 0;

	if (!pdata) {
		rc = -ENODEV;
		goto done;
	}

	/* Make sure settings are correct */
	if ((pdata->micbias.ldoh_v > TABLA_LDOH_2P85_V) ||
	    (pdata->micbias.bias1_cfilt_sel > TABLA_CFILT3_SEL) ||
	    (pdata->micbias.bias2_cfilt_sel > TABLA_CFILT3_SEL) ||
	    (pdata->micbias.bias3_cfilt_sel > TABLA_CFILT3_SEL) ||
	    (pdata->micbias.bias4_cfilt_sel > TABLA_CFILT3_SEL)) {
		rc = -EINVAL;
		goto done;
	}

	/* figure out k value */
	k1 = tabla_find_k_value(pdata->micbias.ldoh_v,
		pdata->micbias.cfilt1_mv);
	k2 = tabla_find_k_value(pdata->micbias.ldoh_v,
		pdata->micbias.cfilt2_mv);
	k3 = tabla_find_k_value(pdata->micbias.ldoh_v,
		pdata->micbias.cfilt3_mv);

	if (IS_ERR_VALUE(k1) || IS_ERR_VALUE(k2) || IS_ERR_VALUE(k3)) {
		rc = -EINVAL;
		goto done;
	}

	/* Set voltage level and always use LDO */
	snd_soc_update_bits(codec, TABLA_A_LDO_H_MODE_1, 0x0C,
		(pdata->micbias.ldoh_v << 2));

	snd_soc_update_bits(codec, TABLA_A_MICB_CFILT_1_VAL, 0xFC,
		(k1 << 2));
	snd_soc_update_bits(codec, TABLA_A_MICB_CFILT_2_VAL, 0xFC,
		(k2 << 2));
	snd_soc_update_bits(codec, TABLA_A_MICB_CFILT_3_VAL, 0xFC,
		(k3 << 2));

	snd_soc_update_bits(codec, TABLA_A_MICB_1_CTL, 0x60,
		(pdata->micbias.bias1_cfilt_sel << 5));
	snd_soc_update_bits(codec, TABLA_A_MICB_2_CTL, 0x60,
		(pdata->micbias.bias2_cfilt_sel << 5));
	snd_soc_update_bits(codec, TABLA_A_MICB_3_CTL, 0x60,
		(pdata->micbias.bias3_cfilt_sel << 5));
	snd_soc_update_bits(codec, TABLA_A_MICB_4_CTL, 0x60,
		(pdata->micbias.bias4_cfilt_sel << 5));

done:
	return rc;
}

static const struct tabla_reg_mask_val tabla_1_1_reg_defaults[] = {

	/* Tabla 1.1 MICBIAS changes */
	TABLA_REG_VAL(TABLA_A_MICB_1_INT_RBIAS, 0x24),
	TABLA_REG_VAL(TABLA_A_MICB_2_INT_RBIAS, 0x24),
	TABLA_REG_VAL(TABLA_A_MICB_3_INT_RBIAS, 0x24),
	TABLA_REG_VAL(TABLA_A_MICB_4_INT_RBIAS, 0x24),

	/* Tabla 1.1 HPH changes */
	TABLA_REG_VAL(TABLA_A_RX_HPH_BIAS_PA, 0x57),
	TABLA_REG_VAL(TABLA_A_RX_HPH_BIAS_LDO, 0x56),

	/* Tabla 1.1 EAR PA changes */
	TABLA_REG_VAL(TABLA_A_RX_EAR_BIAS_PA, 0xA6),
	TABLA_REG_VAL(TABLA_A_RX_EAR_GAIN, 0x02),
	TABLA_REG_VAL(TABLA_A_RX_EAR_VCM, 0x03),

	/* Tabla 1.1 Lineout_5 Changes */
	TABLA_REG_VAL(TABLA_A_RX_LINE_5_GAIN, 0x10),

	/* Tabla 1.1 RX Changes */
	TABLA_REG_VAL(TABLA_A_CDC_RX1_B5_CTL, 0x78),
	TABLA_REG_VAL(TABLA_A_CDC_RX2_B5_CTL, 0x78),
	TABLA_REG_VAL(TABLA_A_CDC_RX3_B5_CTL, 0x78),
	TABLA_REG_VAL(TABLA_A_CDC_RX4_B5_CTL, 0x78),
	TABLA_REG_VAL(TABLA_A_CDC_RX5_B5_CTL, 0x78),
	TABLA_REG_VAL(TABLA_A_CDC_RX6_B5_CTL, 0x78),
	TABLA_REG_VAL(TABLA_A_CDC_RX7_B5_CTL, 0x78),

	/* Tabla 1.1 RX1 and RX2 Changes */
	TABLA_REG_VAL(TABLA_A_CDC_RX1_B6_CTL, 0xA0),
	TABLA_REG_VAL(TABLA_A_CDC_RX2_B6_CTL, 0xA0),

	/* Tabla 1.1 RX3 to RX7 Changes */
	TABLA_REG_VAL(TABLA_A_CDC_RX3_B6_CTL, 0x80),
	TABLA_REG_VAL(TABLA_A_CDC_RX4_B6_CTL, 0x80),
	TABLA_REG_VAL(TABLA_A_CDC_RX5_B6_CTL, 0x80),
	TABLA_REG_VAL(TABLA_A_CDC_RX6_B6_CTL, 0x80),
	TABLA_REG_VAL(TABLA_A_CDC_RX7_B6_CTL, 0x80),

	/* Tabla 1.1 CLASSG Changes */
	TABLA_REG_VAL(TABLA_A_CDC_CLSG_FREQ_THRESH_B3_CTL, 0x1B),
};

static const struct tabla_reg_mask_val tabla_2_0_reg_defaults[] = {

	/* Tabla 2.0 MICBIAS changes */
	TABLA_REG_VAL(TABLA_A_MICB_2_MBHC, 0x02),
};

static void tabla_update_reg_defaults(struct snd_soc_codec *codec)
{
	u32 i;

	for (i = 0; i < ARRAY_SIZE(tabla_1_1_reg_defaults); i++)
		snd_soc_write(codec, tabla_1_1_reg_defaults[i].reg,
				tabla_1_1_reg_defaults[i].val);

	for (i = 0; i < ARRAY_SIZE(tabla_2_0_reg_defaults); i++)
		snd_soc_write(codec, tabla_2_0_reg_defaults[i].reg,
				tabla_2_0_reg_defaults[i].val);
}

static const struct tabla_reg_mask_val tabla_codec_reg_init_val[] = {

	/* Initialize gain registers to use register gain */
	{TABLA_A_RX_HPH_L_GAIN, 0x10, 0x10},
	{TABLA_A_RX_HPH_R_GAIN, 0x10, 0x10},
	{TABLA_A_RX_LINE_1_GAIN, 0x10, 0x10},
	{TABLA_A_RX_LINE_2_GAIN, 0x10, 0x10},
	{TABLA_A_RX_LINE_3_GAIN, 0x10, 0x10},
	{TABLA_A_RX_LINE_4_GAIN, 0x10, 0x10},

	/* Initialize mic biases to differential mode */
	{TABLA_A_MICB_1_INT_RBIAS, 0x24, 0x24},
	{TABLA_A_MICB_2_INT_RBIAS, 0x24, 0x24},
	{TABLA_A_MICB_3_INT_RBIAS, 0x24, 0x24},
	{TABLA_A_MICB_4_INT_RBIAS, 0x24, 0x24},

	{TABLA_A_CDC_CONN_CLSG_CTL, 0x3C, 0x14},

	/* Use 16 bit sample size for TX1 to TX6 */
	{TABLA_A_CDC_CONN_TX_SB_B1_CTL, 0x30, 0x20},
	{TABLA_A_CDC_CONN_TX_SB_B2_CTL, 0x30, 0x20},
	{TABLA_A_CDC_CONN_TX_SB_B3_CTL, 0x30, 0x20},
	{TABLA_A_CDC_CONN_TX_SB_B4_CTL, 0x30, 0x20},
	{TABLA_A_CDC_CONN_TX_SB_B5_CTL, 0x30, 0x20},
	{TABLA_A_CDC_CONN_TX_SB_B6_CTL, 0x30, 0x20},

	/* Use 16 bit sample size for TX7 to TX10 */
	{TABLA_A_CDC_CONN_TX_SB_B7_CTL, 0x60, 0x40},
	{TABLA_A_CDC_CONN_TX_SB_B8_CTL, 0x60, 0x40},
	{TABLA_A_CDC_CONN_TX_SB_B9_CTL, 0x60, 0x40},
	{TABLA_A_CDC_CONN_TX_SB_B10_CTL, 0x60, 0x40},

	/* Use 16 bit sample size for RX */
	{TABLA_A_CDC_CONN_RX_SB_B1_CTL, 0xFF, 0xAA},
	{TABLA_A_CDC_CONN_RX_SB_B2_CTL, 0xFF, 0xAA},

	/*enable HPF filter for TX paths */
	{TABLA_A_CDC_TX1_MUX_CTL, 0x8, 0x0},
	{TABLA_A_CDC_TX2_MUX_CTL, 0x8, 0x0},
	{TABLA_A_CDC_TX3_MUX_CTL, 0x8, 0x0},
	{TABLA_A_CDC_TX4_MUX_CTL, 0x8, 0x0},
	{TABLA_A_CDC_TX5_MUX_CTL, 0x8, 0x0},
	{TABLA_A_CDC_TX6_MUX_CTL, 0x8, 0x0},
	{TABLA_A_CDC_TX7_MUX_CTL, 0x8, 0x0},
	{TABLA_A_CDC_TX8_MUX_CTL, 0x8, 0x0},
	{TABLA_A_CDC_TX9_MUX_CTL, 0x8, 0x0},
	{TABLA_A_CDC_TX10_MUX_CTL, 0x8, 0x0},
};

static void tabla_codec_init_reg(struct snd_soc_codec *codec)
{
	u32 i;

	for (i = 0; i < ARRAY_SIZE(tabla_codec_reg_init_val); i++)
		snd_soc_update_bits(codec, tabla_codec_reg_init_val[i].reg,
				tabla_codec_reg_init_val[i].mask,
				tabla_codec_reg_init_val[i].val);
}

static int tabla_codec_probe(struct snd_soc_codec *codec)
{
	struct tabla *control;
	struct tabla_priv *tabla;
	struct snd_soc_dapm_context *dapm = &codec->dapm;
	int ret = 0;
	int i;

	codec->control_data = dev_get_drvdata(codec->dev->parent);
	control = codec->control_data;

	tabla = kzalloc(sizeof(struct tabla_priv), GFP_KERNEL);
	if (!tabla) {
		dev_err(codec->dev, "Failed to allocate private data\n");
		return -ENOMEM;
	}

	snd_soc_codec_set_drvdata(codec, tabla);

	tabla->mclk_enabled = false;
	tabla->bandgap_type = TABLA_BANDGAP_OFF;
	tabla->clock_active = false;
	tabla->config_mode_active = false;
	tabla->mbhc_polling_active = false;
	tabla->no_mic_headset_override = false;
	tabla->codec = codec;
	tabla->pdata = dev_get_platdata(codec->dev->parent);

	ret = tabla_handle_pdata(tabla);

	if (IS_ERR_VALUE(ret)) {
		pr_err("%s: bad pdata\n", __func__);
		goto err_pdata;
	}

	tabla_update_reg_defaults(codec);
	tabla_codec_init_reg(codec);

	/* TODO only enable bandgap when necessary in order to save power */
	tabla_codec_enable_bandgap(codec, TABLA_BANDGAP_AUDIO_MODE);
	tabla_codec_enable_clock_block(codec, 0);

	snd_soc_add_controls(codec, tabla_snd_controls,
		ARRAY_SIZE(tabla_snd_controls));
	snd_soc_dapm_new_controls(dapm, tabla_dapm_widgets,
		ARRAY_SIZE(tabla_dapm_widgets));
	snd_soc_dapm_add_routes(dapm, audio_map, ARRAY_SIZE(audio_map));
	snd_soc_dapm_sync(dapm);

	ret = tabla_request_irq(codec->control_data, TABLA_IRQ_MBHC_INSERTION,
		tabla_hs_insert_irq, "Headset insert detect", tabla);
	if (ret) {
		pr_err("%s: Failed to request irq %d\n", __func__,
			TABLA_IRQ_MBHC_INSERTION);
		goto err_insert_irq;
	}
	tabla_disable_irq(codec->control_data, TABLA_IRQ_MBHC_INSERTION);

	ret = tabla_request_irq(codec->control_data, TABLA_IRQ_MBHC_REMOVAL,
		tabla_hs_remove_irq, "Headset remove detect", tabla);
	if (ret) {
		pr_err("%s: Failed to request irq %d\n", __func__,
			TABLA_IRQ_MBHC_REMOVAL);
		goto err_remove_irq;
	}
	tabla_disable_irq(codec->control_data, TABLA_IRQ_MBHC_REMOVAL);

	ret = tabla_request_irq(codec->control_data, TABLA_IRQ_MBHC_POTENTIAL,
		tabla_dce_handler, "DC Estimation detect", tabla);
	if (ret) {
		pr_err("%s: Failed to request irq %d\n", __func__,
			TABLA_IRQ_MBHC_POTENTIAL);
		goto err_potential_irq;
	}
	tabla_disable_irq(codec->control_data, TABLA_IRQ_MBHC_POTENTIAL);

	ret = tabla_request_irq(codec->control_data, TABLA_IRQ_MBHC_RELEASE,
		tabla_release_handler, "Button Release detect", tabla);
	if (ret) {
		pr_err("%s: Failed to request irq %d\n", __func__,
			TABLA_IRQ_MBHC_RELEASE);
		goto err_release_irq;
	}
	tabla_disable_irq(codec->control_data, TABLA_IRQ_MBHC_RELEASE);

	ret = tabla_request_irq(codec->control_data, TABLA_IRQ_SLIMBUS,
		tabla_slimbus_irq, "SLIMBUS Slave", tabla);
	if (ret) {
		pr_err("%s: Failed to request irq %d\n", __func__,
			TABLA_IRQ_SLIMBUS);
		goto err_slimbus_irq;
	}

	for (i = 0; i < TABLA_SLIM_NUM_PORT_REG; i++)
		tabla_interface_reg_write(codec->control_data,
			TABLA_SLIM_PGD_PORT_INT_EN0 + i, 0xFF);

#ifdef CONFIG_DEBUG_FS
	debug_tabla_priv = tabla;
#endif

	return ret;

err_slimbus_irq:
	tabla_free_irq(codec->control_data, TABLA_IRQ_MBHC_RELEASE, tabla);
err_release_irq:
	tabla_free_irq(codec->control_data, TABLA_IRQ_MBHC_POTENTIAL, tabla);
err_potential_irq:
	tabla_free_irq(codec->control_data, TABLA_IRQ_MBHC_REMOVAL, tabla);
err_remove_irq:
	tabla_free_irq(codec->control_data, TABLA_IRQ_MBHC_INSERTION, tabla);
err_insert_irq:
err_pdata:
	kfree(tabla);
	return ret;
}
static int tabla_codec_remove(struct snd_soc_codec *codec)
{
	struct tabla_priv *tabla = snd_soc_codec_get_drvdata(codec);
	tabla_free_irq(codec->control_data, TABLA_IRQ_SLIMBUS, tabla);
	tabla_free_irq(codec->control_data, TABLA_IRQ_MBHC_RELEASE, tabla);
	tabla_free_irq(codec->control_data, TABLA_IRQ_MBHC_POTENTIAL, tabla);
	tabla_free_irq(codec->control_data, TABLA_IRQ_MBHC_REMOVAL, tabla);
	tabla_free_irq(codec->control_data, TABLA_IRQ_MBHC_INSERTION, tabla);
	tabla_codec_disable_clock_block(codec);
	tabla_codec_enable_bandgap(codec, TABLA_BANDGAP_OFF);
	kfree(tabla);
	return 0;
}
static struct snd_soc_codec_driver soc_codec_dev_tabla = {
	.probe	= tabla_codec_probe,
	.remove	= tabla_codec_remove,
	.read = tabla_read,
	.write = tabla_write,

	.readable_register = tabla_readable,
	.volatile_register = tabla_volatile,

	.reg_cache_size = TABLA_CACHE_SIZE,
	.reg_cache_default = tabla_reg_defaults,
	.reg_word_size = 1,
};

#ifdef CONFIG_DEBUG_FS
static struct dentry *debugfs_poke;

static int codec_debug_open(struct inode *inode, struct file *file)
{
	file->private_data = inode->i_private;
	return 0;
}

static ssize_t codec_debug_write(struct file *filp,
	const char __user *ubuf, size_t cnt, loff_t *ppos)
{
	char lbuf[32];
	char *buf;
	int rc;

	if (cnt > sizeof(lbuf) - 1)
		return -EINVAL;

	rc = copy_from_user(lbuf, ubuf, cnt);
	if (rc)
		return -EFAULT;

	lbuf[cnt] = '\0';
	buf = (char *)lbuf;
	debug_tabla_priv->no_mic_headset_override = (*strsep(&buf, " ") == '0')
		? false : true;

	return rc;
}

static const struct file_operations codec_debug_ops = {
	.open = codec_debug_open,
	.write = codec_debug_write,
};
#endif

static int __devinit tabla_probe(struct platform_device *pdev)
{
#ifdef CONFIG_DEBUG_FS
	debugfs_poke = debugfs_create_file("TRRS",
		S_IFREG | S_IRUGO, NULL, (void *) "TRRS", &codec_debug_ops);

#endif
	return snd_soc_register_codec(&pdev->dev, &soc_codec_dev_tabla,
		tabla_dai, ARRAY_SIZE(tabla_dai));
}
static int __devexit tabla_remove(struct platform_device *pdev)
{
	snd_soc_unregister_codec(&pdev->dev);

#ifdef CONFIG_DEBUG_FS
	debugfs_remove(debugfs_poke);
#endif
	return 0;
}
static struct platform_driver tabla_codec_driver = {
	.probe = tabla_probe,
	.remove = tabla_remove,
	.driver = {
		.name = "tabla_codec",
		.owner = THIS_MODULE,
	},
};

static int __init tabla_codec_init(void)
{
	return platform_driver_register(&tabla_codec_driver);
}

static void __exit tabla_codec_exit(void)
{
	platform_driver_unregister(&tabla_codec_driver);
}

module_init(tabla_codec_init);
module_exit(tabla_codec_exit);

MODULE_DESCRIPTION("Tabla codec driver");
MODULE_VERSION("1.0");
MODULE_LICENSE("GPL v2");
