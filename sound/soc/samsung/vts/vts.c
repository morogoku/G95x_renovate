/* sound/soc/samsung/vts/vts.c
 *
 * ALSA SoC - Samsung VTS driver
 *
 * Copyright (c) 2016 Samsung Electronics Co. Ltd.
  *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */
//#define DEBUG
#include <linux/clk.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/of_reserved_mem.h>
#include <linux/pm_runtime.h>
#include <linux/firmware.h>
#include <linux/dma-mapping.h>
#include <linux/proc_fs.h>
#include <linux/delay.h>
#include <linux/regmap.h>
#include <linux/wakelock.h>

#include <asm-generic/delay.h>

#include <sound/soc.h>
#include <sound/soc-dapm.h>
#include <sound/pcm_params.h>
#include <sound/tlv.h>

#include <sound/samsung/mailbox.h>
#include <sound/samsung/vts.h>
#include <soc/samsung/exynos-pmu.h>

#include "vts.h"

#undef EMULATOR
#ifdef EMULATOR
static void __iomem *pmu_alive;
#define set_mask_value(id, mask, value) do {id = ((id & ~mask) | (value & mask));} while(0);
static void update_mask_value(volatile void __iomem *sfr,
		unsigned int mask, unsigned int value)
{
	unsigned int sfr_value = readl(sfr);
	set_mask_value(sfr_value, mask, value);
	writel(sfr_value, sfr);
}
#endif

#define VTS_CPU_CONFIGURATION			(0x24E0)
#define PAD_RETENTION_VTS_OPTION		(0x3148)
#define VTS_CPU_LOCAL_PWR_CFG			(0x00000001)
#define VTS_CPU_STATUS				(0x24E4)
#define VTS_CPU_STATUS_STANDBYWFI_MASK		(0x10000000)
#define VTS_CPU_STATUS_STATUS_MASK		(0x00000001)
#define VTS_CPU_OPTION				(0x24E8)
#define VTS_CPU_OPTION_USE_STANDBYWFI_MASK	(0x00010000)
#define VTS_CPU_OPTION_ENABLE_CPU_MASK		(0x00008000)

#define LIMIT_IN_JIFFIES (msecs_to_jiffies(1000))
#define DMIC_CLK_RATE (768000)
#define VTS_TRIGGERED_TIMEOUT_MS (5000)

/* For only external static functions */
static struct vts_data *p_vts_data;

static int vts_start_ipc_transaction_atomic(struct device *dev, struct vts_data *data, int msg, u32 (*values)[3], int sync)
{
	long result = 0;
	u32 ack_value = 0;
	volatile enum ipc_state *state = &data->ipc_state_ap;

	dev_info(dev, "%s:++ msg:%d, values: 0x%08x, 0x%08x, 0x%08x\n",
					__func__, msg, (*values)[0],
					(*values)[1], (*values)[2]);

	if (pm_runtime_suspended(dev)) {
		dev_warn(dev, "%s: VTS IP is in suspended state, IPC cann't be processed \n", __func__);
		return -EINVAL;
	}

	if (!data->vts_ready) {
		dev_warn(dev, "%s: VTS Firmware Not running\n", __func__);
		return -EINVAL;
	}

	spin_lock(&data->ipc_spinlock);

	*state = SEND_MSG;
	mailbox_write_shared_register(data->pdev_mailbox, *values, 0, 3);
	mailbox_generate_interrupt(data->pdev_mailbox, msg);
	data->running_ipc = msg;

	if (sync) {
		int i;
		for (i = 1000; i && (*state != SEND_MSG_OK) &&
				(*state != SEND_MSG_FAIL) &&
				(ack_value != (0x1 << msg)); i--) {
			mailbox_read_shared_register(data->pdev_mailbox, &ack_value, 3, 1);
			dev_dbg(dev, "%s ACK-value: 0x%08x\n", __func__, ack_value);
			udelay(50);
		}
		if (!i) {
			dev_warn(dev, "Transaction timeout!! Ack_value:0x%x\n",
					ack_value);
		}
		if (*state == SEND_MSG_OK || ack_value == (0x1 << msg)) {
			dev_dbg(dev, "Transaction success Ack_value:0x%x\n",
					ack_value);
		} else {
			dev_err(dev, "Transaction failed\n");
		}
		result = (*state == SEND_MSG_OK || ack_value) ? 0 : -EIO;
	}

	/* Clear running IPC & ACK value */
	ack_value = 0x0;
	mailbox_write_shared_register(data->pdev_mailbox, &ack_value, 3, 1);
	data->running_ipc = 0;
	*state = IDLE;

	spin_unlock(&data->ipc_spinlock);
	dev_info(dev, "%s:-- msg:%d \n", __func__, msg);

	return (int)result;
}

int vts_start_ipc_transaction(struct device *dev, struct vts_data *data,
		int msg, u32 (*values)[3], int atomic, int sync)
{
	return vts_start_ipc_transaction_atomic(dev, data, msg, values, sync);
}

static int vts_ipc_ack(struct vts_data *data, u32 result)
{
	pr_debug("%s(%p, %u)\n", __func__, data, result);
	mailbox_write_shared_register(data->pdev_mailbox, &result, 0, 1);
	mailbox_generate_interrupt(data->pdev_mailbox, VTS_IRQ_AP_IPC_RECEIVED);
	return 0;
}

static void vts_cpu_power(bool on)
{
	pr_info("%s(%d)\n", __func__, on);

#ifndef EMULATOR
	exynos_pmu_update(VTS_CPU_CONFIGURATION, VTS_CPU_LOCAL_PWR_CFG,
			on ? VTS_CPU_LOCAL_PWR_CFG : 0);
#else
	update_mask_value(pmu_alive + VTS_CPU_CONFIGURATION, VTS_CPU_LOCAL_PWR_CFG,
			on ? VTS_CPU_LOCAL_PWR_CFG : 0);
#endif

	if (!on) {
#ifndef EMULATOR
		exynos_pmu_update(VTS_CPU_OPTION,
				VTS_CPU_OPTION_USE_STANDBYWFI_MASK,
				VTS_CPU_OPTION_USE_STANDBYWFI_MASK);
#else
		update_mask_value(pmu_alive + VTS_CPU_OPTION,
				VTS_CPU_OPTION_USE_STANDBYWFI_MASK,
				VTS_CPU_OPTION_USE_STANDBYWFI_MASK);
#endif
	}
}

static int vts_cpu_enable(bool enable)
{
	unsigned int mask = VTS_CPU_OPTION_ENABLE_CPU_MASK;
	unsigned int val = (enable ? mask : 0);
	unsigned int status = 0;
	unsigned long after;

	pr_info("%s(%d)\n", __func__, enable);

#ifndef EMULATOR
	exynos_pmu_update(VTS_CPU_OPTION, mask, val);
#else
	update_mask_value(pmu_alive + VTS_CPU_OPTION, mask, val);
#endif
	if (enable) {
		after = jiffies + LIMIT_IN_JIFFIES;
		do {
			schedule();
#ifndef EMULATOR
			exynos_pmu_read(VTS_CPU_STATUS, &status);
#else
			status = readl(pmu_alive + VTS_CPU_STATUS);
#endif
		} while (((status & VTS_CPU_STATUS_STATUS_MASK)
				!= VTS_CPU_STATUS_STATUS_MASK)
				&& time_is_after_eq_jiffies(after));
		if (time_is_before_jiffies(after)) {
			pr_err("vts cpu enable timeout\n");
			return -ETIME;
		}
	}

	return 0;
}

static void vts_reset_cpu(void)
{
#ifndef EMULATOR
	vts_cpu_enable(false);
	vts_cpu_power(false);
	vts_cpu_power(true);
	vts_cpu_enable(true);
#endif
}

static int vts_download_firmware(struct platform_device *pdev)
{
	struct vts_data *data = platform_get_drvdata(pdev);
	struct device *dev = &pdev->dev;
	dev_info(dev, "%s\n", __func__);

	if (!data->firmware) {
		dev_err(dev, "firmware is not loaded\n");
		return -EAGAIN;
	}

	memcpy(data->sram_base, data->firmware->data, data->firmware->size);
	dev_info(dev, "firmware is downloaded to %p (size=%zu)\n", data->sram_base, data->firmware->size);

	return 0;
}

static int vts_wait_for_fw_ready(struct device *dev)
{
	struct vts_data *data = dev_get_drvdata(dev);
	int result;

	result = wait_event_timeout(data->ipc_wait_queue,
			data->vts_ready, msecs_to_jiffies(3000));
	if (data->vts_ready) {
		result = 0;
	} else {
		dev_err(dev, "VTS Firmware is not ready\n");
		result = -ETIME;
	}

	return result;
}

static void vts_pad_retention(bool retention)
{
	if (!retention) {
		exynos_pmu_update(PAD_RETENTION_VTS_OPTION, 0x10000000, 0x10000000);
	}
}

static void vts_cfg_gpio(struct device *dev, const char *name)
{
	struct vts_data *data = dev_get_drvdata(dev);
	struct pinctrl_state *pin_state;
	int ret;

	dev_info(dev, "%s(%s)\n", __func__, name);

	pin_state = pinctrl_lookup_state(data->pinctrl, name);
	if (IS_ERR(pin_state)) {
		dev_err(dev, "Couldn't find pinctrl %s\n", name);
	} else {
		ret = pinctrl_select_state(data->pinctrl, pin_state);
		if (ret < 0)
			dev_err(dev, "Unable to configure pinctrl %s\n", name);
	}
}

