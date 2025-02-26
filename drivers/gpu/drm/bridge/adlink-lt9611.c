// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2018, The Linux Foundation. All rights reserved.
 * Copyright (c) 2019-2020. Linaro Limited.
 */
#include <linux/gpio/consumer.h>
#include <linux/i2c.h>
#include <linux/interrupt.h>
#include <linux/media-bus-format.h>
#include <linux/module.h>
#include <linux/of_graph.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/regulator/consumer.h>

#include <sound/hdmi-codec.h>

#include <drm/drm_atomic_helper.h>
#include <drm/drm_bridge.h>
#include <drm/drm_mipi_dsi.h>
#include <drm/drm_of.h>
#include <drm/drm_print.h>
#include <drm/drm_probe_helper.h>

#include <linux/clk.h>
#include <media/cec.h>

#define EDID_SEG_SIZE	256
#define EDID_LEN	32
#define EDID_LOOP	8
#define KEY_DDC_ACCS_DONE 0x02
#define DDC_NO_ACK	0x50

#define LT9611_4LANES	0
#define AR_16_9 0x02 
#define AR_4_3 0x01
#define	MPEG_PKT_EN 0x01
#define	AIF_PKT_EN  0x02
#define SPD_PKT_EN	0x04//9611 RXΪMIPI������Ҫ��SPD_PKT
#define AVI_PKT_EN	0x08
#define UD1_PKT_EN	0x10
#define UD0_PKT_EN	0x20
#define PREFER_MODE 0

static int lt9611_pattern_enabled;

struct lt9611 {
	struct device *dev;
	struct drm_bridge bridge;
	struct drm_connector connector;

	struct regmap *regmap;

	struct device_node *dsi0_node;
	struct device_node *dsi1_node;
	struct mipi_dsi_device *dsi0;
	struct mipi_dsi_device *dsi1;
	struct platform_device *audio_pdev;

	bool ac_mode;

	struct gpio_desc *reset_gpio;
	struct gpio_desc *enable_gpio;

	bool power_on;
	bool sleep;
	bool is4k;

	struct i2c_client *client;

	enum drm_connector_status status;

	struct cec_adapter *cec_adap;

	u8 edid_buf[EDID_SEG_SIZE];
	u32 vic;
	struct clk *vco;
	struct clk *videopll1;
	struct clk *pix;
#if PREFER_MODE
	int prefer_mode;
#endif
};


#define LT9611_PAGE_CONTROL	0xff

static const struct regmap_range_cfg lt9611_ranges[] = {
	{
		.name = "register_range",
		.range_min =  0,
		.range_max = 0x85ff,
		.selector_reg = LT9611_PAGE_CONTROL,
		.selector_mask = 0xff,
		.selector_shift = 0,
		.window_start = 0,
		.window_len = 0x100,
	},
};

static const struct regmap_config lt9611_regmap_config = {
	.reg_bits = 8,
	.val_bits = 8,
	.max_register = 0xffff,
	.ranges = lt9611_ranges,
	.num_ranges = ARRAY_SIZE(lt9611_ranges),
};

struct lt9611_mode {
	u16 hdisplay;
	u16 vdisplay;
	u8 vrefresh;
	u8 lanes;
	u8 intfs;
	unsigned long vco;
	unsigned long videopll1;
	unsigned long pix;
	bool is4k;
	u32 vic;
};

static struct lt9611_mode lt9611_modes[] = {
	{ 4096, 2160, 30, 4, 1 , 2673000000 , 297000000 , 297000000 , TRUE , 100 }, /* 4096x2160 24bit 30Hz 4Lane 1port */
	{ 3840, 2160, 30, 4, 1 , 2673000000 , 297000000 , 297000000 , TRUE , 95 }, /* 3840x2160 24bit 30Hz 4Lane 1port */
	{ 1920, 1080, 60, 4, 1 , 4008000000 , 445333334 , 148444444 , FALSE , 16 }, /* 1080P 24bit 60Hz 4lane 1port */
	{ 1920, 1200, 60, 4, 1 , 4158000000 , 462000000 , 154000000 , FALSE , 0 }, /* 1920x1200 24bit 60Hz 4lane 1port */
	{ 1536, 2048, 61, 4, 1 , 2655000000 , 295000000 , 295000000 , TRUE , 0 }, /* 1536x2048 24bit 60Hz 4lane 1port */
};


typedef struct video_timing{
u16 hfp;
u16 hs;
u16 hbp;
u16 hact;
u16 htotal;
u16 vfp;
u16 vs;
u16 vbp;
u16 vact;
u16 vtotal;
bool h_polarity;
bool v_polarity;
u16	vic;
u8 aspact_ratio;  // 0=no data, 1=4:3, 2=16:9, 3=no data.
u32 pclk_khz;
};

struct video_timing video_3840x2160_30Hz   =    {176,  88, 296, 3840, 4400,  8,  10, 72, 2160, 2250, 1,1,95,AR_16_9, 297000};
struct video_timing video_4096x2160_30Hz   =    {88,  88, 128, 4096, 4400,  8,  10, 72, 2160, 2250, 1,1,100,AR_4_3, 297000};
struct video_timing video_1920x1080_60Hz   =    {88,  44, 148, 1920,  2200,  4,  5,  36, 1080, 1125, 1,1,16,AR_16_9,148500};
struct video_timing video_1920x1200_60Hz   =    {48,  32, 80, 1920,  2080,  3,  6,  26, 1200, 1235, 1,1,0,AR_16_9,154128};
//video_1536x2048_60Hz
struct video_timing video_1536x2048_61Hz   =    {200,  64, 460, 1536,  2260,  60,  10,  10, 2048, 2128, 1,1,0,AR_16_9,295000};

#define DVI 0x00
#define HDMI 0x01
#define UD0_PKT_EN	0x20

void LT9611_System_Init(struct lt9611 *lt9611)
{
	regmap_write(lt9611->regmap, 0x8251, 0x11);
	//Timer for Frequency meter
	regmap_write(lt9611->regmap, 0x821b, 0x69);
	regmap_write(lt9611->regmap, 0x821c, 0x78);
	regmap_write(lt9611->regmap, 0x82cb, 0x69);//Timer 1
	regmap_write(lt9611->regmap, 0x82cc, 0x78);
	/*power consumption for work*/
	regmap_write(lt9611->regmap, 0x8004, 0xf0);
	regmap_write(lt9611->regmap, 0x8006, 0xf0);
	regmap_write(lt9611->regmap, 0x800a, 0x80);
	regmap_write(lt9611->regmap, 0x800b, 0x46);	//csc clk//46
	regmap_write(lt9611->regmap, 0x800d, 0xef);
	regmap_write(lt9611->regmap, 0x8011, 0xfa);
}

void LT9611_pattern_en(struct lt9611 *lt9611)
{
	regmap_write(lt9611->regmap, 0x824f, 0x80);//[7] = Select ad_txpll_d_clk.
	regmap_write(lt9611->regmap, 0x8250, 0x20);
}

