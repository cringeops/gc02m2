// SPDX-License-Identifier: GPL-2.0
/*
 * gc02m2 driver
 *
 * Copyright (C) 2020 Rockchip Electronics Co., Ltd.
 *
 * V0.0X01.0X01 init version.
 * V0.0X01.0X02
 * 1.add hflip/vflip function.
 * 2.modify set_gain_reg function.
 */

//#define DEBUG 1
#include <linux/clk.h>
#include <linux/device.h>
#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/pm_runtime.h>
#include <linux/of.h>
#include <linux/of_graph.h>
#include <linux/of_gpio.h>

#include <linux/regulator/consumer.h>
#include <linux/sysfs.h>
#include <linux/version.h>
#include <media/media-entity.h>
#include <media/v4l2-async.h>
#include <media/v4l2-ctrls.h>
#include <media/v4l2-subdev.h>
#include <linux/pinctrl/consumer.h>
#include <linux/slab.h>

#ifndef V4L2_CID_DIGITAL_GAIN
#define V4L2_CID_DIGITAL_GAIN		V4L2_CID_GAIN
#endif

#define GC02M2_MIPI_LINK_FREQ	336000000

/* pixel rate = link frequency * 1 * lanes / BITS_PER_SAMPLE */
#define GC02M2_PIXEL_RATE		(GC02M2_MIPI_LINK_FREQ * 2LL * 1LL / 10)
#define GC02M2_XVCLK_FREQ		24000000

#define CHIP_ID					0x02f0
#define GC02M2_REG_CHIP_ID_H	0xf0
#define GC02M2_REG_CHIP_ID_L	0xf1
#define SENSOR_ID(_msb, _lsb)	((_msb) << 8 | (_lsb))

#define GC02M2_PAGE_SELECT		0xfe
#define GC02M2_MODE_SELECT		0x3e
#define GC02M2_MODE_SW_STANDBY	0x00
#define GC02M2_MODE_STREAMING	0x90

#define GC02M2_REG_EXPOSURE_H	0x03
#define GC02M2_REG_EXPOSURE_L	0x04
#define	GC02M2_EXPOSURE_MIN		4
#define	GC02M2_EXPOSURE_STEP	1
#define GC02M2_VTS_MAX			0x7fff

#define GC02M2_ANALOG_GAIN_REG	0xb6
#define GC02M2_PREGAIN_H_REG	0xb1
#define GC02M2_PREGAIN_L_REG	0xb2
#define GC02M2_GAIN_MIN			0x40
#define GC02M2_GAIN_MAX			0x300
#define GC02M2_GAIN_STEP		1
#define GC02M2_GAIN_DEFAULT		0x80

#define GC02M2_REG_VTS_H		0x41
#define GC02M2_REG_VTS_L		0x42

#define GC02M2_MIRROR_FLIP_REG	0x17
#define SC200AI_FETCH_MIRROR(VAL, ENABLE)	(ENABLE ? VAL | 0x01 : VAL & 0xfe)
#define SC200AI_FETCH_FLIP(VAL, ENABLE)	(ENABLE ? VAL | 0x02 : VAL & 0xfd)

#define GC02M2_LANES			1
#define GC02M2_BITS_PER_SAMPLE	10
#define GC02M2_NAME			"gc02m2"
#define REG_NULL				0xFF

static const char * const gc02m2_supply_names[] = {
       "dovdd",        /* Digital I/O power */
       "avdd",         /* Analog power */
       "dvdd",         /* Digital core power */
};

#define GC02M2_NUM_SUPPLIES ARRAY_SIZE(gc02m2_supply_names)

#define to_gc02m2(sd) container_of(sd, struct gc02m2, subdev)

enum gc02m2_max_pad {
	PAD0,
	PAD_MAX,
};

struct regval {
	u8 addr;
	u8 val;
};

struct gc02m2_mode {
	u32 bus_fmt;
	u32 width;
	u32 height;
	struct v4l2_fract max_fps;
	u32 hts_def;
	u32 vts_def;
	u32 exp_def;
	const struct regval *reg_list;
};

struct gc02m2 {
	struct i2c_client	*client;
	struct clk		*xvclk;
	struct gpio_desc	*reset_gpio;
	struct gpio_desc	*pwdn_gpio;
	struct regulator_bulk_data supplies[GC02M2_NUM_SUPPLIES];
	struct pinctrl		*pinctrl;
	struct pinctrl_state	*pins_default;
	struct pinctrl_state	*pins_sleep;
	struct v4l2_subdev	subdev;
	struct media_pad	pad;
	struct v4l2_ctrl_handler ctrl_handler;
	struct v4l2_ctrl	*exposure;
	struct v4l2_ctrl	*anal_gain;
	struct v4l2_ctrl	*digi_gain;
	struct v4l2_ctrl	*hblank;
	struct v4l2_ctrl	*vblank;
	struct v4l2_ctrl	*test_pattern;
	struct mutex		mutex;
	bool			streaming;
	bool			power_on;
	const struct gc02m2_mode *cur_mode;
	unsigned int	lane_num;
	unsigned int	pixel_rate;
};

/*
 * Xclk 24Mhz
 */