static u32 vts_set_baaw(void __iomem *sfr_base, u64 base, u32 size)
{
	u32 aligned_size = round_up(size, SZ_4M);
	u64 aligned_base = round_down(base, aligned_size);

	pr_info("%s: %08llx, %08x, %08llx, %08x", __func__, base, size, aligned_base, aligned_size);

	writel(aligned_size / SZ_4K, sfr_base + VTS_VTS_MEM_CONFIG0);
	writel(aligned_base / SZ_4K, sfr_base + VTS_VTS_MEM_CONFIG1);
	writel(aligned_base / SZ_4K, sfr_base + VTS_VTS_MEM_CONFIG2);
	writel(aligned_size / SZ_4K, sfr_base + VTS_VTS_MEM_CONFIG3);

	return base - aligned_base + VTS_BAAW_BASE;
}

static int vts_clk_set_rate(struct device *dev, unsigned long combination)
{
	struct vts_data *data = dev_get_drvdata(dev);
	unsigned long dmic_rate, dmic_sync, dmic_if;
	int result;

	dev_info(dev, "%s(%lu)\n", __func__, combination);

	switch (combination) {
	case 2:
		dmic_rate = 384000;
		dmic_sync = 384000;
		dmic_if = 768000;
		break;
	case 0:
		dmic_rate = 512000;
		dmic_sync = 512000;
		dmic_if = 1024000;
		break;
	case 1:
		dmic_rate = 768000;
		dmic_sync = 768000;
		dmic_if = 1536000;
		break;
	case 3:
		dmic_rate = 4096000;
		dmic_sync = 2048000;
		dmic_if = 4096000;
		break;
	default:
		result = -EINVAL;
		goto out;
	}


	result = clk_set_rate(data->clk_dmic_if, dmic_if);
	if (IS_ERR_VALUE(result)) {
		dev_err(dev, "Failed to set rate of the clock %s\n", "dmic_if");
		goto out;
	}
	dev_info(dev, "DMIC IF clock rate: %lu\n", clk_get_rate(data->clk_dmic_if));

	result = clk_set_rate(data->clk_dmic_sync, dmic_sync);
	if (IS_ERR_VALUE(result)) {
		dev_err(dev, "Failed to set rate of the clock %s\n", "dmic_sync");
		goto out;
	}
	dev_info(dev, "DMIC SYNC clock rate: %lu\n", clk_get_rate(data->clk_dmic_sync));

	result = clk_set_rate(data->clk_dmic, dmic_rate);
	if (IS_ERR_VALUE(result)) {
		dev_err(dev, "Failed to set rate of the clock %s\n", "dmic");
		goto out;
	}
	dev_info(dev, "DMIC clock rate: %lu\n", clk_get_rate(data->clk_dmic));

out:
	return result;
}

int vts_acquire_sram(struct platform_device *pdev, int vts)
{
	struct vts_data *data = platform_get_drvdata(pdev);
	int previous;

	dev_info(&pdev->dev, "%s(%d)\n", __func__, vts);

	if (!vts) {
		while(pm_runtime_active(&pdev->dev)) {
			dev_warn(&pdev->dev, "%s Clear existing active states\n", __func__);
			pm_runtime_put_sync(&pdev->dev);
		}
	}
	previous = test_and_set_bit(0, &data->sram_acquired);
	if (previous) {
		dev_err(&pdev->dev, "vts sram acquisition failed\n");
		return -EBUSY;
	}

	if (!vts) {
		pm_runtime_get_sync(&pdev->dev);
		data->voicecall_enabled = true;
		data->vts_state = VTS_STATE_VOICECALL;
	}

	writel((vts ? 0 : 1) << VTS_MEM_SEL_OFFSET, data->sfr_base + VTS_SHARED_MEM_CTRL);

	return 0;
}
EXPORT_SYMBOL(vts_acquire_sram);

int vts_release_sram(struct platform_device *pdev, int vts)
{
	struct vts_data *data = platform_get_drvdata(pdev);

	dev_info(&pdev->dev, "%s(%d)\n", __func__, vts);

	if (test_bit(0, &data->sram_acquired) &&
		(data->voicecall_enabled || vts)) {
		writel(0 << VTS_MEM_SEL_OFFSET,
			data->sfr_base + VTS_SHARED_MEM_CTRL);
		clear_bit(0, &data->sram_acquired);

		if (!vts) {
			pm_runtime_put_sync(&pdev->dev);
			data->voicecall_enabled = false;
		}
		dev_info(&pdev->dev, "%s(%d) completed\n",
				__func__, vts);
	} else
		dev_warn(&pdev->dev, "%s(%d) already released\n",
				__func__, vts);

	return 0;
}
EXPORT_SYMBOL(vts_release_sram);

int vts_clear_sram(struct platform_device *pdev)
{
	struct vts_data *data = platform_get_drvdata(pdev);

	pr_info("%s\n", __func__);

	memset(data->sram_base, 0, data->sram_size);

	return 0;
}
EXPORT_SYMBOL(vts_clear_sram);

volatile bool vts_is_on(void)
{
	return p_vts_data && p_vts_data->enabled;
}
EXPORT_SYMBOL(vts_is_on);

static struct snd_soc_dai_driver vts_dai[] = {
	{
		.name = "vts-tri",
		.capture = {
			.stream_name = "VTS Trigger Capture",
			.channels_min = 1,
			.channels_max = 2,
			.rates = SNDRV_PCM_RATE_16000,
			.formats = SNDRV_PCM_FMTBIT_S16,
			.sig_bits = 16,
		 },
	},
	{
		.name = "vts-rec",
		.capture = {
			.stream_name = "VTS Capture",
			.channels_min = 1,
			.channels_max = 2,
			.rates = SNDRV_PCM_RATE_16000,
			.formats = SNDRV_PCM_FMTBIT_S16,
			.sig_bits = 16,
		 },
	},
};

static const char *vts_hpf_sel_texts[] = {"120Hz", "40Hz"};
static SOC_ENUM_SINGLE_DECL(vts_hpf_sel, VTS_DMIC_CONTROL_DMIC_IF, VTS_DMIC_HPF_SEL_OFFSET, vts_hpf_sel_texts);

static const char *vts_cps_sel_texts[] = {"normal", "absolute"};
static SOC_ENUM_SINGLE_DECL(vts_cps_sel, VTS_DMIC_CONTROL_DMIC_IF, VTS_DMIC_CPS_SEL_OFFSET, vts_cps_sel_texts);

static const DECLARE_TLV_DB_SCALE(vts_gain_tlv_array, 0, 6, 0);

static const char *vts_sys_sel_texts[] = {"512kHz", "768kHz", "384kHz", "2048kHz"};
static SOC_ENUM_SINGLE_DECL(vts_sys_sel, VTS_DMIC_CONTROL_DMIC_IF, VTS_DMIC_SYS_SEL_OFFSET, vts_sys_sel_texts);

static int vts_sys_sel_put_enum(struct snd_kcontrol *kcontrol, struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_codec *codec = snd_soc_kcontrol_codec(kcontrol);
	struct device *dev = codec->dev;
	unsigned int *item = ucontrol->value.enumerated.item;
	struct vts_data *data = p_vts_data;

	dev_dbg(dev, "%s(%u)\n", __func__, item[0]);

	data->syssel_rate = item[0];

	return snd_soc_put_enum_double(kcontrol, ucontrol);
}

static const char *vts_polarity_clk_texts[] = {"rising edge of clock", "falling edge of clock"};
static SOC_ENUM_SINGLE_DECL(vts_polarity_clk, VTS_DMIC_CONTROL_DMIC_IF, VTS_DMIC_POLARITY_CLK_OFFSET, vts_polarity_clk_texts);

static const char *vts_polarity_output_texts[] = {"right first", "left first"};
static SOC_ENUM_SINGLE_DECL(vts_polarity_output, VTS_DMIC_CONTROL_DMIC_IF, VTS_DMIC_POLARITY_OUTPUT_OFFSET, vts_polarity_output_texts);

static const char *vts_polarity_input_texts[] = {"left PDM on CLK high", "left PDM on CLK low"};
static SOC_ENUM_SINGLE_DECL(vts_polarity_input, VTS_DMIC_CONTROL_DMIC_IF, VTS_DMIC_POLARITY_INPUT_OFFSET, vts_polarity_input_texts);

static const char *vts_ovfw_ctrl_texts[] = {"limit", "reset"};
static SOC_ENUM_SINGLE_DECL(vts_ovfw_ctrl, VTS_DMIC_CONTROL_DMIC_IF, VTS_DMIC_OVFW_CTRL_OFFSET, vts_ovfw_ctrl_texts);

static const char *vts_cic_sel_texts[] = {"Off", "On"};
static SOC_ENUM_SINGLE_DECL(vts_cic_sel, VTS_DMIC_CONTROL_DMIC_IF, VTS_DMIC_CIC_SEL_OFFSET, vts_cic_sel_texts);

static const char * const vtsexec_mode_text[] = {
	"OFF", "VOICE_TRIGGER_MODE", "SOUND_DETECT_MODE", "VT_ALWAYS_ON_MODE"
};
/* Keyphrases svoice: "Hi Galaxy", Sensory: "Hi Blue Genie" Google:"Okay Google" */
static const char * const vtsactive_phrase_text[] = {
	"SVOICE", "SENSORY", "GOOGLE"
};
static const char * const voicerecog_start_text[] = {
	"Off", "On"
};

static SOC_ENUM_SINGLE_EXT_DECL(vtsexec_mode_enum, vtsexec_mode_text);
static SOC_ENUM_SINGLE_EXT_DECL(vtsactive_phrase_enum, vtsactive_phrase_text);
static SOC_ENUM_SINGLE_EXT_DECL(voicerecog_start_enum, voicerecog_start_text);