void LT9611_PLL(struct lt9611 *lt9611, struct video_timing *video_format)
{
	u32 pclk;
	unsigned int pll_lock_flag, cal_done_flag, band_out;
	u8 hdmi_post_div;
	u8 i;
	pclk = video_format->pclk_khz;
	u8 pcr_m;

	printk("\r\nLT9611_PLL: set rx pll = %d\n", pclk);

	regmap_write(lt9611->regmap, 0x8123, 0x40);//Enable LDO and disable PD
	regmap_write(lt9611->regmap, 0x8124, 0x62);//0x62, LG25UM58 issue, 20180824
	regmap_write(lt9611->regmap, 0x8125, 0x80);//pre-divider
	regmap_write(lt9611->regmap, 0x8126, 0x55);
	regmap_write(lt9611->regmap, 0x812c, 0x37);
	regmap_write(lt9611->regmap, 0x812f, 0x01);
	regmap_write(lt9611->regmap, 0x8127, 0x66);
	regmap_write(lt9611->regmap, 0x8128, 0x88);
	regmap_write(lt9611->regmap, 0x812a, 0x20);//for U3.

	if(pclk > 150000)
	{
		regmap_write(lt9611->regmap, 0x812d, 0x88);
		hdmi_post_div = 0x01;
	}
	else if(pclk > 80000)
	{
		regmap_write(lt9611->regmap, 0x812d, 0x99);
		hdmi_post_div = 0x02;
	}
	else
	{
		regmap_write(lt9611->regmap, 0x812d, 0xaa);
		hdmi_post_div = 0x04;
	}

	pcr_m = (u8)((pclk*5*hdmi_post_div)/27000);
    pcr_m --;
    printk("\r\nLT9611_PLL: pcr_m = 0x%x, hdmi_post_div = %d", pcr_m, hdmi_post_div); //Hex

	regmap_write(lt9611->regmap, 0x832d, 0x40);// M up limit
	regmap_write(lt9611->regmap, 0x8331, 0x08);// M down limit
	regmap_write(lt9611->regmap, 0x8326, 0x80|pcr_m);/* fixed M is to let pll locked*/

	pclk = pclk / 2;
	regmap_write(lt9611->regmap, 0x82e3, pclk/65536);// 13.5M
	pclk = pclk % 65536;
	regmap_write(lt9611->regmap, 0x82e4, pclk/256);
	regmap_write(lt9611->regmap, 0x82e5, pclk%256);

	regmap_write(lt9611->regmap, 0x82de, 0x20);// pll cal en, start calibration
	regmap_write(lt9611->regmap, 0x82de, 0xe0);

	regmap_write(lt9611->regmap, 0x8011, 0x5a);/* Pcr clk reset */
	regmap_write(lt9611->regmap, 0x8011, 0xfa);

	regmap_write(lt9611->regmap, 0x8016, 0xf2);/* pll cal digital reset */
	regmap_write(lt9611->regmap, 0x8018, 0xdc);/* pll analog reset */
	regmap_write(lt9611->regmap, 0x8018, 0xfc);
	regmap_write(lt9611->regmap, 0x8016, 0xf3);/*start calibration*/

	/* pll lock status */
	for(i = 0; i < 6 ; i++)
	{
		regmap_write(lt9611->regmap, 0x8016, 0xe3);/* pll lock logic reset */
		regmap_write(lt9611->regmap, 0x8016, 0xf3);

		regmap_read(lt9611->regmap, 0x82e7, (unsigned int*)&cal_done_flag);
		regmap_read(lt9611->regmap, 0x82e6, (unsigned int*)&band_out);
		regmap_read(lt9611->regmap, 0x8215, (unsigned int*)&pll_lock_flag);

		if((pll_lock_flag & 0x80)&&(cal_done_flag & 0x80)&&(band_out != 0xff))
		{
			printk("\r\nLT9611_PLL: HDMI pll locked band out: 0x%x", band_out);
			break;
		}
		else
		{
			regmap_write(lt9611->regmap, 0x8011, 0x5a);/* Pcr clk reset */
			regmap_write(lt9611->regmap, 0x8011, 0xfa);
			regmap_write(lt9611->regmap, 0x8016, 0xf2);/* pll cal digital reset */
			regmap_write(lt9611->regmap, 0x8018, 0xdc);/* pll analog reset */
			regmap_write(lt9611->regmap, 0x8018, 0xfc);
			regmap_write(lt9611->regmap, 0x8016, 0xf3);/*start calibration*/
			printk("\r\nLT9611_PLL: HDMI pll unlocked, reset pll");
		}
	}
}

void LT9611_pattern_gcm(struct lt9611 *lt9611, struct video_timing *video_format)
{
	u8 POL;
	POL = (video_format-> h_polarity)*0x10 + (video_format-> v_polarity)*0x20;
	POL = ~POL;
	POL &= 0x30;

	regmap_write(lt9611->regmap, 0x82a3, (u8)((video_format->hs+video_format->hbp)/256));//de_delay
	regmap_write(lt9611->regmap, 0x82a4, (u8)((video_format->hs+video_format->hbp)%256));
	regmap_write(lt9611->regmap, 0x82a5, (u8)((video_format->vs+video_format->vbp)%256));//de_top
	regmap_write(lt9611->regmap, 0x82a6, (u8)(video_format->hact/256));
	regmap_write(lt9611->regmap, 0x82a7, (u8)(video_format->hact%256));//de_cnt
	regmap_write(lt9611->regmap, 0x82a8, (u8)(video_format->vact/256));
	regmap_write(lt9611->regmap, 0x82a9, (u8)(video_format->vact%256));//de_line
	regmap_write(lt9611->regmap, 0x82aa, (u8)(video_format->htotal/256));
	regmap_write(lt9611->regmap, 0x82ab, (u8)(video_format->htotal%256));//htotal
	regmap_write(lt9611->regmap, 0x82ac, (u8)(video_format->vtotal/256));
	regmap_write(lt9611->regmap, 0x82ad, (u8)(video_format->vtotal%256));//vtotal
	regmap_write(lt9611->regmap, 0x82ae, (u8)(video_format->hs/256));
	regmap_write(lt9611->regmap, 0x82af, (u8)(video_format->hs%256));//hvsa
	regmap_write(lt9611->regmap, 0x82b0, (u8)(video_format->vs%256));//vsa
	regmap_write(lt9611->regmap, 0x8247, (u8)(POL|0x07));//sync polarity
}

void LT9611_HDMI_TX_Digital(struct lt9611 *lt9611, struct video_timing *video_format) //dsren
{
	u8 VIC = video_format->vic;
	u8 AR = video_format->aspact_ratio;
	u8 pb0,pb2,pb4;
	u8 infoFrame_en;

	infoFrame_en = (AIF_PKT_EN|AVI_PKT_EN|SPD_PKT_EN);
	pb2 =  (AR<<4) + 0x08;
	pb4 =  VIC;
	pb0 = ((pb2 + pb4) <= 0x5f)?(0x5f - pb2 - pb4):(0x15f - pb2 - pb4);

	regmap_write(lt9611->regmap, 0x82d6, 0x8e);
	regmap_write(lt9611->regmap, 0x82d7, 0x04);

	//AVI
	regmap_write(lt9611->regmap, 0x8443, pb0);//AVI_PB0
	regmap_write(lt9611->regmap, 0x8445, pb2);//AVI_PB2
	regmap_write(lt9611->regmap, 0x8447, pb4);//AVI_PB4
	regmap_write(lt9611->regmap, 0x8410, 0x02);//data iland
	regmap_write(lt9611->regmap, 0x8412, 0x64);//act_h_blank

	//VS_IF, 4k 30hz need send VS_IF packet.
	if(VIC == 95)
	{
		regmap_write(lt9611->regmap, 0x8413d, infoFrame_en|UD0_PKT_EN);//UD1 infoframe enable //revise on 20200715
		regmap_write(lt9611->regmap, 0x8474, 0x81);//HB0
		regmap_write(lt9611->regmap, 0x8475, 0x01);//HB1
		regmap_write(lt9611->regmap, 0x8476, 0x05);//HB2
		regmap_write(lt9611->regmap, 0x8477, 0x49);//PB0
		regmap_write(lt9611->regmap, 0x8478, 0x03);//PB1
		regmap_write(lt9611->regmap, 0x8479, 0x0c);//PB2
		regmap_write(lt9611->regmap, 0x847a, 0x00);//PB3
		regmap_write(lt9611->regmap, 0x847b, 0x20);//PB4
		regmap_write(lt9611->regmap, 0x847c, 0x01);//PB5
	}
	else
	{
		regmap_write(lt9611->regmap, 0x843d, infoFrame_en);//UD1 infoframe enable
	}

	if(infoFrame_en&&SPD_PKT_EN)
	{
		regmap_write(lt9611->regmap, 0x84c0, 0x00);//HB0
		regmap_write(lt9611->regmap, 0x84c1, 0x00);//HB1
		regmap_write(lt9611->regmap, 0x84c2, 0x00);//HB2

		regmap_write(lt9611->regmap, 0x84c3, 0x00);//PB0
		regmap_write(lt9611->regmap, 0x84c4, 0x00);//PB1
		regmap_write(lt9611->regmap, 0x84c5, 0x00);//PB2
		regmap_write(lt9611->regmap, 0x84c6, 0x00);//PB3
		regmap_write(lt9611->regmap, 0x84c7, 0x00);//PB4
		regmap_write(lt9611->regmap, 0x84c8, 0x00);//PB5
	}
}

void LT9611_HDMI_TX_Phy(struct lt9611 *lt9611)
{
	regmap_write(lt9611->regmap, 0x8130, 0x6a);//tap0/1

	regmap_write(lt9611->regmap, 0x8131, 0x73);//DC: 0x44, AC:0x73
	regmap_write(lt9611->regmap, 0x8132, 0x4a);
	regmap_write(lt9611->regmap, 0x8133, 0x0b);
	regmap_write(lt9611->regmap, 0x8134, 0x00);
	regmap_write(lt9611->regmap, 0x8135, 0x00);
	regmap_write(lt9611->regmap, 0x8136, 0x00);
	regmap_write(lt9611->regmap, 0x8137, 0x44);
	regmap_write(lt9611->regmap, 0x813f, 0x0f);
	regmap_write(lt9611->regmap, 0x8140, 0x98);//clk swing
	regmap_write(lt9611->regmap, 0x8141, 0x98);//D0 swing
	regmap_write(lt9611->regmap, 0x8142, 0x98);//D1 swing
	regmap_write(lt9611->regmap, 0x8143, 0x98);//D2 swing
	regmap_write(lt9611->regmap, 0x8144, 0x0a);
}

void LT9611_HDCP_Disable(struct lt9611 *lt9611)
{
	regmap_write(lt9611->regmap, 0x8515, 0x45);
}