static const struct regval gc02m2_global_regs[] = {
	/*system*/
	{0xfc, 0x01},
	{0xf4, 0x41},
	{0xf5, 0xe3},
	{0xf6, 0x44},
	{0xf8, 0x38},
	{0xf9, 0x82},
	{0xfa, 0x00},
	{0xfd, 0x80},
	{0xfc, 0x81},
	{0xfe, 0x03},
	{0x01, 0x0b},
	{0xf7, 0x01},
	{0xfc, 0x80},
	{0xfc, 0x80},
	{0xfc, 0x80},
	{0xfc, 0x8e},
	/*CISCTL*/
	{0xfe, 0x00},
	{0x87, 0x09},
	{0xee, 0x72},
	{0xfe, 0x01},
	{0x8c, 0x90},
	{0xfe, 0x00},
	{0x90, 0x00},
	{0x03, 0x04},
	{0x04, 0x7d},
	{0x41, 0x04},
	{0x42, 0xf4},
	{0x05, 0x04},
	{0x06, 0x48},
	{0x07, 0x00},
	{0x08, 0x18},
	{0x9d, 0x18},
	{0x09, 0x00},
	{0x0a, 0x02},
	{0x0d, 0x04},
	{0x0e, 0xbc},
	{0x17, 0x80},
	{0x19, 0x04},
	{0x24, 0x00},
	{0x56, 0x20},
	{0x5b, 0x00},
	{0x5e, 0x01},
	/*analog Register width*/
	{0x21, 0x3c},
	{0x44, 0x20},
	{0xcc, 0x01},
	/*analog mode*/
	{0x1a, 0x04},
	{0x1f, 0x11},
	{0x27, 0x30},
	{0x2b, 0x00},
	{0x33, 0x00},
	{0x53, 0x90},
	{0xe6, 0x50},
	/*analog voltage*/
	{0x39, 0x07},
	{0x43, 0x04},
	{0x46, 0x4a},
	{0x7c, 0xa0},
	{0xd0, 0xbe},
	{0xd1, 0x40},
	{0xd2, 0x40},
	{0xd3, 0xb3},
	{0xde, 0x1c},
	/*analog current*/
	{0xcd, 0x06},
	{0xce, 0x6f},
	/*CISCTL RESET*/
	{0xfc, 0x88},
	{0xfe, 0x10},
	{0xfe, 0x00},
	{0xfc, 0x8e},
	{0xfe, 0x00},
	{0xfe, 0x00},
	{0xfe, 0x00},
	{0xfe, 0x00},
	{0xfc, 0x88},
	{0xfe, 0x10},
	{0xfe, 0x00},
	{0xfc, 0x8e},
	{0xfe, 0x04},
	{0xe0, 0x01},
	{0xfe, 0x00},
	/*ISP*/
	{0xfe, 0x01},
	{0x53, 0x54},
	{0x87, 0x53},
	{0x89, 0x03},
	/*Gain*/
	{0xfe, 0x00},
	{0xb0, 0x74},
	{0xb1, 0x04},
	{0xb2, 0x00},
	{0xb6, 0x00},
	{0xfe, 0x04},
	{0xd8, 0x00},
	{0xc0, 0x40},
	{0xc0, 0x00},
	{0xc0, 0x00},
	{0xc0, 0x00},
	{0xc0, 0x60},
	{0xc0, 0x00},
	{0xc0, 0xc0},
	{0xc0, 0x2a},
	{0xc0, 0x80},
	{0xc0, 0x00},
	{0xc0, 0x00},
	{0xc0, 0x40},
	{0xc0, 0xa0},
	{0xc0, 0x00},
	{0xc0, 0x90},
	{0xc0, 0x19},
	{0xc0, 0xc0},
	{0xc0, 0x00},
	{0xc0, 0xD0},
	{0xc0, 0x2F},
	{0xc0, 0xe0},
	{0xc0, 0x00},
	{0xc0, 0x90},
	{0xc0, 0x39},
	{0xc0, 0x00},
	{0xc0, 0x01},
	{0xc0, 0x20},
	{0xc0, 0x04},
	{0xc0, 0x20},
	{0xc0, 0x01},
	{0xc0, 0xe0},
	{0xc0, 0x0f},
	{0xc0, 0x40},
	{0xc0, 0x01},
	{0xc0, 0xe0},
	{0xc0, 0x1a},
	{0xc0, 0x60},
	{0xc0, 0x01},
	{0xc0, 0x20},
	{0xc0, 0x25},
	{0xc0, 0x80},
	{0xc0, 0x01},
	{0xc0, 0xa0},
	{0xc0, 0x2c},
	{0xc0, 0xa0},
	{0xc0, 0x01},
	{0xc0, 0xe0},
	{0xc0, 0x32},
	{0xc0, 0xc0},
	{0xc0, 0x01},
	{0xc0, 0x20},
	{0xc0, 0x38},
	{0xc0, 0xe0},
	{0xc0, 0x01},
	{0xc0, 0x60},
	{0xc0, 0x3c},
	{0xc0, 0x00},
	{0xc0, 0x02},
	{0xc0, 0xa0},
	{0xc0, 0x40},
	{0xc0, 0x80},
	{0xc0, 0x02},
	{0xc0, 0x18},
	{0xc0, 0x5c},
	{0xfe, 0x00},
	{0x9f, 0x10},
	/*BLK*/
	{0xfe, 0x00},
	{0x26, 0x20},
	{0xfe, 0x01},
	{0x40, 0x22},
	{0x46, 0x7f},
	{0x49, 0x0f},
	{0x4a, 0xf0},
	{0xfe, 0x04},
	{0x14, 0x80},
	{0x15, 0x80},
	{0x16, 0x80},
	{0x17, 0x80},
	/*anti_blooming*/
	{0xfe, 0x01},
	{0x41, 0x20},
	{0x4c, 0x00},
	{0x4d, 0x0c},
	{0x44, 0x08},
	{0x48, 0x03},
	/*Window 1280X720*/
	{0xfe, 0x01},
	{0x90, 0x01},
	{0x91, 0x00},
	{0x92, 0x06},
	{0x93, 0x00},
	{0x94, 0x06},
	{0x95, 0x02},
	{0x96, 0xd0},
	{0x97, 0x05},
	{0x98, 0x00},
	/*mipi*/
	{0xfe, 0x03},
	{0x01, 0x23},
	{0x03, 0xce},
	{0x04, 0x48},
	{0x15, 0x01},
	{0x21, 0x10},
	{0x22, 0x05},
	{0x23, 0x20},
	{0x25, 0x20},
	{0x26, 0x08},
	{0x29, 0x06},
	{0x2a, 0x0a},
	{0x2b, 0x08},
	/*out*/
	{0xfe, 0x01},
	{0x8c, 0x10},
	{REG_NULL, 0x00},
};