static int get_vtsexec_mode(struct snd_kcontrol *kcontrol,
	struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_codec *codec = snd_soc_kcontrol_codec(kcontrol);
	struct vts_data *data = p_vts_data;

	ucontrol->value.integer.value[0] = data->exec_mode;

	dev_dbg(codec->dev, "GET VTS Execution mode: %s \n",
			vtsexec_mode_text[data->exec_mode]);

	return 0;
}

static int set_vtsexec_mode(struct snd_kcontrol *kcontrol,
	struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_codec *codec = snd_soc_kcontrol_codec(kcontrol);
	struct vts_data *data = p_vts_data;
	u32 values[3];
	int result = 0;
	int vtsexecution_mode;

	u32 keyword_type = 1;
	char env[100] = {0,};
	char *envp[2] = {env, NULL};
	struct device *dev = &data->pdev->dev;
	int loopcnt = 10;

	pm_runtime_barrier(codec->dev);

	while (data->voicecall_enabled) {
		dev_warn(codec->dev, "%s voicecall (%d)\n", __func__, data->voicecall_enabled);

		if (loopcnt <= 0) {
			dev_warn(codec->dev, "%s VTS SRAM is Used for CP call\n", __func__);

			keyword_type = -EBUSY;
			snprintf(env, sizeof(env),
				 "VOICE_WAKEUP_WORD_ID=%x",
				 keyword_type);

			kobject_uevent_env(&dev->kobj, KOBJ_CHANGE, envp);
			return -EBUSY;
		}

		loopcnt--;
		usleep_range(10000, 10000);
	}

	dev_warn(codec->dev, "%s voicecall (%d) (End)\n", __func__, data->voicecall_enabled);

	vtsexecution_mode = ucontrol->value.integer.value[0];

	if (vtsexecution_mode > 3) {
		dev_err(codec->dev,
		"Invalid voice control mode =%d", vtsexecution_mode);
		return 0;
	} else {
		dev_info(codec->dev, "%s VTS Execution mode: %d %s \n", __func__,
			data->exec_mode, vtsexec_mode_text[vtsexecution_mode]);
		if (data->exec_mode == VTS_OFF_MODE && vtsexecution_mode != VTS_OFF_MODE) {
			pm_runtime_get_sync(codec->dev);
			vts_clk_set_rate(codec->dev, data->syssel_rate);
		}

		if (pm_runtime_active(codec->dev)) {
			values[0] = vtsexecution_mode;
			values[1] = 0;
			values[2] = 0;
			result = vts_start_ipc_transaction(codec->dev, data, VTS_IRQ_AP_SET_MODE, &values, 0, 1);
			if (IS_ERR_VALUE(result)) {
				dev_err(codec->dev, "%s SET_MODE IPC transaction Failed\n",
						vtsexec_mode_text[vtsexecution_mode]);
				if (data->exec_mode == VTS_OFF_MODE &&
					 vtsexecution_mode != VTS_OFF_MODE)
					pm_runtime_put_sync(codec->dev);
				return result;
			}
		}
		data->exec_mode = vtsexecution_mode;
		dev_info(codec->dev, "%s VTS Execution mode: %s \n", __func__,
			vtsexec_mode_text[vtsexecution_mode]);

		if (vtsexecution_mode == VTS_OFF_MODE && pm_runtime_active(codec->dev)) {
			pm_runtime_put_sync(codec->dev);
		}
	}

	return  0;
}

static int get_vtsactive_phrase(struct snd_kcontrol *kcontrol,
	struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_codec *codec = snd_soc_kcontrol_codec(kcontrol);
	struct vts_data *data = p_vts_data;

	ucontrol->value.integer.value[0] = data->active_trigger;

	dev_dbg(codec->dev, "GET VTS Active Phrase: %s \n",
			vtsactive_phrase_text[data->active_trigger]);

	return 0;
}

static int set_vtsactive_phrase(struct snd_kcontrol *kcontrol,
	struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_codec *codec = snd_soc_kcontrol_codec(kcontrol);
	struct vts_data *data = p_vts_data;
	int vtsactive_phrase;

	vtsactive_phrase = ucontrol->value.integer.value[0];

	if (vtsactive_phrase < 0 || vtsactive_phrase > 2) {
		dev_err(codec->dev,
		"Invalid VTS Trigger Key phrase =%d", vtsactive_phrase);
		return 0;
	} else {
		data->active_trigger = vtsactive_phrase;
		dev_info(codec->dev, "VTS Active phrase: %s \n",
			vtsactive_phrase_text[vtsactive_phrase]);
	}

	return  0;
}

static int get_voicetrigger_value(struct snd_kcontrol *kcontrol,
	struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_codec *codec = snd_soc_kcontrol_codec(kcontrol);
	struct vts_data *data = p_vts_data;

	ucontrol->value.integer.value[0] = data->target_size;

	dev_info(codec->dev, "GET Voice Trigger Value: %d \n",
			data->target_size);

	return 0;
}

static int set_voicetrigger_value(struct snd_kcontrol *kcontrol,
	struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_codec *codec = snd_soc_kcontrol_codec(kcontrol);
	struct vts_data *data = p_vts_data;
	u32 values[3];
	int result = 0;
	int trig_ms;

	pm_runtime_barrier(codec->dev);

	if (data->voicecall_enabled) {
		u32 keyword_type = 1;
		char env[100] = {0,};
		char *envp[2] = {env, NULL};
		struct device *dev = &data->pdev->dev;

		dev_warn(codec->dev, "%s VTS SRAM is Used for CP call\n", __func__);
		keyword_type = -EBUSY;
		snprintf(env, sizeof(env),
			 "VOICE_WAKEUP_WORD_ID=%x",
			 keyword_type);

		kobject_uevent_env(&dev->kobj, KOBJ_CHANGE, envp);
		return -EBUSY;
	}

	trig_ms = ucontrol->value.integer.value[0];

	if (trig_ms > 2000 || trig_ms < 0) {
		dev_err(codec->dev,
		"Invalid Voice Trigger Value = %d (valid range 0~2000ms)", trig_ms);
		return 0;
	} else {
		/* Configure VTS target size */
		values[0] = trig_ms * 32; /* 1ms requires (16KHz,16bit,Mono) = 16samples * 2 bytes = 32 bytes*/
		values[1] = 0;
		values[2] = 0;
		result = vts_start_ipc_transaction(codec->dev, data, VTS_IRQ_AP_TARGET_SIZE, &values, 0, 1);
		if (IS_ERR_VALUE(result)) {
			dev_err(codec->dev, "Voice Trigger Value setting IPC Transaction Failed: %d\n", result);
			return result;
		}

		data->target_size = trig_ms;
		dev_info(codec->dev, "SET Voice Trigger Value: %dms\n",
			data->target_size);
	}

	return  0;
}

static int get_voicerecognize_start(struct snd_kcontrol *kcontrol,
	struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_codec *codec = snd_soc_kcontrol_codec(kcontrol);
	struct vts_data *data = p_vts_data;

	ucontrol->value.integer.value[0] = data->voicerecog_start;

	dev_dbg(codec->dev, "GET Voice Recognization start : %s \n",
			voicerecog_start_text[data->voicerecog_start]);

	return 0;
}

static int vts_start_recognization(struct device *dev, int start)
{
	struct vts_data *data = dev_get_drvdata(dev);
	int active_trigger = data->active_trigger;
	int result;
	u32 values[3];
	const struct firmware *firmware1;
	const struct firmware *firmware2;

	dev_info(dev, "%s\n", __func__);

	if ((data->exec_mode == VTS_VOICE_TRIGGER_MODE ||
		data->exec_mode == VTS_SOUND_DETECT_MODE ||
		data->exec_mode == VTS_VT_ALWAYS_ON_MODE ||
		(data->exec_mode == VTS_OFF_MODE && !start)) &&
		(TRIGGER_NONE < active_trigger && active_trigger < TRIGGER_COUNT)) {
		start = !!start;
		if (start) {
			if (data->exec_mode != VTS_SOUND_DETECT_MODE) {
				/* load voice_net.bin @ offset 0x2A800 &
				voice_grammar.bin @offset 0x32800
				file before starting recognition */
				result = request_firmware(&firmware1, "voice_net.bin", dev);
				if (result != 0) {
					dev_warn(dev, "Failed to request '%s'\n", "voice_net.bin");
					return result;
				}

				memcpy(data->sram_base + 0x2A800, firmware1->data, firmware1->size);
				dev_info(dev, "voice_net.bin Ref Binary uploaded to Firmware size=%zu\n",
						firmware1->size);

				result = request_firmware(&firmware2, "voice_grammar.bin", dev);
				if (result != 0) {
					dev_warn(dev, "Failed to request '%s'\n", "voice_grammar.bin");
					release_firmware(firmware1);
					return result;
				}

				memcpy(data->sram_base + 0x32800, firmware2->data, firmware2->size);
				dev_info(dev, "voice_grammar.bin RefBinary uploaded to Firmware size=%zu\n",
					firmware2->size);

				/* release loaded fimrware binaries */
				release_firmware(firmware1);
				release_firmware(firmware2);
			}

			result = vts_set_dmicctrl(data->pdev,
					VTS_MICCONF_FOR_TRIGGER, true);
			if (IS_ERR_VALUE(result)) {
				dev_err(dev, "%s: MIC control failed\n",
							 __func__);
				return result;
			}

			if (data->exec_mode  == VTS_SOUND_DETECT_MODE) {
				int ctrl_dmicif;
				/* set VTS Gain for LPSD mode */
				ctrl_dmicif = readl(data->dmic_base + VTS_DMIC_CONTROL_DMIC_IF);
				ctrl_dmicif &= ~(0x7 << VTS_DMIC_GAIN_OFFSET);
				writel((ctrl_dmicif |
					(data->lpsdgain << VTS_DMIC_GAIN_OFFSET)),
					data->dmic_base + VTS_DMIC_CONTROL_DMIC_IF);
			}

			/* Send Start recognition IPC command to VTS */
			values[0] = 1 << active_trigger;
			values[1] = 0;
			values[2] = 0;
			result = vts_start_ipc_transaction(dev, data,
					VTS_IRQ_AP_START_RECOGNITION,
					&values, 0, 1);
			if (IS_ERR_VALUE(result)) {
				dev_err(dev, "vts ipc VTS_IRQ_AP_START_RECOGNITION failed: %d\n", result);
				return result;
			}

			data->vts_state = VTS_STATE_RECOG_STARTED;
			dev_info(dev, "%s start=%d, active_trigger=%d\n", __func__, start, active_trigger);

		}else if (!start){
			values[0] = 1 << active_trigger;
			values[1] = 0;
			values[2] = 0;
			result = vts_start_ipc_transaction(dev, data,
					VTS_IRQ_AP_STOP_RECOGNITION,
					&values, 0, 1);
			if (IS_ERR_VALUE(result)) {
				dev_err(dev, "vts ipc VTS_IRQ_AP_STOP_RECOGNITION failed: %d\n", result);
				return result;
			}
			dev_info(dev, "%s start=%d, active_trigger=%d\n", __func__, start, active_trigger);

			result = vts_set_dmicctrl(data->pdev,
					VTS_MICCONF_FOR_TRIGGER, false);
			if (IS_ERR_VALUE(result)) {
				dev_err(dev, "%s: MIC control failed\n",
							 __func__);
				return result;
			}
			data->vts_state = VTS_STATE_RECOG_STOPPED;
		}

	} else {
		if (data->exec_mode == VTS_VOICE_TRIGGER_MODE ||
			 data->exec_mode == VTS_SOUND_DETECT_MODE ||
			 data->exec_mode == VTS_VT_ALWAYS_ON_MODE)
			dev_warn(dev, "active_trigger is not valid: %d\n", active_trigger);
		else
			dev_warn(dev, "VTS Mode Should be Configured TRIGGER Mode\n");
		return -EINVAL;
	}

	return 0;
}