void LT9611_HDMI_Out_Enable(struct lt9611 *lt9611)
{
	regmap_write(lt9611->regmap, 0x8123, 0x40);
	regmap_write(lt9611->regmap, 0x82de, 0x20);
	regmap_write(lt9611->regmap, 0x82de, 0xe0);
	regmap_write(lt9611->regmap, 0x8018, 0xdc);/* txpll sw rst */
	regmap_write(lt9611->regmap, 0x8018, 0xfc);
	regmap_write(lt9611->regmap, 0x8016, 0xf1);/* txpll calibration rest */
	regmap_write(lt9611->regmap, 0x8016, 0xf3);
	regmap_write(lt9611->regmap, 0x8011, 0x5a);//Pcr reset
	regmap_write(lt9611->regmap, 0x8011, 0xfa);
	regmap_write(lt9611->regmap, 0x8130, 0xea);

	LT9611_HDCP_Disable(lt9611);
}
void LT9611_test_pattern(struct lt9611 *lt9611)
{
	struct video_timing *video;

	if(lt9611_pattern_enabled == 1)
		video = &video_3840x2160_30Hz;
	else if(lt9611_pattern_enabled == 2)
		video = &video_1920x1080_60Hz;
	else if(lt9611_pattern_enabled == 3)
		video = &video_4096x2160_30Hz;
	else if(lt9611_pattern_enabled == 4)
		video = &video_1920x1200_60Hz;

	LT9611_System_Init(lt9611);
	LT9611_pattern_en(lt9611);
	LT9611_PLL(lt9611, video);
	LT9611_pattern_gcm(lt9611, video);
	LT9611_HDMI_TX_Digital(lt9611, video);
	LT9611_HDMI_TX_Phy(lt9611);
	LT9611_HDMI_Out_Enable(lt9611);
}

static struct lt9611 *bridge_to_lt9611(struct drm_bridge *bridge)
{
	return container_of(bridge, struct lt9611, bridge);
}

static struct lt9611 *connector_to_lt9611(struct drm_connector *connector)
{
	return container_of(connector, struct lt9611, connector);
}

static void prepare_clk(struct lt9611 *lt9611, struct lt9611_mode *lt9611_mode)
{
	int ret;
	if (lt9611->vco)
	{
		printk("lt9611 prepare_clk vco:%ld\n",clk_get_rate(lt9611->vco));
		clk_prepare_enable(lt9611->vco);
		clk_disable_unprepare(lt9611->vco);

		printk("lt9611 prepare_clk vco:%ld\n", lt9611_mode->vco);
		clk_set_rate(lt9611->vco, lt9611_mode->vco);
		ret = clk_prepare_enable(lt9611->vco);
		if (ret < 0) {
			printk("Failed to enable vco\n");
		}
		else
		{
			printk("lt9611 prepare_clk enable vco:%ld\n",clk_get_rate(lt9611->vco));
		}
	}

	if (lt9611->videopll1)
	{
		printk("lt9611 prepare_clk videopll1:%ld\n",clk_get_rate(lt9611->videopll1));
		clk_prepare_enable(lt9611->videopll1);
		clk_disable_unprepare(lt9611->videopll1);

		printk("lt9611 prepare_clk videopll1:%ld\n", lt9611_mode->videopll1);
		clk_set_rate(lt9611->videopll1, lt9611_mode->videopll1);
		ret = clk_prepare_enable(lt9611->videopll1);
		if (ret < 0) {
			printk("Failed to enable videopll1\n");
		}
		else
		{
			printk("lt9611 prepare_clk enable videopll1:%ld\n",clk_get_rate(lt9611->videopll1));
		}
	}

	if (lt9611->pix)
	{
		printk("lt9611 prepare_clk pix:%ld\n",clk_get_rate(lt9611->pix));
		clk_prepare_enable(lt9611->pix);
		clk_disable_unprepare(lt9611->pix);

		printk("lt9611 prepare_clk pix:%ld\n", lt9611_mode->pix);
		clk_set_rate(lt9611->pix, lt9611_mode->pix);
		ret = clk_prepare_enable(lt9611->pix);
		if (ret < 0) {
			printk("Failed to enable pix\n");
		}
		else
		{
			printk("lt9611 prepare_clk enable pix:%ld\n",clk_get_rate(lt9611->pix));
		}
	}
}

static int lt9611_mipi_input_analog(struct lt9611 *lt9611)
{
	const struct reg_sequence reg_cfg[] = {
		{ 0x8106, 0x60 }, /* port A rx current */
		{ 0x8107, 0x1b }, //eq 5:3 for lane0 2:0 for lane1
		{ 0x8108, 0x1b }, //eq 5:3 lane2 2:0 lane3
		{ 0x810a, 0xfe }, /* port A ldo voltage set */
		{ 0x810b, 0xbf }, /* enable port A lprx */
		{ 0x811c, 0x03 }, /* PortA clk lane no-LP mode */
	};
	if(lt9611_pattern_enabled) return 0;

	return regmap_multi_reg_write(lt9611->regmap, reg_cfg, ARRAY_SIZE(reg_cfg));
}

static void lt9611_mipi_input_digital(struct lt9611 *lt9611)
{
	int settle_count;
	int temp;
	int i;

	if(lt9611_pattern_enabled) return;

	if(lt9611->is4k)
		settle_count=0x16;
	else
		settle_count=0x0b;

	regmap_write(lt9611->regmap, 0x8302, settle_count);
	regmap_write(lt9611->regmap, 0x8306, settle_count);

	regmap_write(lt9611->regmap, 0x824f, 0x80);
	regmap_write(lt9611->regmap, 0x8250, 0x10);
	regmap_write(lt9611->regmap, 0x8303, 0x00);
	for(i=0;i<10;i++)
	{
		regmap_read(lt9611->regmap, 0x8398, &temp);
		printk("port A lane 0 settle = 0x%x loop:%d\n",temp,i);
		if(temp)
			break;
		msleep(10);
	}
	printk("port A lane 0 settle = 0x%x loop:%d\n",temp,i);
	regmap_read(lt9611->regmap, 0x839a, &temp);
	printk("port A lane 1 settle = 0x%x \n",temp);
	regmap_read(lt9611->regmap, 0x839c, &temp);
	printk("port A lane 2 settle = 0x%x \n",temp);
	regmap_read(lt9611->regmap, 0x839e, &temp);
	printk("port A lane 3 settle = 0x%x \n",temp);
	regmap_read(lt9611->regmap, 0x8398, &temp);
	settle_count = temp;

	if(settle_count > 0x40)
	{
		settle_count = 0x06;
	}
	else
	{
		if(settle_count)
			settle_count = settle_count/2;
		else
		{
			if(lt9611->is4k)
				settle_count = 0x16;
			else
				settle_count = 0x0b;
		}
	}
	printk("settle = 0x%x \n",settle_count);

	regmap_write(lt9611->regmap, 0x8300, LT9611_4LANES);
	regmap_write(lt9611->regmap, 0x8302, settle_count);
	regmap_write(lt9611->regmap, 0x8306, settle_count);
	regmap_write(lt9611->regmap, 0x830a, 0x00);//single_port_mipi

}


static void lt9611_mipi_video_setup(struct lt9611 *lt9611,
				    const struct drm_display_mode *mode)
{
	u32 h_total, hactive, hsync_len, hfront_porch, hsync_porch;
	u32 v_total, vactive, vsync_len, vfront_porch, vsync_porch;

	h_total = mode->htotal;
	v_total = mode->vtotal;

	hactive = mode->hdisplay;
	hsync_len = mode->hsync_end - mode->hsync_start;
	hfront_porch = mode->hsync_start - mode->hdisplay;
	hsync_porch = hsync_len + mode->htotal - mode->hsync_end;

	vactive = mode->vdisplay;
	vsync_len = mode->vsync_end - mode->vsync_start;
	vfront_porch = mode->vsync_start - mode->vdisplay;
	vsync_porch = vsync_len + mode->vtotal - mode->vsync_end;
	
	if(lt9611_pattern_enabled) return;

	printk("lt9611_mipi_video_setup hactive:%d is4k:%d\n",hactive,lt9611->is4k);

	regmap_write(lt9611->regmap, 0x830d, (u8)(v_total / 256));
	regmap_write(lt9611->regmap, 0x830e, (u8)(v_total % 256));

	regmap_write(lt9611->regmap, 0x830f, (u8)(vactive / 256));
	regmap_write(lt9611->regmap, 0x8310, (u8)(vactive % 256));

	regmap_write(lt9611->regmap, 0x8311, (u8)(h_total / 256));
	regmap_write(lt9611->regmap, 0x8312, (u8)(h_total % 256));

	regmap_write(lt9611->regmap, 0x8313, (u8)(hactive / 256));
	regmap_write(lt9611->regmap, 0x8314, (u8)(hactive % 256));

	regmap_write(lt9611->regmap, 0x8315, (u8)(vsync_len % 256));
	regmap_write(lt9611->regmap, 0x8316, (u8)(hsync_len % 256));

	regmap_write(lt9611->regmap, 0x8317, (u8)(vfront_porch % 256));

	regmap_write(lt9611->regmap, 0x8318, (u8)(vsync_porch % 256));

	regmap_write(lt9611->regmap, 0x8319, (u8)(hfront_porch % 256));

	regmap_write(lt9611->regmap, 0x831a, (u8)(hsync_porch / 256));
	regmap_write(lt9611->regmap, 0x831b, (u8)(hsync_porch % 256));
}