static const struct gc02m2_mode supported_modes[] = {
	{
		.width = 1280,
		.height = 720,
		.max_fps = {
			.numerator = 10000,
			.denominator = 300000,
		},
		.bus_fmt = MEDIA_BUS_FMT_SRGGB10_1X10,
		.exp_def = 0x0475,
		.hts_def = 0x0448 * 2,
		.vts_def = 0x04f4,
		.reg_list = gc02m2_global_regs,
	},
};

static const s64 link_freq_menu_items[] = {
	GC02M2_MIPI_LINK_FREQ
};

static int gc02m2_write_reg(struct i2c_client *client, u8 reg, u8 val)
{
	struct i2c_msg msg;
	u8 buf[2];
	int ret;

	buf[0] = reg & 0xFF;
	buf[1] = val;

	msg.addr = client->addr;
	msg.flags = client->flags;
	msg.buf = buf;
	msg.len = sizeof(buf);

	ret = i2c_transfer(client->adapter, &msg, 1);
	if (ret >= 0)
		return 0;

	dev_err(&client->dev,
		"gc02m2 write reg(0x%x val:0x%x) failed !\n", reg, val);

	return ret;
}

static int gc02m2_write_array(struct i2c_client *client,
			      const struct regval *regs)
{
	u32 i;
	int ret = 0;

	for (i = 0; ret == 0 && regs[i].addr != REG_NULL; i++) {
		ret = gc02m2_write_reg(client, regs[i].addr, regs[i].val);
	}
	return ret;
}

static int gc02m2_read_reg(struct i2c_client *client, u8 reg, u8 *val)
{
	struct i2c_msg msg[2];
	u8 buf[1];
	int ret;

	buf[0] = reg & 0xFF;

	msg[0].addr = client->addr;
	msg[0].flags = client->flags;
	msg[0].buf = buf;
	msg[0].len = sizeof(buf);

	msg[1].addr = client->addr;
	msg[1].flags = client->flags | I2C_M_RD;
	msg[1].buf = buf;
	msg[1].len = 1;

	ret = i2c_transfer(client->adapter, msg, 2);
	if (ret >= 0) {
		*val = buf[0];
		return 0;
	}

	dev_err(&client->dev, "read reg 0x%x failed with code %d\n", reg, ret);
	return ret;
}

static int gc02m2_get_reso_dist(const struct gc02m2_mode *mode,
				struct v4l2_mbus_framefmt *framefmt)
{
	return abs(mode->width - framefmt->width) +
	       abs(mode->height - framefmt->height);
}

static const struct gc02m2_mode *
gc02m2_find_best_fit(struct v4l2_subdev_format *fmt)
{
	struct v4l2_mbus_framefmt *framefmt = &fmt->format;
	int dist;
	int cur_best_fit = 0;
	int cur_best_fit_dist = -1;
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(supported_modes); i++) {
		dist = gc02m2_get_reso_dist(&supported_modes[i], framefmt);
		if (cur_best_fit_dist == -1 || dist < cur_best_fit_dist) {
			cur_best_fit_dist = dist;
			cur_best_fit = i;
		}
	}

	return &supported_modes[cur_best_fit];
}

static u32 GC02M2_AGC_Param[17][2] = {
			{ 64  ,  0 },
			{ 96  ,  1 },
			{ 127 ,  2 },
			{ 157 ,  3 },
			{ 198 ,  4 },
			{ 227 ,  5 },
			{ 259 ,  6 },
			{ 287 ,  7 },
			{ 318 ,  8 },
			{ 356 ,  9 },
			{ 392 , 10 },
			{ 420 , 11 },
			{ 451 , 12 },
			{ 480 , 13 },
			{ 513 , 14 },
			{ 646 , 15 },
			{ 0xffff , 16 },
};

static int gc02m2_set_fmt(struct v4l2_subdev *sd,
			  struct v4l2_subdev_state *sd_state,
			  struct v4l2_subdev_format *fmt)
{
	struct gc02m2 *gc02m2 = to_gc02m2(sd);
	const struct gc02m2_mode *mode;
	s64 h_blank, vblank_def;

	mutex_lock(&gc02m2->mutex);

	mode = gc02m2_find_best_fit(fmt);
	fmt->format.code = mode->bus_fmt;
	fmt->format.width = mode->width;
	fmt->format.height = mode->height;
	fmt->format.field = V4L2_FIELD_NONE;
	if (fmt->which == V4L2_SUBDEV_FORMAT_TRY) {
#ifdef CONFIG_VIDEO_V4L2_SUBDEV_API
		*v4l2_subdev_get_try_format(sd, sd_state, fmt->pad) = fmt->format;
#else
		mutex_unlock(&gc02m2->mutex);
		return -ENOTTY;
#endif
	} else {
		gc02m2->cur_mode = mode;
		h_blank = mode->hts_def - mode->width;
		__v4l2_ctrl_modify_range(gc02m2->hblank, h_blank,
					 h_blank, 1, h_blank);
		vblank_def = mode->vts_def - mode->height;
		__v4l2_ctrl_modify_range(gc02m2->vblank, vblank_def,
					 GC02M2_VTS_MAX - mode->height,
					 1, vblank_def);
	}