static int set_voicerecognize_start(struct snd_kcontrol *kcontrol,
	struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_codec *codec = snd_soc_kcontrol_codec(kcontrol);
	struct vts_data *data = p_vts_data;
	int result = 0;
	int recognize_start;

	pm_runtime_barrier(codec->dev);

	if (data->voicecall_enabled) {
		u32 keyword_type = 1;
		char env[100] = {0,};
		char *envp[2] = {env, NULL};
		struct device *dev = &data->pdev->dev;

		dev_warn(codec->dev, "%s VTS SRAM is Used for CP call\n", __func__);
		keyword_type = -EBUSY;
		snprintf(env, sizeof(env),
			 "VOICE_WAKEUP_WORD_ID=%x",
			 keyword_type);

		kobject_uevent_env(&dev->kobj, KOBJ_CHANGE, envp);
		return -EBUSY;
	}

	recognize_start = ucontrol->value.integer.value[0];

	if (recognize_start > 1) {
		dev_err(codec->dev,
		"Invalid Voice Recognize input =%d", recognize_start);
		return 0;
	} else {
		if ((!data->voicerecog_start && recognize_start) ||
			(data->voicerecog_start && !recognize_start)) {

			if (recognize_start) {
				pm_runtime_get_sync(codec->dev);
			}

			result = vts_start_recognization(codec->dev, recognize_start);
			if (IS_ERR_VALUE(result)) {
				dev_err(codec->dev, "Voice Start Recognization Failed: %d\n", result);
				if (recognize_start)
					pm_runtime_put_sync(codec->dev);
				return result;
			}

			data->voicerecog_start = recognize_start;

			dev_info(codec->dev, "SET Voice Recognization start : %s \n",
				voicerecog_start_text[data->voicerecog_start]);

			if (!recognize_start) {
				pm_runtime_put_sync(codec->dev);
			}
		}
	}

	return  0;
}

static const struct snd_kcontrol_new vts_controls[] = {
	SOC_SINGLE("PERIOD DATA2REQ", VTS_DMIC_ENABLE_DMIC_IF, VTS_DMIC_PERIOD_DATA2REQ_OFFSET, 3, 0),
	SOC_SINGLE("HPF EN", VTS_DMIC_CONTROL_DMIC_IF, VTS_DMIC_HPF_EN_OFFSET, 1, 0),
	SOC_ENUM("HPF SEL", vts_hpf_sel),
	SOC_ENUM("CPS SEL", vts_cps_sel),
	SOC_SINGLE_TLV("GAIN", VTS_DMIC_CONTROL_DMIC_IF, VTS_DMIC_GAIN_OFFSET, 4, 0, vts_gain_tlv_array),
	SOC_ENUM_EXT("SYS SEL", vts_sys_sel, snd_soc_get_enum_double, vts_sys_sel_put_enum),
	SOC_ENUM("POLARITY CLK", vts_polarity_clk),
	SOC_ENUM("POLARITY OUTPUT", vts_polarity_output),
	SOC_ENUM("POLARITY INPUT", vts_polarity_input),
	SOC_ENUM("OVFW CTRL", vts_ovfw_ctrl),
	SOC_ENUM("CIC SEL", vts_cic_sel),
	SOC_ENUM_EXT("Execution Mode", vtsexec_mode_enum,
		get_vtsexec_mode, set_vtsexec_mode),
	SOC_ENUM_EXT("Active Keyphrase", vtsactive_phrase_enum,
		get_vtsactive_phrase, set_vtsactive_phrase),
	SOC_SINGLE_EXT("VoiceTrigger Value",
		SND_SOC_NOPM,
		0, 2000, 0,
		get_voicetrigger_value, set_voicetrigger_value),
	SOC_ENUM_EXT("VoiceRecognize Start", voicerecog_start_enum,
		get_voicerecognize_start, set_voicerecognize_start),
};

static const char *dmic_sel_texts[] = {"DPDM", "APDM"};
static SOC_ENUM_SINGLE_DECL(dmic_sel_enum, VTS_DMIC_CONTROL_DMIC_IF, VTS_DMIC_DMIC_SEL_OFFSET, dmic_sel_texts);
static const struct snd_kcontrol_new dmic_sel_controls[] = {
	SOC_DAPM_ENUM("MUX", dmic_sel_enum),
};

static const struct snd_kcontrol_new dmic_if_controls[] = {
	SOC_DAPM_SINGLE("RCH EN", VTS_DMIC_CONTROL_DMIC_IF, VTS_DMIC_RCH_EN_OFFSET, 1, 0),
	SOC_DAPM_SINGLE("LCH EN", VTS_DMIC_CONTROL_DMIC_IF, VTS_DMIC_LCH_EN_OFFSET, 1, 0),
};

static const struct snd_soc_dapm_widget vts_dapm_widgets[] = {
	SND_SOC_DAPM_INPUT("PAD APDM"),
	SND_SOC_DAPM_INPUT("PAD DPDM"),
	SND_SOC_DAPM_MUX("DMIC SEL", SND_SOC_NOPM, 0, 0, dmic_sel_controls),
	SOC_MIXER_ARRAY("DMIC IF", SND_SOC_NOPM, 0, 0, dmic_if_controls),
};

static const struct snd_soc_dapm_route vts_dapm_routes[] = {
	// sink, control, source
	{"DMIC SEL", "APDM", "PAD APDM"},
	{"DMIC SEL", "DPDM", "PAD DPDM"},
	{"DMIC IF", "RCH EN", "DMIC SEL"},
	{"DMIC IF", "LCH EN", "DMIC SEL"},
	{"VTS Capture", NULL, "DMIC IF"},
};


static struct regmap * vts_codec_get_regmap(struct device *dev)
{
	struct vts_data *data = dev_get_drvdata(dev);
	return data->regmap_dmic;
}

static int vts_codec_probe(struct snd_soc_codec *codec)
{
	vts_clk_set_rate(codec->dev, 0);

	return 0;
}

static const struct snd_soc_codec_driver vts_codec = {
	.probe = vts_codec_probe,
	.ignore_pmdown_time = true,
	.idle_bias_off = true,
	.suspend_bias_off = true,
	.controls = vts_controls,
	.num_controls = ARRAY_SIZE(vts_controls),
	.dapm_widgets = vts_dapm_widgets,
	.num_dapm_widgets = ARRAY_SIZE(vts_dapm_widgets),
	.dapm_routes = vts_dapm_routes,
	.num_dapm_routes = ARRAY_SIZE(vts_dapm_routes),
	.get_regmap = vts_codec_get_regmap,
};