static void lt9611_pcr_setup(struct lt9611 *lt9611, const struct drm_display_mode *mode , int pcr_m)
{
	int pol, h_polarity,v_polarity;
	int pcr;

	const struct reg_sequence reg_cfg[] = {
		{ 0x830b, 0x01 },
		{ 0x830c, 0x10 },
		{ 0x8348, 0x00 },
		{ 0x8349, 0x81 },

		/* stage 1 */
		{ 0x8321, 0x4a },
		{ 0x8324, 0x71 },
		{ 0x8325, 0x30 },
		{ 0x832a, 0x01 },


		{ 0x834a, 0x40 },
		{ 0x831d, 0x10 },
	};


	if(lt9611_pattern_enabled) return;

	h_polarity=1;
	v_polarity=1;
	pol = (h_polarity)*0x02 + (v_polarity);
	pol = ~pol;
	pol &= 0x03;

	regmap_multi_reg_write(lt9611->regmap, reg_cfg, ARRAY_SIZE(reg_cfg));
	/* stage 2 */
	regmap_write(lt9611->regmap, 0x834a, 0x40);
	regmap_write(lt9611->regmap, 0x831d, 0x10 | pol);

	lt9611_mipi_video_setup(lt9611, mode);

	regmap_write(lt9611->regmap, 0x8326, pcr_m);

	/* pcr rst */
	regmap_write(lt9611->regmap, 0x8011, 0x5a);
	regmap_write(lt9611->regmap, 0x8011, 0xfa);
}

static int lt9611_pll_setup(struct lt9611 *lt9611, const struct drm_display_mode *mode , int *pcr_m)
{
	unsigned int pclk = mode->clock;
	int hdmi_post_div ;
	unsigned int pll_lock_flag, cal_done_flag, band_out;
	int i;

	const struct reg_sequence reg_cfg[] = {
		/* txpll init */
		{ 0x8123, 0x40 },
		{ 0x8124, 0x62 },
		{ 0x8125, 0x80 },
		{ 0x8126, 0x55 },
		{ 0x812c, 0x37 },
		{ 0x812f, 0x01 },
		{ 0x8127, 0x66 },
		{ 0x8128, 0x88 },
		{ 0x812a, 0x20 },    //for U3
	};

	if(lt9611_pattern_enabled) return 0;

	regmap_multi_reg_write(lt9611->regmap, reg_cfg, ARRAY_SIZE(reg_cfg));

	if (pclk > 150000)
	{
		regmap_write(lt9611->regmap, 0x812d, 0x88);
		hdmi_post_div = 0x01;
	}
	else if (pclk > 80000)
	{
		regmap_write(lt9611->regmap, 0x812d, 0x99);
		hdmi_post_div = 0x02;
	}
	else
	{
		regmap_write(lt9611->regmap, 0x812d, 0xaa);
		hdmi_post_div = 0x04;
	}
	*pcr_m = ((pclk*5*hdmi_post_div)/27000);
	(*pcr_m) --;

	printk("\r\nlt9611_pll_setup: pcr_m = 0x%x, hdmi_post_div = %d", *pcr_m, hdmi_post_div); //Hex

	regmap_write(lt9611->regmap, 0x832d, 0x40);
	regmap_write(lt9611->regmap, 0x8331, 0x08);
	regmap_write(lt9611->regmap, 0x8326, 0x80| *pcr_m);

	/*
	 * first divide pclk by 2 first
	 *  - write divide by 64k to 19:16 bits which means shift by 17
	 *  - write divide by 256 to 15:8 bits which means shift by 9
	 *  - write remainder to 7:0 bits, which means shift by 1
	 */
	pclk = pclk / 2;
	regmap_write(lt9611->regmap, 0x82e3, pclk/65536);
	pclk = pclk % 65536;
	regmap_write(lt9611->regmap, 0x82e4, pclk/256);
	regmap_write(lt9611->regmap, 0x82e5, pclk%256);

	regmap_write(lt9611->regmap, 0x82de, 0x20);
	regmap_write(lt9611->regmap, 0x82de, 0xe0);

	regmap_write(lt9611->regmap, 0x8011, 0x5a);/* Pcr clk reset */
	regmap_write(lt9611->regmap, 0x8011, 0xfa);

	regmap_write(lt9611->regmap, 0x8016, 0xf2);/* pll cal digital reset */
	regmap_write(lt9611->regmap, 0x8018, 0xdc);/* pll analog reset */
	regmap_write(lt9611->regmap, 0x8018, 0xfc);
	regmap_write(lt9611->regmap, 0x8016, 0xf3);/*start calibration*/

	/* pll lock status */
	for(i = 0; i < 6 ; i++)
	{
		regmap_write(lt9611->regmap, 0x8016, 0xe3);/* pll lock logic reset */
		regmap_write(lt9611->regmap, 0x8016, 0xf3);

		regmap_read(lt9611->regmap, 0x82e7, (unsigned int*)&cal_done_flag);
		regmap_read(lt9611->regmap, 0x82e6, (unsigned int*)&band_out);
		regmap_read(lt9611->regmap, 0x8215, (unsigned int*)&pll_lock_flag);

		if((pll_lock_flag & 0x80)&&(cal_done_flag & 0x80)&&(band_out != 0xff))
		{
			printk("\r\nLT9611_PLL: HDMI pll locked band out: 0x%x", band_out);
			break;
		}
		else
		{
			regmap_write(lt9611->regmap, 0x8011, 0x5a);/* Pcr clk reset */
			regmap_write(lt9611->regmap, 0x8011, 0xfa);
			regmap_write(lt9611->regmap, 0x8016, 0xf2);/* pll cal digital reset */
			regmap_write(lt9611->regmap, 0x8018, 0xdc);/* pll analog reset */
			regmap_write(lt9611->regmap, 0x8018, 0xfc);
			regmap_write(lt9611->regmap, 0x8016, 0xf3);/*start calibration*/
			printk("\r\nLT9611_PLL: HDMI pll unlocked, reset pll");
		}
	}

	return 0;
}

static int lt9611_read_video_check(struct lt9611 *lt9611, unsigned int reg)
{
	unsigned int temp, temp2;
	int ret;

	ret = regmap_read(lt9611->regmap, reg, &temp);
	if (ret)
		return ret;
	temp <<= 8;
	ret = regmap_read(lt9611->regmap, reg + 1, &temp2);
	if (ret)
		return ret;

	return (temp + temp2);
}

static int lt9611_video_check(struct lt9611 *lt9611)
{
	u32 v_total, vactive, hactive_a, hactive_b, h_total_sysclk;
	int temp;

	/* top module video check */

	/* vactive */
	temp = lt9611_read_video_check(lt9611, 0x8282);
	if (temp < 0)
		goto end;
	vactive = temp;

	/* v_total */
	temp = lt9611_read_video_check(lt9611, 0x826c);
	if (temp < 0)
		goto end;
	v_total = temp;

	/* h_total_sysclk */
	temp = lt9611_read_video_check(lt9611, 0x8286);
	if (temp < 0)
		goto end;
	h_total_sysclk = temp;

	/* hactive_a */
	temp = lt9611_read_video_check(lt9611, 0x8382);
	if (temp < 0)
		goto end;
	hactive_a = temp / 3;

	/* hactive_b */
	temp = lt9611_read_video_check(lt9611, 0x8386);
	if (temp < 0)
		goto end;
	hactive_b = temp / 3;

	dev_info(lt9611->dev,
		 "video check: hactive_a=%d, hactive_b=%d, vactive=%d, v_total=%d, h_total_sysclk=%d\n",
		 hactive_a, hactive_b, vactive, v_total, h_total_sysclk);

	return 0;

end:
	dev_err(lt9611->dev, "read video check error\n");
	return temp;
}

static void lt9611_hdmi_tx_digital(struct lt9611 *lt9611)
{
	int AR = AR_16_9;
	int pb0,pb2,pb4;
	int infoFrame_en,VIC;

	if(lt9611_pattern_enabled) return;
/*
	if(lt9611->vic==0)
		if(lt9611->is4k)
			lt9611->vic = 95;
		else
			lt9611->vic = 16;
*/
	VIC=lt9611->vic;
	infoFrame_en = (AIF_PKT_EN|AVI_PKT_EN|SPD_PKT_EN);
	pb2 =  (AR<<4) + 0x08;
	pb4 =  VIC;
	pb0 = ((pb2 + pb4) <= 0x5f)?(0x5f - pb2 - pb4):(0x15f - pb2 - pb4);

	regmap_write(lt9611->regmap, 0x82d6, 0x8e);//sync polarity
	regmap_write(lt9611->regmap, 0x82d7, 0x04);

	//AVI
	regmap_write(lt9611->regmap, 0x8443, pb0); 
	regmap_write(lt9611->regmap, 0x8445, pb2);
	regmap_write(lt9611->regmap, 0x8447, pb4);
	regmap_write(lt9611->regmap, 0x8410, 0x02);//data iland
	regmap_write(lt9611->regmap, 0x8412, 0x64);//act_h_blank

	//VS_IF, 4k 30hz need send VS_IF packet.
	if(VIC == 95)
	{
		regmap_write(lt9611->regmap, 0x8413d, infoFrame_en|UD0_PKT_EN);//UD1 infoframe enable //revise on 20200715
		regmap_write(lt9611->regmap, 0x8474, 0x81);//HB0
		regmap_write(lt9611->regmap, 0x8475, 0x01);//HB1
		regmap_write(lt9611->regmap, 0x8476, 0x05);//HB2
		regmap_write(lt9611->regmap, 0x8477, 0x49);//PB0
		regmap_write(lt9611->regmap, 0x8478, 0x03);//PB1
		regmap_write(lt9611->regmap, 0x8479, 0x0c);//PB2
		regmap_write(lt9611->regmap, 0x847a, 0x00);//PB3
		regmap_write(lt9611->regmap, 0x847b, 0x20);//PB4
		regmap_write(lt9611->regmap, 0x847c, 0x01);//PB5
	}
	else
	{
		regmap_write(lt9611->regmap, 0x843d, infoFrame_en);//UD1 infoframe enable
	}

	if(infoFrame_en&&SPD_PKT_EN)
	{
		regmap_write(lt9611->regmap, 0x84c0, 0x00);//HB0
		regmap_write(lt9611->regmap, 0x84c1, 0x00);//HB1
		regmap_write(lt9611->regmap, 0x84c2, 0x00);//HB2

		regmap_write(lt9611->regmap, 0x84c3, 0x00);//PB0
		regmap_write(lt9611->regmap, 0x84c4, 0x00);//PB1
		regmap_write(lt9611->regmap, 0x84c5, 0x00);//PB2
		regmap_write(lt9611->regmap, 0x84c6, 0x00);//PB3
		regmap_write(lt9611->regmap, 0x84c7, 0x00);//PB4
		regmap_write(lt9611->regmap, 0x84c8, 0x00);//PB5
	}
}