	mutex_unlock(&gc02m2->mutex);

	return 0;
}

static int gc02m2_get_fmt(struct v4l2_subdev *sd,
			  struct v4l2_subdev_state *sd_state,
			  struct v4l2_subdev_format *fmt)
{
	struct gc02m2 *gc02m2 = to_gc02m2(sd);
	const struct gc02m2_mode *mode = gc02m2->cur_mode;

	mutex_lock(&gc02m2->mutex);
	if (fmt->which == V4L2_SUBDEV_FORMAT_TRY) {
#ifdef CONFIG_VIDEO_V4L2_SUBDEV_API
		fmt->format = *v4l2_subdev_get_try_format(sd, sd_state, fmt->pad);
#else
		mutex_unlock(&gc02m2->mutex);
		return -ENOTTY;
#endif
	} else {
		fmt->format.width = mode->width;
		fmt->format.height = mode->height;
		fmt->format.code = mode->bus_fmt;
		fmt->format.field = V4L2_FIELD_NONE;
	}
	mutex_unlock(&gc02m2->mutex);

	return 0;
}

static int gc02m2_enum_mbus_code(struct v4l2_subdev *sd,
				 struct v4l2_subdev_state *sd_state,
				 struct v4l2_subdev_mbus_code_enum *code)
{
	struct gc02m2 *gc02m2 = to_gc02m2(sd);
	if (code->index != 0)
		return -EINVAL;

	code->code = gc02m2->cur_mode->bus_fmt;;

	return 0;
}

static int gc02m2_enum_frame_sizes(struct v4l2_subdev *sd,
				   struct v4l2_subdev_state *sd_state,
				   struct v4l2_subdev_frame_size_enum *fse)
{
	if (fse->index >= ARRAY_SIZE(supported_modes))
		return -EINVAL;

	if (fse->code != supported_modes[0].bus_fmt)
		return -EINVAL;

	fse->min_width  = supported_modes[fse->index].width;
	fse->max_width  = supported_modes[fse->index].width;
	fse->max_height = supported_modes[fse->index].height;
	fse->min_height = supported_modes[fse->index].height;

	return 0;
}

static int gc02m2_g_frame_interval(struct v4l2_subdev *sd,
				   struct v4l2_subdev_frame_interval *fi)
{
	struct gc02m2 *gc02m2 = to_gc02m2(sd);
	const struct gc02m2_mode *mode = gc02m2->cur_mode;

	mutex_lock(&gc02m2->mutex);
	fi->interval = mode->max_fps;
	mutex_unlock(&gc02m2->mutex);

	return 0;
}

/* Calculate the delay in us by clock rate and clock cycles */
static inline u32 gc02m2_cal_delay(u32 cycles)
{
	return DIV_ROUND_UP(cycles, GC02M2_XVCLK_FREQ / 1000 / 1000);
}

static int __gc02m2_power_on(struct gc02m2 *gc02m2)
{
	int ret;
	u32 delay_us;
	struct device *dev = &gc02m2->client->dev;

	if (!IS_ERR_OR_NULL(gc02m2->pins_default)) {
		ret = pinctrl_select_state(gc02m2->pinctrl,
					   gc02m2->pins_default);
		if (ret < 0)
			dev_err(dev, "could not set pins\n");
	}
	ret = clk_set_rate(gc02m2->xvclk, GC02M2_XVCLK_FREQ);
	if (ret < 0)
		dev_warn(dev, "Failed to set xvclk rate (24MHz)\n");
	if (clk_get_rate(gc02m2->xvclk) != GC02M2_XVCLK_FREQ)
		dev_warn(dev, "xvclk mismatched, modes are based on 24MHz\n");
	ret = clk_prepare_enable(gc02m2->xvclk);
	if (ret < 0) {
		dev_err(dev, "Failed to enable xvclk\n");
		return ret;
	}

	ret = regulator_bulk_enable(GC02M2_NUM_SUPPLIES, gc02m2->supplies);
	if (ret < 0) {
		dev_err(dev, "Failed to enable regulators\n");
		goto disable_clk;
	}

	if (!IS_ERR(gc02m2->reset_gpio))
		gpiod_set_value_cansleep(gc02m2->reset_gpio, 1);
	if (!IS_ERR(gc02m2->pwdn_gpio))
		gpiod_set_value_cansleep(gc02m2->pwdn_gpio, 0);

	if (!IS_ERR(gc02m2->reset_gpio))
		gpiod_set_value_cansleep(gc02m2->reset_gpio, 0);
	usleep_range(500, 1000);

	/* 8192 cycles prior to first SCCB transaction */
	delay_us = gc02m2_cal_delay(8192);
	usleep_range(delay_us, delay_us * 2);
	gc02m2->power_on = true;
	return 0;

disable_clk:
	clk_disable_unprepare(gc02m2->xvclk);

	return ret;
}

static void __gc02m2_power_off(struct gc02m2 *gc02m2)
{
	if (!IS_ERR(gc02m2->pwdn_gpio))
		gpiod_set_value_cansleep(gc02m2->pwdn_gpio, 1);
	clk_disable_unprepare(gc02m2->xvclk);
	if (!IS_ERR(gc02m2->reset_gpio))
		gpiod_set_value_cansleep(gc02m2->reset_gpio, 1);
	regulator_bulk_disable(GC02M2_NUM_SUPPLIES, gc02m2->supplies);
	gc02m2->power_on = false;
}