int vts_set_dmicctrl(struct platform_device *pdev, int micconf_type, bool enable)
{
	struct device *dev = &pdev->dev;
	struct vts_data *data = platform_get_drvdata(pdev);
	int dmic_clkctrl = 0;
	int ctrl_dmicif = 0;
	int select_dmicclk = 0;

	dev_dbg(dev, "%s-- flag: %d mictype: %d micusagecnt: %d\n",
		 __func__, enable, micconf_type, data->micclk_init_cnt);
	if (!data->vts_ready) {
		dev_warn(dev, "%s: VTS Firmware Not running\n", __func__);
		return -EINVAL;
	}

	if (enable) {
		if (!data->micclk_init_cnt) {
			ctrl_dmicif = readl(data->dmic_base + VTS_DMIC_CONTROL_DMIC_IF);

			if (ctrl_dmicif & (0x1 << VTS_DMIC_DMIC_SEL_OFFSET)) {
				vts_cfg_gpio(dev, "amic_default");
				select_dmicclk = ((0x1 << VTS_ENABLE_CLK_GEN_OFFSET) |
					(0x1 << VTS_SEL_EXT_DMIC_CLK_OFFSET) |
					(0x1 << VTS_ENABLE_CLK_CLK_GEN_OFFSET));
				writel(select_dmicclk, data->sfr_base + VTS_USER_REG2);
				/* Set AMIC VTS Gain */
				writel((ctrl_dmicif |
					 (data->amicgain << VTS_DMIC_GAIN_OFFSET)),
					 data->dmic_base + VTS_DMIC_CONTROL_DMIC_IF);

			} else {
				vts_cfg_gpio(dev, "dmic_default");
				select_dmicclk = ((0x0 << VTS_ENABLE_CLK_GEN_OFFSET) |
					(0x0 << VTS_SEL_EXT_DMIC_CLK_OFFSET) |
					(0x0 << VTS_ENABLE_CLK_CLK_GEN_OFFSET));
				writel(select_dmicclk, data->sfr_base + VTS_USER_REG2);
				/* Set DMIC VTS Gain */
				writel((ctrl_dmicif |
					 (data->dmicgain << VTS_DMIC_GAIN_OFFSET)),
					 data->dmic_base + VTS_DMIC_CONTROL_DMIC_IF);
			}

			dmic_clkctrl = readl(data->sfr_base + VTS_DMIC_CLK_CTRL);
			writel(dmic_clkctrl | (0x1 << VTS_CLK_ENABLE_OFFSET),
				data->sfr_base + VTS_DMIC_CLK_CTRL);
			dev_info(dev, "%s Micclk setting ENABLED\n", __func__);
		}

		/* check whether Mic is already configure or not based on VTS
		   option type for MIC configuration book keeping */
		if (!(data->mic_ready & (0x1 << VTS_MICCONF_FOR_TRIGGER)) &&
			micconf_type == VTS_MICCONF_FOR_TRIGGER) {
			data->micclk_init_cnt++;
			data->mic_ready |= (0x1 << micconf_type);
			dev_info(dev, "%s Micclk ENABLED for TRIGGER ++ %d\n",
				 __func__, data->mic_ready);
		} else if (!(data->mic_ready & (0x1 << VTS_MICCONF_FOR_RECORD)) &&
			micconf_type == VTS_MICCONF_FOR_RECORD) {
			data->micclk_init_cnt++;
			data->mic_ready |= (0x1 << micconf_type);
			dev_info(dev, "%s Micclk ENABLED for RECORD ++ %d\n",
				 __func__, data->mic_ready);
		}
	} else {
		if (data->micclk_init_cnt)
			data->micclk_init_cnt--;
		if (!data->micclk_init_cnt) {
			vts_cfg_gpio(dev, "idle");

			dmic_clkctrl = readl(data->sfr_base + VTS_DMIC_CLK_CTRL);
			writel(dmic_clkctrl & ~(0x1 << VTS_CLK_ENABLE_OFFSET),
				data->sfr_base + VTS_DMIC_CLK_CTRL);
			writel(0x0, data->sfr_base + VTS_USER_REG2);
			/* reset VTS Gain to default */
			writel((ctrl_dmicif & (~(0x7 << VTS_DMIC_GAIN_OFFSET))),
				 data->dmic_base + VTS_DMIC_CONTROL_DMIC_IF);
			dev_info(dev, "%s Micclk setting DISABLED\n", __func__);
		}

		/* MIC configuration book keeping */
		if ((data->mic_ready & (0x1 << VTS_MICCONF_FOR_TRIGGER)) &&
			micconf_type == VTS_MICCONF_FOR_TRIGGER) {
			data->mic_ready &= ~(0x1 << micconf_type);
			dev_info(dev, "%s Micclk DISABLED for TRIGGER -- %d\n",
				 __func__, data->mic_ready);
		} else if ((data->mic_ready & (0x1 << VTS_MICCONF_FOR_RECORD)) &&
			micconf_type == VTS_MICCONF_FOR_RECORD) {
			data->mic_ready &= ~(0x1 << micconf_type);
			dev_info(dev, "%s Micclk DISABLED for RECORD -- %d\n",
				 __func__, data->mic_ready);
		}
	}

	return 0;
}

static irqreturn_t vts_error_handler(int irq, void *dev_id)
{
	struct platform_device *pdev = dev_id;
	struct device *dev = &pdev->dev;
	struct vts_data *data = platform_get_drvdata(pdev);
	u32 error_code;

	mailbox_read_shared_register(data->pdev_mailbox, &error_code, 3, 1);
	vts_ipc_ack(data, 1);

	dev_err(dev, "Error occurred on VTS: 0x%x\n", (int)error_code);
	vts_reset_cpu();

	return IRQ_HANDLED;
}

static irqreturn_t vts_boot_completed_handler(int irq, void *dev_id)
{
	struct platform_device *pdev = dev_id;
	struct device *dev = &pdev->dev;
	struct vts_data *data = platform_get_drvdata(pdev);

	data->vts_ready = 1;

	vts_ipc_ack(data, 1);
	wake_up(&data->ipc_wait_queue);

	dev_info(dev, "VTS boot completed\n");

	return IRQ_HANDLED;
}

static irqreturn_t vts_ipc_received_handler(int irq, void *dev_id)
{
	struct platform_device *pdev = dev_id;
	struct device *dev = &pdev->dev;
	struct vts_data *data = platform_get_drvdata(pdev);
	u32 result;

	mailbox_read_shared_register(data->pdev_mailbox, &result, 3, 1);
	dev_dbg(dev, "VTS received IPC: 0x%x\n", result);

	switch (data->ipc_state_ap) {
	case SEND_MSG:
		if (result  == (0x1 << data->running_ipc)) {
			dev_dbg(dev, "IPC transaction completed\n");
			data->ipc_state_ap = SEND_MSG_OK;
		} else {
			dev_err(dev, "IPC transaction error\n");
			data->ipc_state_ap = SEND_MSG_FAIL;
		}
		break;
	default:
		dev_warn(dev, "State fault: %d Ack_value:0x%x\n",
				data->ipc_state_ap, result);
		break;
	}

	return IRQ_HANDLED;
}

static irqreturn_t vts_voice_triggered_handler(int irq, void *dev_id)
{
	struct platform_device *pdev = dev_id;
	struct device *dev = &pdev->dev;
	struct vts_data *data = platform_get_drvdata(pdev);
	u32 id, score, frame_count;
	u32 keyword_type = 1;
	char env[100] = {0,};
	char *envp[2] = {env, NULL};

	if (data->mic_ready & (0x1 << PLATFORM_VTS_TRIGGER_RECORD)) {
		mailbox_read_shared_register(data->pdev_mailbox, &id,
						 3, 1);
		vts_ipc_ack(data, 1);

		frame_count = (u32)(id & GENMASK(15, 0));
		score = (u32)((id & GENMASK(27, 16)) >> 16);
		id >>= 28;

		dev_info(dev, "VTS triggered: id = %u, score = %u\n",
				id, score);
		dev_info(dev, "VTS triggered: frame_count = %u\n",
				 frame_count);

		if (data->exec_mode == VTS_VOICE_TRIGGER_MODE ||
			 data->exec_mode == VTS_VT_ALWAYS_ON_MODE) {
			keyword_type = 1;
			snprintf(env, sizeof(env),
					 "VOICE_WAKEUP_WORD_ID=%x",
					 keyword_type);
		} else if (data->exec_mode == VTS_SOUND_DETECT_MODE) {
			snprintf(env, sizeof(env),
				 "VOICE_WAKEUP_WORD_ID=LPSD");
		} else {
			dev_warn(dev, "Unknown VTS Execution Mode!!\n");
		}

		kobject_uevent_env(&dev->kobj, KOBJ_CHANGE, envp);
		wake_lock_timeout(&data->wake_lock,
				VTS_TRIGGERED_TIMEOUT_MS);
		data->vts_state = VTS_STATE_RECOG_TRIGGERED;
	}

	return IRQ_HANDLED;
}

static irqreturn_t vts_trigger_period_elapsed_handler(int irq, void *dev_id)
{
	struct platform_device *pdev = dev_id;
	struct device *dev = &pdev->dev;
	struct vts_data *data = platform_get_drvdata(pdev);
	struct vts_platform_data *platform_data = platform_get_drvdata(data->pdev_vtsdma[0]);
	u32 pointer;

	if (data->mic_ready & (0x1 << PLATFORM_VTS_TRIGGER_RECORD)) {
		mailbox_read_shared_register(data->pdev_mailbox,
					 &pointer, 2, 1);
		dev_dbg(dev, "%s:[%s] Base: %08x pointer:%08x\n",
			 __func__, (platform_data->id ? "VTS-RECORD" :
			 "VTS-TRIGGER"), data->dma_area_vts, pointer);

		if (pointer)
			platform_data->pointer = (pointer -
					 data->dma_area_vts);
		vts_ipc_ack(data, 1);


		snd_pcm_period_elapsed(platform_data->substream);
	}

	return IRQ_HANDLED;
}