static void lt9611_hdmi_tx_phy(struct lt9611 *lt9611)
{
	struct reg_sequence reg_cfg[] = {
		{ 0x8130, 0x6a },
		{ 0x8131, 0x73 }, /* HDMI AC mode */
		{ 0x8132, 0x4a },
		{ 0x8133, 0x0b },
		{ 0x8134, 0x09 },
		{ 0x8135, 0x09 },
		{ 0x8136, 0x09 },
		{ 0x8137, 0x44 },
		{ 0x813f, 0x0f },
		{ 0x8140, 0x70 },//clk swing
		{ 0x8141, 0x70 },//D0 swing
		{ 0x8142, 0x70 },//D1 swing
		{ 0x8143, 0x70 },//D2 swing
		{ 0x8144, 0x0a },
	};

	struct reg_sequence reg_cfg_4k[] = {
		{ 0x8130, 0x6a },
		{ 0x8131, 0x73 }, /* HDMI AC mode */
		{ 0x8132, 0x4a },
		{ 0x8133, 0x0b },
		{ 0x8134, 0x1c },
		{ 0x8135, 0x1c },
		{ 0x8136, 0x1c },
		{ 0x8137, 0x44 },
		{ 0x813f, 0x0f },
		{ 0x8140, 0x95 },
		{ 0x8141, 0xb0 },
		{ 0x8142, 0xb0 },
		{ 0x8143, 0xb0 },
		{ 0x8144, 0x0a },
	};

	if(lt9611_pattern_enabled) return ;
	
	if(lt9611->is4k)
		regmap_multi_reg_write(lt9611->regmap, reg_cfg_4k, ARRAY_SIZE(reg_cfg_4k));
	else
		regmap_multi_reg_write(lt9611->regmap, reg_cfg, ARRAY_SIZE(reg_cfg));
}

static void lt9611_lowpower_mode(struct lt9611 *lt9611, bool on)
{
	 if(on)
	 {
		regmap_write(lt9611->regmap, 0x8102, 0x49);
		regmap_write(lt9611->regmap, 0x8123, 0x80);
		regmap_write(lt9611->regmap, 0x8130, 0x00); //0x00 --> 0xc0, tx phy and clk can not power down, otherwise dc det don't work.
		regmap_write(lt9611->regmap, 0x8011, 0x0a);

	 }
	 else
	 {
		regmap_write(lt9611->regmap, 0x8102, 0x12);
		regmap_write(lt9611->regmap, 0x8123, 0x40);
		regmap_write(lt9611->regmap, 0x8130, 0x0a);
		regmap_write(lt9611->regmap, 0x8011, 0xfa);
	 }
}

static irqreturn_t lt9611_irq_thread_handler(int irq, void *dev_id)
{
	struct lt9611 *lt9611 = dev_id;
	unsigned int irq_flag0 = 0;
	unsigned int irq_flag3 = 0;

	regmap_read(lt9611->regmap, 0x820f, &irq_flag3);
	regmap_read(lt9611->regmap, 0x820c, &irq_flag0);

	if(lt9611_pattern_enabled) return IRQ_HANDLED;

	/* hpd changed low */
	if (irq_flag3 & 0x80) {
		dev_info(lt9611->dev, "hdmi cable disconnected\n");
#if PREFER_MODE
		lt9611->prefer_mode = -1;
#endif
		regmap_write(lt9611->regmap, 0x8207, 0xbf);
		regmap_write(lt9611->regmap, 0x8207, 0x3f);
		lt9611_lowpower_mode(lt9611,true);
	}

	/* hpd changed high */
	if (irq_flag3 & 0x40) {
		dev_info(lt9611->dev, "hdmi cable connected\n");
		lt9611_lowpower_mode(lt9611,false);

		regmap_write(lt9611->regmap, 0x8207, 0x7f);
		regmap_write(lt9611->regmap, 0x8207, 0x3f);
		msleep(200);
	}

	if (irq_flag3 & 0xc0 && lt9611->bridge.dev)
		drm_kms_helper_hotplug_event(lt9611->bridge.dev);

	/* video input changed */
	if (irq_flag0 & 0x01) {
		dev_info(lt9611->dev, "video input changed\n");
		regmap_write(lt9611->regmap, 0x829e, 0xff);
		regmap_write(lt9611->regmap, 0x829e, 0xf7);
		regmap_write(lt9611->regmap, 0x8204, 0xff);
		regmap_write(lt9611->regmap, 0x8204, 0xfe);
	}

	return IRQ_HANDLED;
}

static void lt9611_enable_hpd_interrupts(struct lt9611 *lt9611)
{
	unsigned int val;

	if(lt9611_pattern_enabled) return ;

	regmap_read(lt9611->regmap, 0x8203, &val);

	val &= ~0xc0;
	regmap_write(lt9611->regmap, 0x8203, val);
	regmap_write(lt9611->regmap, 0x8207, 0xff); /* clear */
	regmap_write(lt9611->regmap, 0x8207, 0x3f);
}

static void lt9611_sleep_setup(struct lt9611 *lt9611)
{
	const struct reg_sequence sleep_setup[] = {
		{ 0x8024, 0x76 },
		{ 0x8023, 0x01 },
		{ 0x8157, 0x03 }, /* set addr pin as output */
		{ 0x8149, 0x0b },
		{ 0x8151, 0x30 }, /* disable IRQ */
		{ 0x8102, 0x48 }, /* MIPI Rx power down */
		{ 0x8123, 0x80 },
		{ 0x8130, 0x00 },
		{ 0x8100, 0x01 }, /* bandgap power down */
		{ 0x8101, 0x00 }, /* system clk power down */
	};

	if(lt9611_pattern_enabled) return ;

	regmap_multi_reg_write(lt9611->regmap,
			       sleep_setup, ARRAY_SIZE(sleep_setup));
	lt9611->sleep = true;
}

static int lt9611_power_on(struct lt9611 *lt9611)
{
	int ret;
	const struct reg_sequence seq[] = {
		/* LT9611_System_Init */
		{ 0x8101, 0x18 }, /* sel xtal clock */

		/* timer for frequency meter */
		{ 0x821b, 0x69 }, /* timer 2 */
		{ 0x821c, 0x78 },
		{ 0x82cb, 0x69 }, /* timer 1 */
		{ 0x82cc, 0x78 },

		/* irq init */
		{ 0x8251, 0x01 },
		{ 0x8258, 0x0a }, /* hpd irq */
		{ 0x8259, 0x80 }, /* hpd debounce width */
		{ 0x829e, 0xf7 }, /* video check irq */

		/* power consumption for work */
		{ 0x8004, 0xf0 },
		{ 0x8006, 0xf0 },
		{ 0x800a, 0x80 },
		{ 0x800b, 0x46 },
		{ 0x800d, 0xef },
		{ 0x8011, 0xfa },
	};

	if(lt9611_pattern_enabled) return 0;

	if (lt9611->power_on)
		return 0;

	ret = regmap_multi_reg_write(lt9611->regmap, seq, ARRAY_SIZE(seq));
	if (!ret)
		lt9611->power_on = true;

	return ret;
}

static int lt9611_power_off(struct lt9611 *lt9611)
{
	int ret;

	if(lt9611_pattern_enabled) return 0;

	ret = regmap_write(lt9611->regmap, 0x8130, 0x6a);
	if (!ret)
		lt9611->power_on = false;

	return ret;
}

static void lt9611_reset(struct lt9611 *lt9611)
{
	gpiod_set_value_cansleep(lt9611->reset_gpio, 0);
	msleep(20);

	gpiod_set_value_cansleep(lt9611->reset_gpio, 1);
	msleep(20);

	gpiod_set_value_cansleep(lt9611->reset_gpio, 0);
	msleep(100);
}