static int __gc02m2_start_stream(struct gc02m2 *gc02m2)
{
	int ret;

	/* In case these controls are set before streaming */
	mutex_unlock(&gc02m2->mutex);
	ret = v4l2_ctrl_handler_setup(&gc02m2->ctrl_handler);
	mutex_lock(&gc02m2->mutex);
	if (ret)
		return ret;

	ret = gc02m2_write_reg(gc02m2->client, GC02M2_PAGE_SELECT, 0x00);
	ret |= gc02m2_write_reg(gc02m2->client, GC02M2_MODE_SELECT,
				 GC02M2_MODE_STREAMING);
	ret |= gc02m2_write_reg(gc02m2->client, GC02M2_PAGE_SELECT, 0x00);

	return ret;
}

static int __gc02m2_stop_stream(struct gc02m2 *gc02m2)
{
	int ret;

	ret = gc02m2_write_reg(gc02m2->client, GC02M2_PAGE_SELECT, 0x00);
	ret |= gc02m2_write_reg(gc02m2->client, GC02M2_MODE_SELECT,
				 GC02M2_MODE_SW_STANDBY);
	ret |= gc02m2_write_reg(gc02m2->client, GC02M2_PAGE_SELECT, 0x00);

	return ret;
}

static int gc02m2_s_stream(struct v4l2_subdev *sd, int on)
{
	struct gc02m2 *gc02m2 = to_gc02m2(sd);
	struct i2c_client *client = gc02m2->client;
	int ret = 0;

	mutex_lock(&gc02m2->mutex);
	on = !!on;
	if (on == gc02m2->streaming)
		goto unlock_and_return;

	if (on) {
		ret = pm_runtime_get_sync(&client->dev);
		if (ret < 0) {
			pm_runtime_put_noidle(&client->dev);
			goto unlock_and_return;
		}

		ret = __gc02m2_start_stream(gc02m2);
		if (ret) {
			v4l2_err(sd, "start stream failed while write regs\n");
			pm_runtime_put(&client->dev);
			goto unlock_and_return;
		}
	} else {
		__gc02m2_stop_stream(gc02m2);
		pm_runtime_put(&client->dev);
	}

	gc02m2->streaming = on;

unlock_and_return:
	mutex_unlock(&gc02m2->mutex);

	return ret;
}

static int gc02m2_s_power(struct v4l2_subdev *sd, int on)
{
	struct gc02m2 *gc02m2 = to_gc02m2(sd);
	struct i2c_client *client = gc02m2->client;
	int ret = 0;

	mutex_lock(&gc02m2->mutex);

	/* If the power state is not modified - no work to do. */
	if (gc02m2->power_on == !!on)
		goto unlock_and_return;

	if (on) {
		ret = pm_runtime_get_sync(&client->dev);
		if (ret < 0) {
			pm_runtime_put_noidle(&client->dev);
			goto unlock_and_return;
		}

		ret = gc02m2_write_array(gc02m2->client, gc02m2->cur_mode->reg_list);
		if (ret) {
			v4l2_err(sd, "could not set init registers\n");
			pm_runtime_put_noidle(&client->dev);
			goto unlock_and_return;
		}

		gc02m2->power_on = true;
	} else {
		pm_runtime_put(&client->dev);
		gc02m2->power_on = false;
	}

unlock_and_return:
	mutex_unlock(&gc02m2->mutex);

	return ret;
}


static int gc02m2_runtime_resume(struct device *dev)
{
	struct i2c_client *client = to_i2c_client(dev);
	struct v4l2_subdev *sd = i2c_get_clientdata(client);
	struct gc02m2 *gc02m2 = to_gc02m2(sd);

	return __gc02m2_power_on(gc02m2);
}

static int gc02m2_runtime_suspend(struct device *dev)
{
	struct i2c_client *client = to_i2c_client(dev);
	struct v4l2_subdev *sd = i2c_get_clientdata(client);
	struct gc02m2 *gc02m2 = to_gc02m2(sd);

	__gc02m2_power_off(gc02m2);

	return 0;
}

#ifdef CONFIG_VIDEO_V4L2_SUBDEV_API
static int gc02m2_open(struct v4l2_subdev *sd, struct v4l2_subdev_fh *fh)
{
	struct gc02m2 *gc02m2 = to_gc02m2(sd);
	struct v4l2_mbus_framefmt *try_fmt =
				v4l2_subdev_get_try_format(sd, fh->state, 0);
	const struct gc02m2_mode *def_mode = &supported_modes[0];

	mutex_lock(&gc02m2->mutex);
	/* Initialize try_fmt */
	try_fmt->width = def_mode->width;
	try_fmt->height = def_mode->height;
	try_fmt->code = def_mode->bus_fmt;
	try_fmt->field = V4L2_FIELD_NONE;

	mutex_unlock(&gc02m2->mutex);
	/* No crop or compose */

	return 0;
}
#endif

static int gc02m2_enum_frame_interval(struct v4l2_subdev *sd,
					struct v4l2_subdev_state *sd_state,
					struct v4l2_subdev_frame_interval_enum *fie)
{
	if (fie->index >= ARRAY_SIZE(supported_modes))
		return -EINVAL;

	fie->code = supported_modes[fie->index].bus_fmt;
	fie->width = supported_modes[fie->index].width;
	fie->height = supported_modes[fie->index].height;
	fie->interval = supported_modes[fie->index].max_fps;
	return 0;
}

static const struct dev_pm_ops gc02m2_pm_ops = {
	SET_RUNTIME_PM_OPS(gc02m2_runtime_suspend,
			   gc02m2_runtime_resume, NULL)
};