static irqreturn_t vts_record_period_elapsed_handler(int irq, void *dev_id)
{
	struct platform_device *pdev = dev_id;
	struct device *dev = &pdev->dev;
	struct vts_data *data = platform_get_drvdata(pdev);
	struct vts_platform_data *platform_data = platform_get_drvdata(data->pdev_vtsdma[1]);
	u32 pointer;

	if (data->mic_ready & (0x1 << PLATFORM_VTS_NORMAL_RECORD)) {
		mailbox_read_shared_register(data->pdev_mailbox,
						 &pointer, 1, 1);
		dev_dbg(dev, "%s:[%s] Base: %08x pointer:%08x\n",
			 __func__,
			 (platform_data->id ? "VTS-RECORD" : "VTS-TRIGGER"),
			 (data->dma_area_vts + BUFFER_BYTES_MAX/2), pointer);

		if (pointer)
			platform_data->pointer = (pointer -
				(data->dma_area_vts + BUFFER_BYTES_MAX/2));
		vts_ipc_ack(data, 1);

		snd_pcm_period_elapsed(platform_data->substream);
	}

	return IRQ_HANDLED;
}

void vts_register_dma(struct platform_device *pdev_vts,
		struct platform_device *pdev_vts_dma, unsigned int id)
{
	struct vts_data *data = platform_get_drvdata(pdev_vts);

	if (id < ARRAY_SIZE(data->pdev_vtsdma)) {
		data->pdev_vtsdma[id] = pdev_vts_dma;
		if (id > data->vtsdma_count) {
			data->vtsdma_count = id + 1;
		}
		dev_info(&data->pdev->dev, "%s: VTS-DMA id(%u)Registered \n", __func__, id);
	} else {
		dev_err(&data->pdev->dev, "%s: invalid id(%u)\n", __func__, id);
	}
}

static int vts_suspend(struct device *dev)
{
	struct vts_data *data = dev_get_drvdata(dev);
	u32 values[3] = {0,0,0};
	int result = 0;

	if (data->vts_ready) {
		if (data->running &&
			data->vts_state == VTS_STATE_RECOG_TRIGGERED) {
			result = vts_start_ipc_transaction(dev, data,
					VTS_IRQ_AP_RESTART_RECOGNITION,
					&values, 0, 1);
			if (IS_ERR_VALUE(result)) {
				dev_err(dev, "%s restarted trigger failed\n",
					__func__);
				goto error_ipc;
			}
			data->vts_state = VTS_STATE_RECOG_STARTED;
		}

		/* enable vts wakeup source interrupts */
		enable_irq_wake(data->irq[VTS_IRQ_VTS_VOICE_TRIGGERED]);
		enable_irq_wake(data->irq[VTS_IRQ_VTS_ERROR]);
		dev_info(dev, "%s: Enable VTS Wakeup source irqs\n", __func__);
	}

error_ipc:
	return result;
}

static int vts_resume(struct device *dev)
{
	struct vts_data *data = dev_get_drvdata(dev);

	if (data->vts_ready) {
		/* disable vts wakeup source interrupts */
		disable_irq_wake(data->irq[VTS_IRQ_VTS_VOICE_TRIGGERED]);
		disable_irq_wake(data->irq[VTS_IRQ_VTS_ERROR]);
		dev_info(dev, "%s: Disable VTS Wakeup source irqs\n", __func__);
	}
	return 0;
}

static void vts_irq_enable(struct platform_device *pdev, bool enable)
{
	struct vts_data *data = platform_get_drvdata(pdev);
	struct device *dev = &pdev->dev;
	int irqidx;

	dev_info(dev, "%s IRQ Enable: [%s]\n", __func__,
			(enable ? "TRUE" : "FALSE"));

	for (irqidx = 0; irqidx < VTS_IRQ_COUNT; irqidx++) {
		if (enable)
			enable_irq(data->irq[irqidx]);
		else
			disable_irq(data->irq[irqidx]);
	}
}

static void vts_save_register(struct vts_data *data)
{
	regcache_cache_only(data->regmap_dmic, true);
	regcache_mark_dirty(data->regmap_dmic);
}

static void vts_restore_register(struct vts_data *data)
{
	regcache_cache_only(data->regmap_dmic, false);
	regcache_sync(data->regmap_dmic);
}

void vts_dbg_print_gpr(struct device *dev, struct vts_data *data)
{
	int i;
	char buf[10];
	unsigned int version = data->vtsfw_version;

	buf[0] = (((version >> 24) & 0xFF) + '0');
	buf[1] = '.';
	buf[2] = (((version >> 16) & 0xFF) + '0');
	buf[3] = '.';
	buf[4] = ((version & 0xFF) + '0');
	buf[5] = '\0';

	dev_info(dev, "========================================\n");
	dev_info(dev, "VTS CM4 register dump (%s)\n", buf);
	dev_info(dev, "----------------------------------------\n");
	for (i = 0; i <= 14; i++) {
		dev_info(dev, "CM4_R%02d        : %08x\n", i,
				readl(data->gpr_base + VTS_CM4_R(i)));
	}
	dev_info(dev, "CM4_PC         : %08x\n",
			readl(data->gpr_base + VTS_CM4_PC));
	dev_info(dev, "========================================\n");
}

void vts_dbg_dump_log_gpr(
	struct device *dev,
	struct vts_data *data,
	unsigned int dbg_type,
	const char *reason)
{
	struct vts_dbg_dump *p_dump = NULL;
	int i;

	dev_dbg(dev, "%s\n", __func__);

	if (!vts_is_on() || !data->running) {
		dev_info(dev, "%s is skipped due to no power\n", __func__);
		return;
	}

	if (dbg_type == RUNTIME_SUSPEND_DUMP)
		p_dump = (struct vts_dbg_dump *)data->dmab_log.area;
	else
		p_dump = (struct vts_dbg_dump *)(data->dmab_log.area +
					(LOG_BUFFER_BYTES_MAX/2));

	/* Get VTS firmware log msgs */
	memcpy_fromio(p_dump->sram, data->sramlog_baseaddr, SZ_2K);

	p_dump->time = sched_clock();
	strncpy(p_dump->reason, reason, sizeof(p_dump->reason) - 1);
	for (i = 0; i <= 14; i++)
		p_dump->gpr[i] = readl(data->gpr_base + VTS_CM4_R(i));

	p_dump->gpr[i++] = readl(data->gpr_base + VTS_CM4_PC);
}

static void exynos_vts_panic_handler(void)
{
	static bool has_run;
	struct vts_data *data = p_vts_data;
	struct device *dev = data ? (data->pdev ? &data->pdev->dev : NULL) : NULL;

	dev_dbg(dev, "%s\n", __func__);

	if (vts_is_on() && dev) {
		if (has_run) {
			dev_info(dev, "already dumped\n");
			return;
		}
		has_run = true;

		/* Dump VTS GPR register & Log messages */
		vts_dbg_dump_log_gpr(dev, data, KERNEL_PANIC_DUMP,
						"panic");
	} else {
		dev_info(dev, "%s: dump is skipped due to no power\n", __func__);
	}
}

static int vts_panic_handler(struct notifier_block *nb,
			       unsigned long action, void *data)
{
	exynos_vts_panic_handler();
	return NOTIFY_OK;
}

static struct notifier_block vts_panic_notifier = {
	.notifier_call	= vts_panic_handler,
	.next		= NULL,
	.priority	= 0	/* priority: INT_MAX >= x >= 0 */
};

static int vts_runtime_suspend(struct device *dev)
{
	struct platform_device *pdev = to_platform_device(dev);
	struct vts_data *data = dev_get_drvdata(dev);
	int i = 1000;
	unsigned int status = 0;
	u32 values[3] = {0,0,0};
	int result = 0;

	dev_info(dev, "%s \n", __func__);

	vts_save_register(data);

	if (data->running) {
		result = vts_start_ipc_transaction(dev, data, VTS_IRQ_AP_POWER_DOWN, &values, 0, 1);
		if (IS_ERR_VALUE(result)) {
			dev_warn(dev, "POWER_DOWN IPC transaction Failed\n");
		}

		/* Dump VTS GPR register & Log messages */
		vts_dbg_print_gpr(dev, data);
		vts_dbg_dump_log_gpr(dev, data, RUNTIME_SUSPEND_DUMP,
						"runtime_suspend");

		/* wait for VTS STANDBYWFI in STATUS SFR */
		do {
			exynos_pmu_read(VTS_CPU_STATUS, &status);
			dev_dbg(dev, "%s Read VTS_CPU_STATUS for STANDBYWFI status\n", __func__);
		} while (i-- && !(status & VTS_CPU_STATUS_STANDBYWFI_MASK));

		if (!i) {
			dev_warn(dev, "VTS IP entering WFI time out\n");
		}

		if (data->irq_state) {
			vts_irq_enable(pdev, false);
			data->irq_state = false;
		}
		vts_release_sram(pdev, 1);
		clk_disable(data->clk_dmic);
		vts_cpu_enable(false);
		vts_cpu_power(false);
		data->running = false;
	}

	data->enabled = false;
	data->exec_mode = VTS_OFF_MODE;
	data->voicerecog_start = 0;
	data->target_size = 0;
	/* reset micbias setting count */
	data->micclk_init_cnt = 0;
	data->mic_ready = 0;
	data->vts_ready = 0;
	data->vts_state = VTS_STATE_NONE;
	dev_info(dev, "%s Exit \n", __func__);
	return 0;
}