static void lt9611_assert_5v(struct lt9611 *lt9611)
{
	if (!lt9611->enable_gpio)
		return;

	gpiod_set_value_cansleep(lt9611->enable_gpio, 1);
	msleep(20);
}

static struct lt9611_mode *lt9611_find_mode(const struct drm_display_mode *mode
#if PREFER_MODE
, int prefer_mode
#endif
)
{
	int i;

printk("lt9611_find_mode hdisplay:%d vdisplay:%d vrefresh:%d\n",mode->hdisplay,mode->vdisplay,drm_mode_vrefresh(mode));
	for (i = 0; i < ARRAY_SIZE(lt9611_modes); i++) {
		if (lt9611_modes[i].hdisplay == mode->hdisplay &&
		    lt9611_modes[i].vdisplay == mode->vdisplay &&
		    lt9611_modes[i].vrefresh == drm_mode_vrefresh(mode)) {
#if PREFER_MODE
			if((prefer_mode!=-1)&&(prefer_mode!=i))
				return NULL;
#endif
			return &lt9611_modes[i];
		}
	}

	return NULL;
}

/* connector funcs */
static enum drm_connector_status
lt9611_connector_detect(struct drm_connector *connector, bool force)
{
	struct lt9611 *lt9611 = connector_to_lt9611(connector);
	unsigned int reg_val = 0;
	int connected = 0;

	regmap_read(lt9611->regmap, 0x825e, &reg_val);
	connected  = (reg_val & BIT(2));

	lt9611->status = connected ?  connector_status_connected :
				connector_status_disconnected;

	return lt9611->status;
}

static int lt9611_read_edid(struct lt9611 *lt9611)
{
	unsigned int temp;
	int ret = 0;
	int i, j;

	if(lt9611_pattern_enabled) return 0;

	msleep(500);//wait for clk_disable_unused finish

	/* memset to clear old buffer, if any */
	memset(lt9611->edid_buf, 0, sizeof(lt9611->edid_buf));

	regmap_write(lt9611->regmap, 0x8503, 0xc9);

	/* 0xA0 is EDID device address */
	regmap_write(lt9611->regmap, 0x8504, 0xa0);
	/* 0x00 is EDID offset address */
	regmap_write(lt9611->regmap, 0x8505, 0x00);

	/* length for read */
	regmap_write(lt9611->regmap, 0x8506, EDID_LEN);
	regmap_write(lt9611->regmap, 0x8514, 0x7f);

	for (i = 0; i < EDID_LOOP; i++) {
		/* offset address */
		regmap_write(lt9611->regmap, 0x8505, i * EDID_LEN);
		regmap_write(lt9611->regmap, 0x8507, 0x36);
		regmap_write(lt9611->regmap, 0x8507, 0x31);
		regmap_write(lt9611->regmap, 0x8507, 0x37);
		usleep_range(5000, 10000);

		regmap_read(lt9611->regmap, 0x8540, &temp);

		if (temp & KEY_DDC_ACCS_DONE) {
			for (j = 0; j < EDID_LEN; j++) {
				regmap_read(lt9611->regmap, 0x8583, &temp);
				lt9611->edid_buf[i * EDID_LEN + j] = temp;
			}

		} else if (temp & DDC_NO_ACK) { /* DDC No Ack or Abitration lost */
			dev_err(lt9611->dev, "read edid failed: no ack\n");
			ret = -EIO;
			goto end;

		} else {
			dev_err(lt9611->dev, "read edid failed: access not done\n");
			ret = -EIO;
			goto end;
		}
	}

end:
	regmap_write(lt9611->regmap, 0x8507, 0x1f);
	return ret;
}
#if PREFER_MODE
static void lt9611_prefer_mode(struct lt9611 *lt9611)
{
	int i;
	{
		for(i=0;i<sizeof(lt9611->edid_buf);i+=16)
		{
			printk("0x%02x : %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x",
				i, *(lt9611->edid_buf+i+0) , *(lt9611->edid_buf+i+1) , *(lt9611->edid_buf+i+2) , *(lt9611->edid_buf+i+3)
				, *(lt9611->edid_buf+i+4) , *(lt9611->edid_buf+i+5) , *(lt9611->edid_buf+i+6) , *(lt9611->edid_buf+i+7)
				, *(lt9611->edid_buf+i+8) , *(lt9611->edid_buf+i+9) , *(lt9611->edid_buf+i+10) , *(lt9611->edid_buf+i+11)
				, *(lt9611->edid_buf+i+12) , *(lt9611->edid_buf+i+13) , *(lt9611->edid_buf+i+14) , *(lt9611->edid_buf+i+15)
				);
		}
	}
	if((*(lt9611->edid_buf+0x36)!=0)&&(*(lt9611->edid_buf+0x37)!=0))
	{
		int h_active = (((*(lt9611->edid_buf+0x3a))&0xf0)<<4)|(*(lt9611->edid_buf+0x38));

		printk("lt9611_prefer_mode h_active:%d\n",h_active);

		for (i = 0; i < ARRAY_SIZE(lt9611_modes); i++) {
			if (lt9611_modes[i].hdisplay == h_active)
			{
				lt9611->prefer_mode = i;
				break;
			}
		}
	}
}
#endif
static int
lt9611_get_edid_block(void *data, u8 *buf, unsigned int block, size_t len)
{
	struct lt9611 *lt9611 = data;
	int ret;

	if (len > 128)
		return -EINVAL;

	/* supports up to 1 extension block */
	/* TODO: add support for more extension blocks */
	if (block > 1)
		return -EINVAL;

	if (block == 0) {
		ret = lt9611_read_edid(lt9611);
		if (ret) {
			dev_err(lt9611->dev, "edid read failed\n");
			return ret;
		}
	}

	block %= 2;
#if PREFER_MODE
	if(lt9611->prefer_mode == -1)
		lt9611_prefer_mode(lt9611);
#endif
	memcpy(buf, lt9611->edid_buf + (block * 128), len);

	return 0;
}

static int lt9611_connector_get_modes(struct drm_connector *connector)
{
	struct lt9611 *lt9611 = connector_to_lt9611(connector);
	unsigned int count;
	struct edid *edid;

	lt9611_power_on(lt9611);
	edid = drm_do_get_edid(connector, lt9611_get_edid_block, lt9611);
	drm_connector_update_edid_property(connector, edid);
	count = drm_add_edid_modes(connector, edid);
	kfree(edid);

	return count;
}

static enum drm_mode_status
lt9611_connector_mode_valid(struct drm_connector *connector,
			    struct drm_display_mode *mode)
{
	struct lt9611_mode *lt9611_mode = lt9611_find_mode(mode
#if PREFER_MODE
	,-1
#endif
	);

	return lt9611_mode ? MODE_OK : MODE_BAD;
}

/* bridge funcs */
static void lt9611_bridge_enable(struct drm_bridge *bridge)
{
	struct lt9611 *lt9611 = bridge_to_lt9611(bridge);

	if(lt9611_pattern_enabled) return ;

	if (lt9611_power_on(lt9611)) {
		dev_err(lt9611->dev, "power on failed\n");
		return;
	}

	lt9611_mipi_input_analog(lt9611);
	lt9611_mipi_input_digital(lt9611);
	//lt9611_mipi_input_digital(lt9611);
	lt9611_hdmi_tx_digital(lt9611);
	lt9611_hdmi_tx_phy(lt9611);

	//msleep(500);

	lt9611_video_check(lt9611);

	/* Enable HDMI output */
	regmap_write(lt9611->regmap, 0x8130, 0xff);
}

static void lt9611_bridge_disable(struct drm_bridge *bridge)
{
	struct lt9611 *lt9611 = bridge_to_lt9611(bridge);
	int ret;

	if(lt9611_pattern_enabled) return ;

	/* Disable HDMI output */
	ret = regmap_write(lt9611->regmap, 0x8130, 0x6a);
	if (ret) {
		dev_err(lt9611->dev, "video on failed\n");
		return;
	}

	if (lt9611_power_off(lt9611)) {
		dev_err(lt9611->dev, "power on failed\n");
		return;
	}
}

static struct
drm_connector_helper_funcs lt9611_bridge_connector_helper_funcs = {
	.get_modes = lt9611_connector_get_modes,
	.mode_valid = lt9611_connector_mode_valid,
};

static const struct drm_connector_funcs lt9611_bridge_connector_funcs = {
	.fill_modes = drm_helper_probe_single_connector_modes,
	.detect = lt9611_connector_detect,
	.destroy = drm_connector_cleanup,
	.reset = drm_atomic_helper_connector_reset,
	.atomic_duplicate_state = drm_atomic_helper_connector_duplicate_state,
	.atomic_destroy_state = drm_atomic_helper_connector_destroy_state,
};

static struct mipi_dsi_device *lt9611_attach_dsi(struct lt9611 *lt9611,
						 struct device_node *dsi_node)
{
	const struct mipi_dsi_device_info info = { "lt9611", 0, NULL };
	struct mipi_dsi_device *dsi;
	struct mipi_dsi_host *host;
	int ret;

