/*
 * Rockchip machine ASoC driver for boards using a ES8328 CODEC.
 *
 * Copyright (c) 2015, ROCKCHIP CORPORATION.  All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/gpio.h>
#include <linux/of_gpio.h>
#include <linux/delay.h>
#include <sound/core.h>
#include <sound/jack.h>
#include <sound/pcm.h>
#include <sound/pcm_params.h>
#include <sound/soc.h>
#include "rockchip_i2s.h"

#define DRV_NAME "rockchip-snd-es8328"

static int jack_gpio;
static struct gpio_desc *phone_ctl_gpio;
static struct snd_soc_jack_gpio hp_jack_gpios[] = {
	{
		.name = "hp-det",
		.report = SND_JACK_HEADPHONE | SND_JACK_LINEOUT,
		.invert = 0,
		.debounce_time = 200,
	}
};
static unsigned int use_count;

static struct snd_soc_jack headset_jack;

static int rk_headphone_jack_event(struct snd_soc_dapm_widget *widget,
				   struct snd_kcontrol *ctrl, int event)
{
	int on = !SND_SOC_DAPM_EVENT_OFF(event);

	if (phone_ctl_gpio)
		gpiod_set_value_cansleep(phone_ctl_gpio, on);

	return 0;
}

static const struct snd_soc_dapm_widget rk_dapm_widgets[] = {
	SND_SOC_DAPM_HP("Headphone Jack", rk_headphone_jack_event),
	SND_SOC_DAPM_MIC("Int Mic", NULL),
};

static const struct snd_soc_dapm_route rk_audio_map[] = {
	/* Input Lines */
	{"LINPUT1", NULL, "Int Mic"},
	{"RINPUT1", NULL, "Int Mic"},
	{"LINPUT1", NULL, "Mic Bias"},
	{"RINPUT1", NULL, "Mic Bias"},
	{"Mic Bias", NULL, "Int Mic"},
	{"Mic Bias", NULL, "Int Mic"},

	/* Output Lines */
	{"Headphone Jack", NULL, "LOUT2"},
	{"Headphone Jack", NULL, "ROUT2"},
};

static const struct snd_kcontrol_new rk_es_controls[] = {
	SOC_DAPM_PIN_SWITCH("Headphone Jack"),
	SOC_DAPM_PIN_SWITCH("Int Mic"),
};

static int rk_es8328_hw_params(struct snd_pcm_substream *substream,
			       struct snd_pcm_hw_params *params)
{
	int ret = 0;
	struct snd_soc_pcm_runtime *rtd = substream->private_data;
	struct snd_soc_dai *cpu_dai = rtd->cpu_dai;
	struct snd_soc_dai *codec_dai = rtd->codec_dai;
	int mclk;

	switch (params_rate(params)) {
	case 8000:
	case 16000:
	case 24000:
	case 32000:
	case 48000:
	case 96000:
		mclk = 12288000;
		break;
	case 11025:
	case 22050:
	case 44100:
	case 88200:
		mclk = 11289600;
		break;
	default:
		return -EINVAL;
	}

	ret = snd_soc_dai_set_sysclk(codec_dai, 0, mclk, SND_SOC_CLOCK_IN);
	if (ret < 0 && ret != -ENOTSUPP) {
		dev_err(codec_dai->dev, "Can't set codec clock %d\n", ret);
		return ret;
	}

	ret = snd_soc_dai_set_sysclk(cpu_dai, 0, mclk, SND_SOC_CLOCK_OUT);
	if (ret < 0 && ret != -ENOTSUPP) {
		dev_err(cpu_dai->dev, "Can't set cpu clock %d\n", ret);
		return ret;
	}

	use_count++;
	return 0;
}

static int rk_es8328_hw_free(struct snd_pcm_substream *substream)
{
	struct snd_soc_pcm_runtime *rtd = substream->private_data;
	struct snd_soc_dai *codec_dai = rtd->codec_dai;

	if (use_count) {
		use_count--;
		if (!use_count)
			snd_soc_dai_set_sysclk(codec_dai, 0, 0, SND_SOC_CLOCK_IN);
	} else
		dev_err(codec_dai->dev, "Unbalanced hw_free!\n");

	return 0;
}

static int rk_init(struct snd_soc_pcm_runtime *runtime)
{
	struct snd_soc_card *card = runtime->card;
	struct snd_soc_dapm_context *dapm = &card->dapm;
	int ret;

	snd_soc_dapm_nc_pin(dapm, "LOUT1");
	snd_soc_dapm_nc_pin(dapm, "ROUT1");

	if (gpio_is_valid(jack_gpio)) {
		ret = snd_soc_card_jack_new(card, "Headphone Jack",
					    SND_JACK_HEADPHONE | SND_JACK_LINEOUT,
					    &headset_jack, NULL, 0);
		if (ret < 0) {
			dev_err(card->dev, "New Headset Jack failed! (%d)\n", ret);
			return ret;
		}

		ret = snd_soc_jack_add_gpiods(card->dev, &headset_jack,
					      ARRAY_SIZE(hp_jack_gpios),
					      hp_jack_gpios);
	}

	return ret;
}