static int vts_runtime_resume(struct device *dev)
{
	struct platform_device *pdev = to_platform_device(dev);
	struct vts_data *data = dev_get_drvdata(dev);
	u32 values[3];
	int result;

	dev_info(dev, "%s \n", __func__);

	data->enabled = true;

	vts_restore_register(data);

	vts_cfg_gpio(dev, "dmic_default");
	vts_cfg_gpio(dev, "idle");
	vts_pad_retention(false);

	if (!data->irq_state) {
		vts_irq_enable(pdev, true);
		data->irq_state = true;
	}

	result = vts_acquire_sram(pdev, 1);
	if (IS_ERR_VALUE(result)) {
		dev_warn(dev, "Failed to acquire sram\n");
		goto error_sram;
	}

	result = clk_enable(data->clk_dmic);
	if (IS_ERR_VALUE(result)) {
		dev_err(dev, "Failed to enable the clock\n");
		goto error_clk;
	}
	dev_info(dev, "dmic clock rate:%lu\n", clk_get_rate(data->clk_dmic));

	vts_cpu_power(true);

	result = vts_download_firmware(pdev);
	if (IS_ERR_VALUE(result)) {
		dev_err(dev, "Failed to download firmware\n");
		goto error_firmware;
	}

	vts_cpu_enable(true);

	vts_wait_for_fw_ready(dev);

	/* Configure select sys clock rate */
	vts_clk_set_rate(dev, data->syssel_rate);

	data->dma_area_vts= vts_set_baaw(data->sfr_base,
			data->dmab.addr, BUFFER_BYTES_MAX);
	values[0] = data->dma_area_vts;
	values[1] = 0x140;
	values[2] = 0x800;
	result = vts_start_ipc_transaction(dev, data, VTS_IRQ_AP_SET_DRAM_BUFFER, &values, 0, 1);
	if (IS_ERR_VALUE(result)) {
		dev_err(dev, "DRAM_BUFFER Setting IPC transaction Failed\n");
		goto error_firmware;
	}

	values[0] = VTS_OFF_MODE;
	values[1] = 0;
	values[2] = 0;
	result = vts_start_ipc_transaction(dev, data, VTS_IRQ_AP_SET_MODE, &values, 0, 1);
	if (IS_ERR_VALUE(result)) {
		dev_err(dev, "SET_MODE to OFF IPC transaction Failed\n");
		goto error_firmware;
	}
	data->exec_mode = VTS_OFF_MODE;
	data->active_trigger = 0;

	values[0] = VTS_ENABLE_SRAM_LOG;
	values[1] = 0;
	values[2] = 0;
	result = vts_start_ipc_transaction(dev, data, VTS_IRQ_AP_TEST_COMMAND, &values, 0, 1);
	if (IS_ERR_VALUE(result)) {
		dev_err(dev, "Enable_SRAM_log ipc transaction failed\n");
		goto error_firmware;
	}

	/* Enable CM4 GPR Dump */
	writel(0x1, data->gpr_base);

	dev_dbg(dev, "%s DRAM-setting and VTS-Mode is completed \n", __func__);
	dev_info(dev, "%s Exit \n", __func__);

	data->running = true;
	data->vts_state = VTS_STATE_IDLE;
	return 0;

error_firmware:
	vts_cpu_power(false);
	clk_disable(data->clk_dmic);
error_clk:
	vts_release_sram(pdev, 1);
error_sram:
	if (data->irq_state) {
		vts_irq_enable(pdev, false);
		data->irq_state = false;
	}
	data->running = false;
	return 0;
}

static const struct dev_pm_ops samsung_vts_pm = {
	SET_SYSTEM_SLEEP_PM_OPS(vts_suspend, vts_resume)
	SET_RUNTIME_PM_OPS(vts_runtime_suspend, vts_runtime_resume, NULL)
};

static const struct of_device_id exynos_vts_of_match[] = {
	{
		.compatible = "samsung,vts",
	},
	{},
};
MODULE_DEVICE_TABLE(of, exynos_vts_of_match);

static ssize_t vtsfw_version_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct vts_data *data = dev_get_drvdata(dev);
	unsigned int version = data->vtsfw_version;

	buf[0] = (((version >> 24) & 0xFF) + '0');
	buf[1] = '.';
	buf[2] = (((version >> 16) & 0xFF) + '0');
	buf[3] = '.';
	buf[4] = ((version & 0xFF) + '0');
	buf[5] = '\n';
	buf[6] = '\0';

	return 7;
}

static ssize_t vtsdetectlib_version_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct vts_data *data = dev_get_drvdata(dev);
	unsigned int version = data->vtsdetectlib_version;

	buf[0] = (((version >> 24) & 0xFF) + '0');
	buf[1] = '.';
	buf[2] = (((version >> 16) & 0xFF) + '0');
	buf[3] = '.';
	buf[4] = ((version & 0xFF) + '0');
	buf[5] = '\n';
	buf[6] = '\0';

	return 7;
}
static DEVICE_ATTR_RO(vtsfw_version);
static DEVICE_ATTR_RO(vtsdetectlib_version);

static void vts_complete_firmware_request(const struct firmware *fw, void *context)
{
	struct platform_device *pdev = context;
	struct device *dev = &pdev->dev;
	struct vts_data *data = platform_get_drvdata(pdev);
	unsigned int *pversion = NULL;

	if (!fw) {
		dev_err(dev, "Failed to request firmware\n");
		return;
	}

	data->firmware = fw;
	pversion = (unsigned int*) (fw->data + VTSFW_VERSION_OFFSET);
	data->vtsfw_version = *pversion;
	pversion = (unsigned int*) (fw->data + DETLIB_VERSION_OFFSET);
	data->vtsdetectlib_version = *pversion;

	dev_info(dev, "Firmware loaded at %p (%zu)\n", fw->data, fw->size);
	dev_info(dev, "Firmware version: 0x%x Detection library version: 0x%x\n", data->vtsfw_version, data->vtsdetectlib_version);
}

static void __iomem *samsung_vts_devm_request_and_map(struct platform_device *pdev, const char *name, size_t *size)
{
	struct resource *res;
	void __iomem *result;

	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, name);
	if (IS_ERR_OR_NULL(res)) {
		dev_err(&pdev->dev, "Failed to get %s\n", name);
		return ERR_PTR(-EINVAL);
	}

	if (size) {
		*size = resource_size(res);
	}

	res = devm_request_mem_region(&pdev->dev, res->start, resource_size(res), name);
	if (IS_ERR_OR_NULL(res)) {
		dev_err(&pdev->dev, "Failed to request %s\n", name);
		return ERR_PTR(-EFAULT);
	}

	result = devm_ioremap(&pdev->dev, res->start, resource_size(res));
	if (IS_ERR_OR_NULL(result)) {
		dev_err(&pdev->dev, "Failed to map %s\n", name);
		return ERR_PTR(-EFAULT);
	}

	dev_info(&pdev->dev, "%s: %s(%p) is mapped on %p with size of %zu",
			__func__, name, (void *)res->start, result, (size_t)resource_size(res));

	return result;
}

static int samsung_vts_devm_request_threaded_irq(
		struct platform_device *pdev, const char *irq_name,
		unsigned int hw_irq, irq_handler_t thread_fn)
{
	struct device *dev = &pdev->dev;
	struct vts_data *data = platform_get_drvdata(pdev);
	int result;

	data->irq[hw_irq] = platform_get_irq_byname(pdev, irq_name);
	if (IS_ERR_VALUE(data->irq[hw_irq])) {
		dev_err(dev, "Failed to get irq %s: %d\n", irq_name, data->irq[hw_irq]);
		return data->irq[hw_irq];
	}

	result = devm_request_threaded_irq(dev, data->irq[hw_irq],
			NULL, thread_fn,
			IRQF_TRIGGER_RISING | IRQF_ONESHOT, dev->init_name,
			pdev);

	if (IS_ERR_VALUE(result)) {
		dev_err(dev, "Unable to request irq %s: %d\n", irq_name, result);
	}

	return result;
}

static struct clk *devm_clk_get_and_prepare(struct device *dev, const char *name)
{
	struct clk *clk;
	int result;

	clk = devm_clk_get(dev, name);
	if (IS_ERR(clk)) {
		dev_err(dev, "Failed to get clock %s\n", name);
		goto error;
	}

	result = clk_prepare(clk);
	if (IS_ERR_VALUE(result)) {
		dev_err(dev, "Failed to prepare clock %s\n", name);
		goto error;
	}

error:
	return clk;
}

static const struct reg_default vts_dmic_reg_defaults[] = {
	{0x0000, 0x00030000},
	{0x0004, 0x00000000},
};

static const struct regmap_config vts_codec_regmap_config = {
	.reg_bits = 32,
	.val_bits = 32,
	.reg_stride = 4,
	.max_register = VTS_DMIC_CONTROL_DMIC_IF,
	.reg_defaults = vts_dmic_reg_defaults,
	.num_reg_defaults = ARRAY_SIZE(vts_dmic_reg_defaults),
	.cache_type = REGCACHE_RBTREE,
	.fast_io = true,
};