	host = of_find_mipi_dsi_host_by_node(dsi_node);
	if (!host) {
		dev_err(lt9611->dev, "failed to find dsi host\n");
		return ERR_PTR(-EPROBE_DEFER);
	}

	dsi = mipi_dsi_device_register_full(host, &info);
	if (IS_ERR(dsi)) {
		dev_err(lt9611->dev, "failed to create dsi device\n");
		return dsi;
	}

	dsi->lanes = 4;
	dsi->format = MIPI_DSI_FMT_RGB888;
	dsi->mode_flags = MIPI_DSI_MODE_VIDEO | MIPI_DSI_MODE_VIDEO_SYNC_PULSE |
			  MIPI_DSI_MODE_VIDEO_HSE;

	ret = mipi_dsi_attach(dsi);
	if (ret < 0) {
		dev_err(lt9611->dev, "failed to attach dsi to host\n");
		mipi_dsi_device_unregister(dsi);
		return ERR_PTR(ret);
	}

	return dsi;
}

static void lt9611_bridge_detach(struct drm_bridge *bridge)
{
	struct lt9611 *lt9611 = bridge_to_lt9611(bridge);

	if (lt9611->dsi1) {
		mipi_dsi_detach(lt9611->dsi1);
		mipi_dsi_device_unregister(lt9611->dsi1);
	}

	mipi_dsi_detach(lt9611->dsi0);
	mipi_dsi_device_unregister(lt9611->dsi0);
}

static int lt9611_connector_init(struct drm_bridge *bridge, struct lt9611 *lt9611)
{
	int ret;

	ret = drm_connector_init(bridge->dev, &lt9611->connector,
				 &lt9611_bridge_connector_funcs,
				 DRM_MODE_CONNECTOR_HDMIA);
	if (ret) {
		DRM_ERROR("Failed to initialize connector with drm\n");
		return ret;
	}

	drm_connector_helper_add(&lt9611->connector,
				 &lt9611_bridge_connector_helper_funcs);
	drm_connector_attach_encoder(&lt9611->connector, bridge->encoder);

	if (!bridge->encoder) {
		DRM_ERROR("Parent encoder object not found");
		return -ENODEV;
	}

	return 0;
}

static int lt9611_bridge_attach(struct drm_bridge *bridge,
				enum drm_bridge_attach_flags flags)
{
	struct lt9611 *lt9611 = bridge_to_lt9611(bridge);
	int ret;

	if (!(flags & DRM_BRIDGE_ATTACH_NO_CONNECTOR)) {
		ret = lt9611_connector_init(bridge, lt9611);
		if (ret < 0)
			return ret;
	}

	/* Attach primary DSI */
	lt9611->dsi0 = lt9611_attach_dsi(lt9611, lt9611->dsi0_node);
	if (IS_ERR(lt9611->dsi0))
		return PTR_ERR(lt9611->dsi0);

	/* Attach secondary DSI, if specified */
	if (lt9611->dsi1_node) {
		lt9611->dsi1 = lt9611_attach_dsi(lt9611, lt9611->dsi1_node);
		if (IS_ERR(lt9611->dsi1)) {
			ret = PTR_ERR(lt9611->dsi1);
			goto err_unregister_dsi0;
		}
	}

	return 0;

err_unregister_dsi0:
	lt9611_bridge_detach(bridge);
	drm_connector_cleanup(&lt9611->connector);
	mipi_dsi_device_unregister(lt9611->dsi0);

	return ret;
}

static enum drm_mode_status lt9611_bridge_mode_valid(struct drm_bridge *bridge,
						     const struct drm_display_info *info,
						     const struct drm_display_mode *mode)
{
	struct lt9611 *lt9611 = bridge_to_lt9611(bridge);
	struct lt9611_mode *lt9611_mode = lt9611_find_mode(mode
#if PREFER_MODE
	,lt9611->prefer_mode
#endif
	);

	if (!lt9611_mode)
	{
		return MODE_BAD;
	}
	else if (lt9611_mode->intfs > 1 && !lt9611->dsi1)
	{
		return MODE_PANEL;
	}
	else
	{
		return MODE_OK;
	}
}

static void lt9611_bridge_pre_enable(struct drm_bridge *bridge)
{
	struct lt9611 *lt9611 = bridge_to_lt9611(bridge);

	if(lt9611_pattern_enabled) return ;

	if (!lt9611->sleep)
		return;

	lt9611_reset(lt9611);
	regmap_write(lt9611->regmap, 0x80ee, 0x01);

	lt9611->sleep = false;
}

static void lt9611_bridge_post_disable(struct drm_bridge *bridge)
{
	struct lt9611 *lt9611 = bridge_to_lt9611(bridge);

	lt9611_sleep_setup(lt9611);
}

static void lt9611_bridge_mode_set(struct drm_bridge *bridge,
				   const struct drm_display_mode *mode,
				   const struct drm_display_mode *adj_mode)
{
	struct lt9611 *lt9611 = bridge_to_lt9611(bridge);
	struct hdmi_avi_infoframe avi_frame;
	struct lt9611_mode *lt9611_mode = lt9611_find_mode(mode
#if PREFER_MODE
	,-1
#endif
	);
	int ret;
	int pcr_m=0;

	if(lt9611_mode)
	{
		prepare_clk(lt9611,lt9611_mode);
		lt9611->is4k = lt9611_mode->is4k;
	}
	lt9611_bridge_pre_enable(bridge);

	//lt9611_mipi_input_digital(lt9611, mode);
	lt9611_pll_setup(lt9611, mode ,&pcr_m);
	//lt9611_mipi_video_setup(lt9611, mode);
	lt9611_pcr_setup(lt9611, mode,pcr_m);

	ret = drm_hdmi_avi_infoframe_from_display_mode(&avi_frame,
						       &lt9611->connector,
						       mode);
	if (!ret)
	{
		lt9611->vic = avi_frame.video_code;
	}
	if(lt9611->vic==0)
		if(lt9611_mode)
			lt9611->vic = lt9611_mode->vic;

	printk("lt9611_bridge_mode_set hdisplay:%d vic:%d\n",mode->hdisplay,lt9611_mode->vic);
}

static enum drm_connector_status lt9611_bridge_detect(struct drm_bridge *bridge)
{
	struct lt9611 *lt9611 = bridge_to_lt9611(bridge);
	unsigned int reg_val = 0;
	int connected;

	regmap_read(lt9611->regmap, 0x825e, &reg_val);
	connected  = reg_val & BIT(2);

	lt9611->status = connected ?  connector_status_connected :
				connector_status_disconnected;

	return lt9611->status;
}

static struct edid *lt9611_bridge_get_edid(struct drm_bridge *bridge,
					   struct drm_connector *connector)
{
	struct lt9611 *lt9611 = bridge_to_lt9611(bridge);
	struct edid *edid;

	lt9611_power_on(lt9611);
	edid = drm_do_get_edid(connector, lt9611_get_edid_block, lt9611);

	cec_s_phys_addr_from_edid(lt9611->cec_adap, edid);

	return edid;
}

static void lt9611_bridge_hpd_enable(struct drm_bridge *bridge)
{
	struct lt9611 *lt9611 = bridge_to_lt9611(bridge);

	lt9611_enable_hpd_interrupts(lt9611);
}



static void lt9611_bridge_hpd_notify(struct drm_bridge *bridge,
				      enum drm_connector_status status)
{
	struct lt9611 *adv = bridge_to_lt9611(bridge);

	if (status == connector_status_disconnected)
	{
		cec_phys_addr_invalidate(adv->cec_adap);
	}
}

static const struct drm_bridge_funcs lt9611_bridge_funcs = {
	.enable = lt9611_bridge_enable,
	.disable = lt9611_bridge_disable,
	.mode_set = lt9611_bridge_mode_set,
	.mode_valid = lt9611_bridge_mode_valid,
	.attach = lt9611_bridge_attach,
	.detect = lt9611_bridge_detect,
	.get_edid = lt9611_bridge_get_edid,
	.detach = lt9611_bridge_detach,
	//.hpd_notify = lt9611_bridge_hpd_notify,
};

static int lt9611_parse_dt(struct device *dev,
			   struct lt9611 *lt9611)
{
	lt9611->dsi0_node = of_graph_get_remote_node(dev->of_node, 0, -1);
	if (!lt9611->dsi0_node) {
		dev_err(lt9611->dev, "failed to get remote node for primary dsi\n");
		return -ENODEV;
	}

	lt9611->dsi1_node = of_graph_get_remote_node(dev->of_node, 1, -1);

	lt9611->ac_mode = of_property_read_bool(dev->of_node, "lt,ac-mode");

	return 0;
}

static int lt9611_gpio_init(struct lt9611 *lt9611)
{
	struct device *dev = lt9611->dev;

	lt9611->reset_gpio = devm_gpiod_get(dev, "reset", GPIOD_OUT_HIGH);
	if (IS_ERR(lt9611->reset_gpio)) {
		dev_err(dev, "failed to acquire reset gpio\n");
		return PTR_ERR(lt9611->reset_gpio);
	}
	return 0;
}