static struct snd_soc_ops rk_es8328_ops = {
	.hw_params = rk_es8328_hw_params,
	.hw_free = rk_es8328_hw_free,
};

static struct snd_soc_dai_link rk_dailink = {
	.name = "es8328",
	.stream_name = "es8328 PCM",
	.codec_dai_name = "es8328-hifi-analog",
	.init = rk_init,
	.ops = &rk_es8328_ops,
	/* set es8328 as master - required for ADC since ALRCK is NC */
	.dai_fmt = SND_SOC_DAIFMT_I2S | SND_SOC_DAIFMT_NB_NF |
		SND_SOC_DAIFMT_CBM_CFM,
	.symmetric_rates = 1,
};

static struct snd_soc_card snd_soc_card_rk = {
	.name = "I2S-ES8328",
	.owner = THIS_MODULE,
	.dai_link = &rk_dailink,
	.num_links = 1,
	.dapm_widgets = rk_dapm_widgets,
	.num_dapm_widgets = ARRAY_SIZE(rk_dapm_widgets),
	.dapm_routes = rk_audio_map,
	.num_dapm_routes = ARRAY_SIZE(rk_audio_map),
	.controls = rk_es_controls,
	.num_controls = ARRAY_SIZE(rk_es_controls),
};

static int snd_rk_mc_probe(struct platform_device *pdev)
{
	int ret = 0;
	struct snd_soc_card *card = &snd_soc_card_rk;
	struct device_node *np = pdev->dev.of_node;

	dev_err(&pdev->dev, "%s\n", __func__);

	/* register the soc card */
	card->dev = &pdev->dev;

	jack_gpio = of_get_named_gpio(np, "hp-det-gpio", 0);
	if (jack_gpio == -EPROBE_DEFER)
		return -EPROBE_DEFER;

	phone_ctl_gpio = devm_gpiod_get(&pdev->dev, "phone-ctl", GPIOD_OUT_HIGH);
	if (IS_ERR(phone_ctl_gpio)) {
		if (PTR_ERR(phone_ctl_gpio) != -ENOENT)
			return PTR_ERR(phone_ctl_gpio);

		phone_ctl_gpio = NULL;
	}

	rk_dailink.codec_of_node = of_parse_phandle(np,
			"rockchip,audio-codec", 0);
	if (!rk_dailink.codec_of_node) {
		dev_err(&pdev->dev,
			"Property 'rockchip,audio-codec' missing or invalid\n");
		return -EINVAL;
	}

	rk_dailink.cpu_of_node = of_parse_phandle(np,
			"rockchip,i2s-controller", 0);
	if (!rk_dailink.cpu_of_node) {
		dev_err(&pdev->dev,
			"Property 'rockchip,i2s-controller' missing or invalid\n");
		return -EINVAL;
	}

	rk_dailink.platform_of_node = rk_dailink.cpu_of_node;

	ret = snd_soc_of_parse_card_name(card, "rockchip,model");
	if (ret) {
		dev_err(&pdev->dev,
			"Soc parse card name failed %d\n", ret);
		return ret;
	}

	ret = devm_snd_soc_register_card(&pdev->dev, card);
	if (ret) {
		dev_err(&pdev->dev,
			"Soc register card failed %d\n", ret);
		return ret;
	}

	return ret;
}

static int snd_rk_mc_remove(struct platform_device *pdev)
{
	if (gpio_is_valid(jack_gpio))
		snd_soc_jack_free_gpios(&headset_jack,
					ARRAY_SIZE(hp_jack_gpios),
					hp_jack_gpios);
	return 0;
}

static const struct of_device_id rockchip_es8328_of_match[] = {
	{ .compatible = "rockchip,rockchip-audio-es8328", },
	{},
};
MODULE_DEVICE_TABLE(of, rockchip_es8328_of_match);

static struct platform_driver snd_rk_mc_driver = {
	.probe = snd_rk_mc_probe,
	.remove = snd_rk_mc_remove,
	.driver = {
		.name = DRV_NAME,
		.pm = &snd_soc_pm_ops,
		.of_match_table = rockchip_es8328_of_match,
	},
};
module_platform_driver(snd_rk_mc_driver);

MODULE_AUTHOR("John Keeping <john@metanate.com>");
MODULE_DESCRIPTION("Rockchip es8328 machine ASoC driver");
MODULE_LICENSE("GPL v2");
MODULE_ALIAS("platform:" DRV_NAME);