static int samsung_vts_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct device_node *np = dev->of_node;
	struct vts_data *data;
	int result;
	int dmic_clkctrl = 0;

	dev_info(dev, "%s \n", __func__);
	data = devm_kzalloc(dev, sizeof(struct vts_data), GFP_KERNEL);
	if (!data) {
		dev_err(dev, "Failed to allocate memory\n");
		result = -ENOMEM;
		goto error;
	}

	/* initialize device structure members */
	data->active_trigger = TRIGGER_MCD;

	/* initialize micbias setting count */
	data->micclk_init_cnt = 0;
	data->mic_ready = 0;
	data->vts_state = VTS_STATE_NONE;

	platform_set_drvdata(pdev, data);
	data->pdev = pdev;
	p_vts_data = data;

	init_waitqueue_head(&data->ipc_wait_queue);
	spin_lock_init(&data->ipc_spinlock);
	mutex_init(&data->ipc_mutex);
	wake_lock_init(&data->wake_lock, WAKE_LOCK_SUSPEND, "vts");

	data->pinctrl = devm_pinctrl_get(dev);
	if (IS_ERR(data->pinctrl)) {
		dev_err(dev, "Couldn't get pins (%li)\n",
				PTR_ERR(data->pinctrl));
		return PTR_ERR(data->pinctrl);
	}

	data->sfr_base = samsung_vts_devm_request_and_map(pdev, "sfr", NULL);
	if (IS_ERR(data->sfr_base)) {
		result = PTR_ERR(data->sfr_base);
		goto error;
	}

	data->sram_base = samsung_vts_devm_request_and_map(pdev, "sram", &data->sram_size);
	if (IS_ERR(data->sram_base)) {
		result = PTR_ERR(data->sram_base);
		goto error;
	}

	data->dmic_base = samsung_vts_devm_request_and_map(pdev, "dmic", NULL);
	if (IS_ERR(data->dmic_base)) {
		result = PTR_ERR(data->dmic_base);
		goto error;
	}

	data->gpr_base = samsung_vts_devm_request_and_map(pdev, "gpr", NULL);
	if (IS_ERR(data->gpr_base)) {
		result = PTR_ERR(data->gpr_base);
		goto error;
	}

	data->lpsdgain = 0;
	data->dmicgain = 0;
	data->amicgain = 0;

	/* read tunned VTS gain values */
	of_property_read_u32(np, "lpsd-gain", &data->lpsdgain);
	of_property_read_u32(np, "dmic-gain", &data->dmicgain);
	of_property_read_u32(np, "amic-gain", &data->amicgain);

	dev_info(dev, "VTS Tunned Gain value LPSD: %d DMIC: %d AMIC: %d\n",
			data->lpsdgain, data->dmicgain, data->amicgain);

	data->dmab.area = dmam_alloc_coherent(dev, BUFFER_BYTES_MAX, &data->dmab.addr, GFP_KERNEL);
	if (data->dmab.area == NULL) {
		result = -ENOMEM;
		goto error;
	}
	data->dmab.bytes = BUFFER_BYTES_MAX/2;
	data->dmab.dev.dev = dev;
	data->dmab.dev.type = SNDRV_DMA_TYPE_DEV;

	data->dmab_rec.area = (data->dmab.area + BUFFER_BYTES_MAX/2);
	data->dmab_rec.addr = (data->dmab.addr + BUFFER_BYTES_MAX/2);
	data->dmab_rec.bytes = BUFFER_BYTES_MAX/2;
	data->dmab_rec.dev.dev = dev;
	data->dmab_rec.dev.type = SNDRV_DMA_TYPE_DEV;

	data->dmab_log.area = dmam_alloc_coherent(dev, LOG_BUFFER_BYTES_MAX,
				&data->dmab_log.addr, GFP_KERNEL);
	if (data->dmab_log.area == NULL) {
		result = -ENOMEM;
		goto error;
	}
	data->dmab.bytes = LOG_BUFFER_BYTES_MAX;
	data->dmab.dev.dev = dev;
	data->dmab.dev.type = SNDRV_DMA_TYPE_DEV;

	data->clk_rco = devm_clk_get_and_prepare(dev, "rco");
	if (IS_ERR(data->clk_rco)) {
		result = PTR_ERR(data->clk_rco);
		goto error;
	}

	result = clk_enable(data->clk_rco);
	if (IS_ERR_VALUE(result)) {
		dev_err(dev, "Failed to enable the rco\n");
		goto error;
	}

	data->clk_dmic = devm_clk_get_and_prepare(dev, "dmic");
	if (IS_ERR(data->clk_dmic)) {
		result = PTR_ERR(data->clk_dmic);
		goto error;
	}

	data->clk_dmic_if= devm_clk_get_and_prepare(dev, "dmic_if");
	if (IS_ERR(data->clk_dmic_if)) {
		result = PTR_ERR(data->clk_dmic_if);
		goto error;
	}

	data->clk_dmic_sync = devm_clk_get_and_prepare(dev, "dmic_sync");
	if (IS_ERR(data->clk_dmic_sync)) {
		result = PTR_ERR(data->clk_dmic_sync);
		goto error;
	}

	result = samsung_vts_devm_request_threaded_irq(pdev, "error",
			VTS_IRQ_VTS_ERROR, vts_error_handler);
	if (IS_ERR_VALUE(result)) {
		goto error;
	}

	result = samsung_vts_devm_request_threaded_irq(pdev, "boot_completed",
			VTS_IRQ_VTS_BOOT_COMPLETED, vts_boot_completed_handler);
	if (IS_ERR_VALUE(result)) {
		goto error;
	}

	result = samsung_vts_devm_request_threaded_irq(pdev, "ipc_received",
			VTS_IRQ_VTS_IPC_RECEIVED, vts_ipc_received_handler);
	if (IS_ERR_VALUE(result)) {
		goto error;
	}

	result = samsung_vts_devm_request_threaded_irq(pdev, "voice_triggered",
			VTS_IRQ_VTS_VOICE_TRIGGERED, vts_voice_triggered_handler);
	if (IS_ERR_VALUE(result)) {
		goto error;
	}

	result = samsung_vts_devm_request_threaded_irq(pdev, "trigger_period_elapsed",
			VTS_IRQ_VTS_PERIOD_ELAPSED, vts_trigger_period_elapsed_handler);
	if (IS_ERR_VALUE(result)) {
		goto error;
	}

	result = samsung_vts_devm_request_threaded_irq(pdev, "record_period_elapsed",
			VTS_IRQ_VTS_REC_PERIOD_ELAPSED, vts_record_period_elapsed_handler);
	if (IS_ERR_VALUE(result)) {
		goto error;
	}

	data->irq_state = true;

	data->pdev_mailbox = of_find_device_by_node(of_parse_phandle(np, "mailbox", 0));
	if (!data->pdev_mailbox) {
		dev_err(dev, "Failed to get mailbox\n");
		result = -EPROBE_DEFER;
		goto error;
	}

	result = request_firmware_nowait(THIS_MODULE,
			FW_ACTION_HOTPLUG,
			"vts.bin",
			dev,
			GFP_KERNEL,
			pdev,
			vts_complete_firmware_request);
	if (IS_ERR_VALUE(result)) {
		dev_err(dev, "Failed to request firmware\n");
		goto error;
	}

	data->regmap_dmic = devm_regmap_init_mmio_clk(dev,
			NULL,
			data->dmic_base,
			&vts_codec_regmap_config);

	result = snd_soc_register_codec(dev, &vts_codec, vts_dai, ARRAY_SIZE(vts_dai));
	if (IS_ERR_VALUE(result)) {
		dev_err(dev, "Failed to register ASoC codec\n");
		goto error;
	}

#ifdef EMULATOR
	pmu_alive = ioremap(0x16480000, 0x10000);
#endif
	pm_runtime_enable(dev);
	pm_runtime_get_sync(dev);

	vts_cfg_gpio(dev, "idle");

	data->voicecall_enabled = false;
	data->voicerecog_start = 0;
	data->syssel_rate = 0;
	data->target_size = 0;
	data->vtsfw_version = 0x0;
	data->vtsdetectlib_version = 0x0;
	dmic_clkctrl = readl(data->sfr_base + VTS_DMIC_CLK_CTRL);
	writel(dmic_clkctrl & ~(0x1 << VTS_CLK_ENABLE_OFFSET),
				data->sfr_base + VTS_DMIC_CLK_CTRL);
	dev_dbg(dev, "DMIC_CLK_CTRL: Before 0x%x After 0x%x \n", dmic_clkctrl,
			readl(data->sfr_base + VTS_DMIC_CLK_CTRL));

	result = device_create_file(dev, &dev_attr_vtsfw_version);
	if (IS_ERR_VALUE(result))
		dev_warn(dev, "Failed to create file: %s\n", "vtsfw_version");

	result = device_create_file(dev, &dev_attr_vtsdetectlib_version);
	if (IS_ERR_VALUE(result))
		dev_warn(dev, "Failed to create file: %s\n", "vtsdetectlib_version");

	data->sramlog_baseaddr = (char *)(data->sram_base + VTS_SRAMLOG_MSGS_OFFSET);

	atomic_notifier_chain_register(&panic_notifier_list, &vts_panic_notifier);

	device_init_wakeup(dev, true);
	dev_info(dev, "Probed successfully\n");

error:
	return result;
}

static int samsung_vts_remove(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct vts_data *data = platform_get_drvdata(pdev);

	pm_runtime_disable(dev);
	clk_unprepare(data->clk_dmic);
#ifndef CONFIG_PM
	vts_runtime_suspend(dev);
#endif
	release_firmware(data->firmware);
	snd_soc_unregister_codec(dev);
#ifdef EMULATOR
	iounmap(pmu_alive);
#endif
	return 0;
}

static struct platform_driver samsung_vts_driver = {
	.probe  = samsung_vts_probe,
	.remove = samsung_vts_remove,
	.driver = {
		.name = "samsung-vts",
		.owner = THIS_MODULE,
		.of_match_table = of_match_ptr(exynos_vts_of_match),
		.pm = &samsung_vts_pm,
	},
};

module_platform_driver(samsung_vts_driver);

static int __init samsung_vts_late_initcall(void)
{
	pr_info("%s\n", __func__);

	if (p_vts_data && p_vts_data->pdev) {
		pm_runtime_put_sync(&p_vts_data->pdev->dev);
	} else {
		pr_err("%s: p_vts_data or p_vts_data->pdev is null", __func__);
	}
	return 0;
}
late_initcall(samsung_vts_late_initcall);

/* Module information */
MODULE_AUTHOR("Gyeongtaek Lee, <gt82.lee@samsung.com>");
MODULE_AUTHOR("Palli Satish Kumar Reddy, <palli.satish@samsung.com>");
MODULE_DESCRIPTION("Samsung Voice Trigger System");
MODULE_ALIAS("platform:samsung-vts");
MODULE_LICENSE("GPL");