static int lt9611_read_device_rev(struct lt9611 *lt9611)
{
	unsigned int rev;
	int ret;

	regmap_write(lt9611->regmap, 0x80ee, 0x01);
	ret = regmap_read(lt9611->regmap, 0x8002, &rev);
	if (ret)
		dev_err(lt9611->dev, "failed to read revision: %d\n", ret);
	else
		dev_info(lt9611->dev, "LT9611 revision: 0x%x\n", rev);

	return ret;
}

static int lt9611_hdmi_hw_params(struct device *dev, void *data,
				 struct hdmi_codec_daifmt *fmt,
				 struct hdmi_codec_params *hparms)
{
	struct lt9611 *lt9611 = data;

	if(lt9611_pattern_enabled) return 0 ;

	if (hparms->sample_rate == 48000)
		regmap_write(lt9611->regmap, 0x840f, 0x2b);
	else if (hparms->sample_rate == 96000)
		regmap_write(lt9611->regmap, 0x840f, 0xab);
	else
		return -EINVAL;

	regmap_write(lt9611->regmap, 0x8435, 0x00);
	regmap_write(lt9611->regmap, 0x8436, 0x18);
	regmap_write(lt9611->regmap, 0x8437, 0x00);

	return 0;
}

static int lt9611_audio_startup(struct device *dev, void *data)
{
	struct lt9611 *lt9611 = data;

	if(lt9611_pattern_enabled) return 0;

	regmap_write(lt9611->regmap, 0x82d6, 0x8c);
	regmap_write(lt9611->regmap, 0x82d7, 0x04);

	regmap_write(lt9611->regmap, 0x8406, 0x08);
	regmap_write(lt9611->regmap, 0x8407, 0x10);

	regmap_write(lt9611->regmap, 0x8434, 0xd5);

	return 0;
}

static void lt9611_audio_shutdown(struct device *dev, void *data)
{
	struct lt9611 *lt9611 = data;

	if(lt9611_pattern_enabled) return ;

	regmap_write(lt9611->regmap, 0x8406, 0x00);
	regmap_write(lt9611->regmap, 0x8407, 0x00);
}

static int lt9611_hdmi_i2s_get_dai_id(struct snd_soc_component *component,
				      struct device_node *endpoint)
{
	struct of_endpoint of_ep;
	int ret;

	ret = of_graph_parse_endpoint(endpoint, &of_ep);
	if (ret < 0)
		return ret;

	/*
	 * HDMI sound should be located as reg = <2>
	 * Then, it is sound port 0
	 */
	if (of_ep.port == 2)
		return 0;

	return -EINVAL;
}

static const struct hdmi_codec_ops lt9611_codec_ops = {
	.hw_params	= lt9611_hdmi_hw_params,
	.audio_shutdown = lt9611_audio_shutdown,
	.audio_startup	= lt9611_audio_startup,
	.get_dai_id	= lt9611_hdmi_i2s_get_dai_id,
};

static struct hdmi_codec_pdata codec_data = {
	.ops = &lt9611_codec_ops,
	.max_i2s_channels = 8,
	.i2s = 1,
};

static int lt9611_audio_init(struct device *dev, struct lt9611 *lt9611)
{
	codec_data.data = lt9611;
	lt9611->audio_pdev =
		platform_device_register_data(dev, HDMI_CODEC_DRV_NAME,
					      PLATFORM_DEVID_AUTO,
					      &codec_data, sizeof(codec_data));

	return PTR_ERR_OR_ZERO(lt9611->audio_pdev);
}

static void lt9611_audio_exit(struct lt9611 *lt9611)
{
	if (lt9611->audio_pdev) {
		platform_device_unregister(lt9611->audio_pdev);
		lt9611->audio_pdev = NULL;
	}
}

static int lt9611_probe(struct i2c_client *client)
{
	struct lt9611 *lt9611;
	struct device *dev = &client->dev;
	int ret;

	if (!i2c_check_functionality(client->adapter, I2C_FUNC_I2C)) {
		dev_err(dev, "device doesn't support I2C\n");
		return -ENODEV;
	}

	lt9611 = devm_kzalloc(dev, sizeof(*lt9611), GFP_KERNEL);
	if (!lt9611)
		return -ENOMEM;

	lt9611->dev = &client->dev;
	lt9611->client = client;
	lt9611->sleep = false;

	lt9611->regmap = devm_regmap_init_i2c(client, &lt9611_regmap_config);
	if (IS_ERR(lt9611->regmap)) {
		dev_err(lt9611->dev, "regmap i2c init failed\n");
		return PTR_ERR(lt9611->regmap);
	}

	ret = lt9611_parse_dt(&client->dev, lt9611);
	if (ret) {
		dev_err(dev, "failed to parse device tree\n");
		return ret;
	}

	ret = lt9611_gpio_init(lt9611);
	if (ret < 0)
		goto err_of_put;

	lt9611_reset(lt9611);


	ret = lt9611_read_device_rev(lt9611);
	if (ret) {
		dev_err(dev, "failed to read chip rev\n");
		goto err_disable_regulators;
	}

	if(lt9611_pattern_enabled)
		LT9611_test_pattern(lt9611);

	lt9611->vco = devm_clk_get(dev, "vco");
	if (IS_ERR(lt9611->vco)) {
            lt9611->vco = NULL;
            dev_err(dev, "vco missing or invalid\n");
    }

	lt9611->videopll1 = devm_clk_get(dev, "videopll1");
	if (IS_ERR(lt9611->videopll1)) {
            lt9611->videopll1 = NULL;
            dev_err(dev, "videopll1 missing or invalid\n");
    }

	lt9611->pix = devm_clk_get(dev, "pix");
	if (IS_ERR(lt9611->pix)) {
            lt9611->pix = NULL;
            dev_err(dev, "pix missing or invalid\n");
    }
#if PREFER_MODE
	lt9611->prefer_mode = -1;
#endif
	ret = devm_request_threaded_irq(dev, client->irq, NULL,
					lt9611_irq_thread_handler,
					IRQF_ONESHOT, "lt9611_irq", lt9611);
	if (ret) {
		dev_err(dev, "failed to request irq\n");
		goto err_disable_regulators;
	}

	i2c_set_clientdata(client, lt9611);

	lt9611->bridge.funcs = &lt9611_bridge_funcs;
	lt9611->bridge.of_node = client->dev.of_node;
	lt9611->bridge.ops = DRM_BRIDGE_OP_DETECT | DRM_BRIDGE_OP_EDID |
			     DRM_BRIDGE_OP_HPD | DRM_BRIDGE_OP_MODES;
	lt9611->bridge.type = DRM_MODE_CONNECTOR_HDMIA;

	drm_bridge_add(&lt9611->bridge);

	lt9611_enable_hpd_interrupts(lt9611);

	return lt9611_audio_init(dev, lt9611);

err_disable_regulators:

err_of_put:
	of_node_put(lt9611->dsi1_node);
	of_node_put(lt9611->dsi0_node);

	return ret;
}
static void lt9611_remove(struct i2c_client *client)
{
	struct lt9611 *lt9611 = i2c_get_clientdata(client);

	disable_irq(client->irq);
	lt9611_audio_exit(lt9611);
	drm_bridge_remove(&lt9611->bridge);
	
	//cec_unregister_adapter(lt9611->cec_adap);

	of_node_put(lt9611->dsi1_node);
	of_node_put(lt9611->dsi0_node);
}

static int __init lt9611_test_pattern_enable(char *str)
{
	if (!strcmp(str, "4k")) {
		pr_info("Enable the lt9611 test pattern 4k mode.\n");
		lt9611_pattern_enabled = 1;
	}
	if (!strcmp(str, "1080p")) {
		pr_info("Enable the lt9611 test pattern 1080p mode.\n");
		lt9611_pattern_enabled = 2;
	}
	if (!strcmp(str, "4096")) {
		pr_info("Enable the lt9611 test pattern 4096x2160 mode.\n");
		lt9611_pattern_enabled = 3;
	}
	if (!strcmp(str, "1200")) {
		pr_info("Enable the lt9611 test pattern 1920x1200 mode.\n");
		lt9611_pattern_enabled = 4;
	}
	return 1;
}

void lt9611_shutdown(struct i2c_client *client)
{
	struct lt9611 *lt9611 = i2c_get_clientdata(client);

	gpiod_set_value_cansleep(lt9611->reset_gpio, 1);
}

__setup("lt9611_pattern_enabled=", lt9611_test_pattern_enable);

static struct i2c_device_id lt9611_id[] = {
	{ "adlink,lt9611", 0 },
	{}
};
MODULE_DEVICE_TABLE(i2c, lt9611_id);

static const struct of_device_id lt9611_match_table[] = {
	{ .compatible = "adlink,lt9611" },
	{ }
};
MODULE_DEVICE_TABLE(of, lt9611_match_table);

static struct i2c_driver lt9611_driver = {
	.driver = {
		.name = "adlink-lt9611",
		.of_match_table = lt9611_match_table,
	},
	.probe = lt9611_probe,
	.remove = lt9611_remove,
	.shutdown = lt9611_shutdown,
	.id_table = lt9611_id,
};
module_i2c_driver(lt9611_driver);

MODULE_LICENSE("GPL v2");