#ifdef CONFIG_VIDEO_V4L2_SUBDEV_API
static const struct v4l2_subdev_internal_ops gc02m2_internal_ops = {
	.open = gc02m2_open,
};
#endif

static const struct v4l2_subdev_core_ops gc02m2_core_ops = {
	.s_power = gc02m2_s_power,
};

static const struct v4l2_subdev_video_ops gc02m2_video_ops = {
	.s_stream = gc02m2_s_stream,
	.g_frame_interval = gc02m2_g_frame_interval,
};

static const struct v4l2_subdev_pad_ops gc02m2_pad_ops = {
	.enum_mbus_code = gc02m2_enum_mbus_code,
	.enum_frame_size = gc02m2_enum_frame_sizes,
	.enum_frame_interval = gc02m2_enum_frame_interval,
	.get_fmt = gc02m2_get_fmt,
	.set_fmt = gc02m2_set_fmt,
};

static const struct v4l2_subdev_ops gc02m2_subdev_ops = {
	.core	= &gc02m2_core_ops,
	.video	= &gc02m2_video_ops,
	.pad	= &gc02m2_pad_ops,
};

#define DIGITAL_GAIN_BASE 1024
static int gc02m2_set_gain_reg(struct gc02m2 *gc02m2, u32 total_gain)
{
	struct device *dev = &gc02m2->client->dev;
	int ret = 0, i = 0;
	u32 dgain = 0;

	dev_dbg(dev, "total_gain = 0x%04x!\n", total_gain);
	if (total_gain < 0x40)
		total_gain = 0x40;

	for (i = 15; i >= 0; i--) {
		if (total_gain >= GC02M2_AGC_Param[i][0] &&
			total_gain <  GC02M2_AGC_Param[i + 1][0])
			break;
		}
	ret = gc02m2_write_reg(gc02m2->client,
		GC02M2_PAGE_SELECT,	0x00);
	ret |= gc02m2_write_reg(gc02m2->client,
		GC02M2_ANALOG_GAIN_REG, GC02M2_AGC_Param[i][1]);
	dgain = total_gain * DIGITAL_GAIN_BASE / GC02M2_AGC_Param[i][0];

	dev_dbg(dev, "AGC_Param[%d][0] = %d dgain = 0x%04x!\n",
		i, GC02M2_AGC_Param[i][0], dgain);
	ret |= gc02m2_write_reg(gc02m2->client,
		GC02M2_PREGAIN_H_REG,
		dgain >> 8);
	ret |= gc02m2_write_reg(gc02m2->client,
		GC02M2_PREGAIN_L_REG,
		dgain & 0xff);
	return ret;
}

static int gc02m2_set_ctrl(struct v4l2_ctrl *ctrl)
{
	struct gc02m2 *gc02m2 = container_of(ctrl->handler,
					     struct gc02m2, ctrl_handler);
	struct i2c_client *client = gc02m2->client;
	s64 max;
	int ret = 0;
	u32 vts = 0;
	u8 val = 0;

	/* Propagate change of current control to all related controls */
	switch (ctrl->id) {
	case V4L2_CID_VBLANK:
		/* Update max exposure while meeting expected vblanking */
		max = gc02m2->cur_mode->height + ctrl->val - 16;
		__v4l2_ctrl_modify_range(gc02m2->exposure,
					 gc02m2->exposure->minimum, max,
					 gc02m2->exposure->step,
					 gc02m2->exposure->default_value);
		break;
	}

	if (!pm_runtime_get_if_in_use(&client->dev))
		return 0;

	switch (ctrl->id) {
	case V4L2_CID_EXPOSURE:
		/* 4 least significant bits of expsoure are fractional part */
		ret = gc02m2_write_reg(gc02m2->client,
					 GC02M2_PAGE_SELECT, 0x00);
		ret |= gc02m2_write_reg(gc02m2->client,
					 GC02M2_REG_EXPOSURE_H,
					 (ctrl->val >> 8) & 0x3f);
		ret |= gc02m2_write_reg(gc02m2->client,
					 GC02M2_REG_EXPOSURE_L,
					 ctrl->val & 0xff);

		break;
	case V4L2_CID_ANALOGUE_GAIN:
		ret = gc02m2_set_gain_reg(gc02m2, ctrl->val);
		break;
	case V4L2_CID_VBLANK:
		vts = ctrl->val + gc02m2->cur_mode->height;
		ret = gc02m2_write_reg(gc02m2->client,
					 GC02M2_PAGE_SELECT, 0x00);
		ret |= gc02m2_write_reg(gc02m2->client, GC02M2_REG_VTS_H,
			(vts >> 8) & 0x3f);
		ret |= gc02m2_write_reg(gc02m2->client, GC02M2_REG_VTS_L,
			vts & 0xff);
		break;
	case V4L2_CID_HFLIP:
		ret = gc02m2_write_reg(gc02m2->client,
					 GC02M2_PAGE_SELECT, 0x00);
		ret |= gc02m2_read_reg(gc02m2->client, GC02M2_MIRROR_FLIP_REG, &val);
		ret |= gc02m2_write_reg(client, GC02M2_MIRROR_FLIP_REG,
			SC200AI_FETCH_MIRROR(val, ctrl->val));
		break;
	case V4L2_CID_VFLIP:
		ret = gc02m2_write_reg(gc02m2->client,
					 GC02M2_PAGE_SELECT, 0x00);
		ret |= gc02m2_read_reg(gc02m2->client, GC02M2_MIRROR_FLIP_REG, &val);
		ret |= gc02m2_write_reg(client, GC02M2_MIRROR_FLIP_REG,
			SC200AI_FETCH_FLIP(val, ctrl->val));
		break;
	default:
		dev_warn(&client->dev, "%s Unhandled id:0x%x, val:0x%x\n",
			 __func__, ctrl->id, ctrl->val);
		break;
	}

	pm_runtime_put(&client->dev);

	return ret;
}

static const struct v4l2_ctrl_ops gc02m2_ctrl_ops = {
	.s_ctrl = gc02m2_set_ctrl,
};

static int gc02m2_check_sensor_id(struct gc02m2 *gc02m2,
				  struct i2c_client *client)
{
	struct device *dev = &gc02m2->client->dev;
	u8 pid, ver = 0x00;
	int ret;
	unsigned short id;

	ret = gc02m2_read_reg(client, GC02M2_REG_CHIP_ID_H, &pid);
	if (ret) {
		dev_err(dev, "Read chip ID H register error\n");
		return ret;
	}

	ret = gc02m2_read_reg(client, GC02M2_REG_CHIP_ID_L, &ver);
	if (ret) {
		dev_err(dev, "Read chip ID L register error\n");
		return ret;
	}

	id = SENSOR_ID(pid, ver);
	if (id != CHIP_ID) {
		dev_err(dev, "Unexpected sensor id(%06x), ret(%d)\n", id, ret);
		return ret;
	}

	dev_info(dev, "detected gc%04x sensor\n", id);

	return 0;
}

static int gc02m2_configure_regulators(struct gc02m2 *gc02m2)
{
	unsigned int i;

	for (i = 0; i < GC02M2_NUM_SUPPLIES; i++)
		gc02m2->supplies[i].supply = gc02m2_supply_names[i];

	return devm_regulator_bulk_get(&gc02m2->client->dev,
				       GC02M2_NUM_SUPPLIES,
				       gc02m2->supplies);
}

static int gc02m2_parse_of(struct gc02m2 *gc02m2)
{
	struct device *dev = &gc02m2->client->dev;
	struct device_node *endpoint;
	struct fwnode_handle *fwnode;
	int rval;

	endpoint = of_graph_get_next_endpoint(dev->of_node, NULL);
	if (!endpoint) {
		dev_err(dev, "Failed to get endpoint\n");
		return -EINVAL;
	}
	fwnode = of_fwnode_handle(endpoint);
	rval = fwnode_property_read_u32_array(fwnode, "data-lanes", NULL, 0);
	of_node_put(endpoint);
	if (rval <= 0) {
		dev_warn(dev, " Get mipi lane num failed!\n");
		return -1;
	}

	gc02m2->lane_num = rval;
	if (1 == gc02m2->lane_num) {
		gc02m2->cur_mode = &supported_modes[0];
		/* pixel rate = link frequency * 2 * lanes / BITS_PER_SAMPLE */
		gc02m2->pixel_rate = GC02M2_MIPI_LINK_FREQ * 2U * gc02m2->lane_num / 10U;
		dev_info(dev, "lane_num(%d)  pixel_rate(%u)\n",
				 gc02m2->lane_num, gc02m2->pixel_rate);
	} else {
		dev_err(dev, "unsupported lane_num(%d)\n", gc02m2->lane_num);
		return -1;
	}
	return 0;
}

static int gc02m2_initialize_controls(struct gc02m2 *gc02m2)
{
	const struct gc02m2_mode *mode;
	struct v4l2_ctrl_handler *handler;
	struct v4l2_ctrl *ctrl;
	s64 exposure_max, vblank_def;
	u32 h_blank;
	int ret;
	struct device *dev = &gc02m2->client->dev;

	dev_info(dev, "Enter %s(%d) !\n", __func__, __LINE__);
	handler = &gc02m2->ctrl_handler;
	mode = gc02m2->cur_mode;
	ret = v4l2_ctrl_handler_init(handler, 8);
	if (ret)
		return ret;
	handler->lock = &gc02m2->mutex;

	ctrl = v4l2_ctrl_new_int_menu(handler, NULL, V4L2_CID_LINK_FREQ,
				      0, 0, link_freq_menu_items);
	if (ctrl)
		ctrl->flags |= V4L2_CTRL_FLAG_READ_ONLY;

	v4l2_ctrl_new_std(handler, NULL, V4L2_CID_PIXEL_RATE,
			  0, GC02M2_PIXEL_RATE, 1, GC02M2_PIXEL_RATE);

	h_blank = mode->hts_def - mode->width;
	gc02m2->hblank = v4l2_ctrl_new_std(handler, NULL, V4L2_CID_HBLANK,
				h_blank, h_blank, 1, h_blank);
	if (gc02m2->hblank)
		gc02m2->hblank->flags |= V4L2_CTRL_FLAG_READ_ONLY;

	vblank_def = mode->vts_def - mode->height;
	gc02m2->vblank = v4l2_ctrl_new_std(handler, &gc02m2_ctrl_ops,
				V4L2_CID_VBLANK, vblank_def,
				GC02M2_VTS_MAX - mode->height,
				1, vblank_def);

	exposure_max = mode->vts_def - 16;
	gc02m2->exposure = v4l2_ctrl_new_std(handler, &gc02m2_ctrl_ops,
				V4L2_CID_EXPOSURE, GC02M2_EXPOSURE_MIN,
				exposure_max, GC02M2_EXPOSURE_STEP,
				mode->exp_def);

	gc02m2->anal_gain = v4l2_ctrl_new_std(handler, &gc02m2_ctrl_ops,
				V4L2_CID_ANALOGUE_GAIN, GC02M2_GAIN_MIN,
				GC02M2_GAIN_MAX, GC02M2_GAIN_STEP,
				GC02M2_GAIN_DEFAULT);

	v4l2_ctrl_new_std(handler, &gc02m2_ctrl_ops,
				V4L2_CID_HFLIP, 0, 1, 1, 0);

	v4l2_ctrl_new_std(handler, &gc02m2_ctrl_ops,
				V4L2_CID_VFLIP, 0, 1, 1, 0);

	if (handler->error) {
		ret = handler->error;
		dev_err(&gc02m2->client->dev,
			"Failed to init controls(%d)\n", ret);
		goto err_free_handler;
	}

	gc02m2->subdev.ctrl_handler = handler;

	return 0;

err_free_handler:
	v4l2_ctrl_handler_free(handler);

	return ret;
}

static int gc02m2_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	struct gc02m2 *gc02m2;
	struct v4l2_subdev *sd;
	int ret;

	gc02m2 = devm_kzalloc(dev, sizeof(*gc02m2), GFP_KERNEL);
	if (!gc02m2)
		return -ENOMEM;

	gc02m2->client = client;
	gc02m2->cur_mode = &supported_modes[0];

	gc02m2->xvclk = devm_clk_get(dev, "xvclk");
	if (IS_ERR(gc02m2->xvclk)) {
		dev_err(dev, "Failed to get xvclk\n");
		return -EINVAL;
	}

	gc02m2->reset_gpio = devm_gpiod_get(dev, "reset", GPIOD_OUT_LOW);
	if (IS_ERR(gc02m2->reset_gpio))
		dev_warn(dev, "Failed to get reset-gpios\n");

	gc02m2->pwdn_gpio = devm_gpiod_get(dev, "powerdown", GPIOD_OUT_HIGH);
	if (IS_ERR(gc02m2->pwdn_gpio))
		dev_warn(dev, "Failed to get pwdn-gpios\n");

	ret = gc02m2_parse_of(gc02m2);
	if (ret != 0)
		return -EINVAL;

	gc02m2->pinctrl = devm_pinctrl_get(dev);
	if (IS_ERR(gc02m2->pinctrl))
		dev_err(dev, "no pinctrl\n");

	ret = gc02m2_configure_regulators(gc02m2);
	if (ret) {
		dev_err(dev, "Failed to get power regulators\n");
		return ret;
	}

	mutex_init(&gc02m2->mutex);

	sd = &gc02m2->subdev;
	v4l2_i2c_subdev_init(sd, client, &gc02m2_subdev_ops);
	ret = gc02m2_initialize_controls(gc02m2);
	if (ret)
		goto err_destroy_mutex;

	ret = __gc02m2_power_on(gc02m2);
	if (ret)
		goto err_free_handler;

	ret = gc02m2_check_sensor_id(gc02m2, client);
	if (ret)
		goto err_power_off;

#ifdef CONFIG_VIDEO_V4L2_SUBDEV_API
	sd->internal_ops = &gc02m2_internal_ops;
	sd->flags |= V4L2_SUBDEV_FL_HAS_DEVNODE;
#endif
	gc02m2->pad.flags = MEDIA_PAD_FL_SOURCE;
	sd->entity.function = MEDIA_ENT_F_CAM_SENSOR;
	ret = media_entity_pads_init(&sd->entity, 1, &gc02m2->pad);
	if (ret < 0)
		goto err_power_off;

	ret = v4l2_async_register_subdev_sensor(sd);
	if (ret) {
		dev_err(dev, "v4l2 async register subdev failed\n");
		goto err_clean_entity;
	}

	pm_runtime_set_active(dev);
	pm_runtime_enable(dev);
	pm_runtime_idle(dev);

	return 0;

err_clean_entity:
	media_entity_cleanup(&sd->entity);
err_power_off:
	__gc02m2_power_off(gc02m2);
err_free_handler:
	v4l2_ctrl_handler_free(&gc02m2->ctrl_handler);
err_destroy_mutex:
	mutex_destroy(&gc02m2->mutex);

	return ret;
}

static void gc02m2_remove(struct i2c_client *client)
{
	struct v4l2_subdev *sd = i2c_get_clientdata(client);
	struct gc02m2 *gc02m2 = to_gc02m2(sd);

	v4l2_async_unregister_subdev(sd);
	media_entity_cleanup(&sd->entity);
	v4l2_ctrl_handler_free(&gc02m2->ctrl_handler);
	mutex_destroy(&gc02m2->mutex);

	pm_runtime_disable(&client->dev);
	if (!pm_runtime_status_suspended(&client->dev))
		__gc02m2_power_off(gc02m2);
	pm_runtime_set_suspended(&client->dev);
}

#if IS_ENABLED(CONFIG_OF)
static const struct of_device_id gc02m2_of_match[] = {
	{ .compatible = "galaxycore,gc02m2" },
	{},
};
MODULE_DEVICE_TABLE(of, gc02m2_of_match);
#endif

static const struct i2c_device_id gc02m2_match_id[] = {
	{ "galaxycore,gc02m2", 0 },
	{ },
};

static struct i2c_driver gc02m2_i2c_driver = {
	.driver = {
		.name = GC02M2_NAME,
		.pm = &gc02m2_pm_ops,
		.of_match_table = of_match_ptr(gc02m2_of_match),
	},
	.probe		= &gc02m2_probe,
	.remove		= &gc02m2_remove,
	.id_table	= gc02m2_match_id,
};

static int __init sensor_mod_init(void)
{
	return i2c_add_driver(&gc02m2_i2c_driver);
}

static void __exit sensor_mod_exit(void)
{
	i2c_del_driver(&gc02m2_i2c_driver);
}

device_initcall_sync(sensor_mod_init);
module_exit(sensor_mod_exit);

MODULE_DESCRIPTION("GalaxyCore gc02m2 sensor driver");
MODULE_LICENSE("GPL v2");
