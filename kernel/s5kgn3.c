// SPDX-License-Identifier: GPL-2.0-only
/*
 * Samsung S5KGN3 CMOS Image Sensor driver (C-PHY 3-trio)
 *
 * Register sequences extracted from Samsung Galaxy S22 (r0q) CamX binary.
 */

#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/pm_runtime.h>
#include <linux/regulator/consumer.h>
#include <media/v4l2-cci.h>
#include <media/v4l2-ctrls.h>
#include <media/v4l2-device.h>
#include <media/v4l2-fwnode.h>
#include <media/v4l2-mediabus.h>

#include "s5kgn3-regs.h"

#define S5KGN3_REG_CHIP_ID		CCI_REG16(0x0000)
#define S5KGN3_CHIP_ID			0x08e3

#define S5KGN3_REG_MODE_SELECT		CCI_REG16(0x0100)
#define S5KGN3_REG_STREAM_CFG		CCI_REG16(0x0b32)
#define S5KGN3_REG_STREAM_STOP		CCI_REG16(0x0e00)
#define S5KGN3_MODE_STANDBY		0x0003
#define S5KGN3_MODE_STREAMING		0x0103

#define S5KGN3_REG_EXPOSURE		CCI_REG16(0x0202)
#define S5KGN3_EXPOSURE_MIN		4
#define S5KGN3_EXPOSURE_STEP		1
#define S5KGN3_EXPOSURE_MARGIN		8

#define S5KGN3_REG_ANALOGUE_GAIN	CCI_REG16(0x0204)
#define S5KGN3_ANA_GAIN_MIN		0x0020
#define S5KGN3_ANA_GAIN_MAX		0x0400
#define S5KGN3_ANA_GAIN_STEP		1
#define S5KGN3_ANA_GAIN_DEFAULT		0x0020

#define S5KGN3_REG_VTS			CCI_REG16(0x0340)
#define S5KGN3_VTS_MAX			0xffff
#define S5KGN3_VBLANK_MIN		32

#define S5KGN3_XCLK_FREQ		19200000
#define S5KGN3_NUM_DATA_LANES		3

#define S5KGN3_NATIVE_WIDTH		8160
#define S5KGN3_NATIVE_HEIGHT		6120

static const char * const s5kgn3_supply_names[] = {
	"vana",
	"vio",
	"vdig",
	"vcustom1",
};

#define S5KGN3_NUM_SUPPLIES		ARRAY_SIZE(s5kgn3_supply_names)

#define S5KGN3_PIXEL_RATE_MIN		1768550400ULL
#define S5KGN3_PIXEL_RATE_MAX		2267596800ULL

static const s64 s5kgn3_link_freq_menu[] = {
	1657600000ULL,
	1292800000ULL,
};

struct s5kgn3_mode {
	u32 width;
	u32 height;
	u32 hts;
	u32 vts_def;
	u64 pixel_rate;
	u32 link_freq_idx;
	struct v4l2_rect crop;
	const struct cci_reg_sequence *regs;
	unsigned int num_regs;
};

static const struct s5kgn3_mode s5kgn3_modes[] = {
	{
		.width = 4080,
		.height = 3060,
		.hts = 5472,
		.vts_def = 11696,
		.pixel_rate = 2267596800ULL,
		.link_freq_idx = 0,
		.crop = {
			.left = 0,
			.top = 0,
			.width = 8160,
			.height = 6120,
		},
		.regs = s5kgn3_mode_4080x3060_30fps_regs,
		.num_regs = ARRAY_SIZE(s5kgn3_mode_4080x3060_30fps_regs),
	},
	{
		.width = 2040,
		.height = 1532,
		.hts = 9152,
		.vts_def = 6992,
		.pixel_rate = 1768550400ULL,
		.link_freq_idx = 1,
		.crop = {
			.left = 32,
			.top = 64,
			.width = 8160,
			.height = 6120,
		},
		.regs = s5kgn3_mode_2040x1532_30fps_regs,
		.num_regs = ARRAY_SIZE(s5kgn3_mode_2040x1532_30fps_regs),
	},
};

struct s5kgn3 {
	struct v4l2_subdev sd;
	struct media_pad pad;
	struct device *dev;
	struct regmap *regmap;

	struct clk *xclk;
	struct gpio_desc *reset_gpio;
	struct regulator_bulk_data supplies[S5KGN3_NUM_SUPPLIES];

	struct v4l2_ctrl_handler ctrl_handler;
	struct v4l2_ctrl *link_freq;
	struct v4l2_ctrl *pixel_rate;
	struct v4l2_ctrl *hblank;
	struct v4l2_ctrl *vblank;
	struct v4l2_ctrl *exposure;
	struct v4l2_ctrl *gain;

	const struct s5kgn3_mode *cur_mode;
};

static inline struct s5kgn3 *to_s5kgn3(struct v4l2_subdev *sd)
{
	return container_of(sd, struct s5kgn3, sd);
}

static int s5kgn3_write_seq(struct regmap *regmap,
			    const struct s5kgn3_reg_seq *regs,
			    unsigned int count)
{
	unsigned int i;
	int ret;

	for (i = 0; i < count; i++) {
		ret = cci_write(regmap, regs[i].reg, regs[i].val, NULL);
		if (ret)
			return ret;

		if (regs[i].delay_us)
			fsleep(regs[i].delay_us);
	}

	return 0;
}

static int s5kgn3_power_on(struct device *dev)
{
	struct v4l2_subdev *sd = dev_get_drvdata(dev);
	struct s5kgn3 *s5kgn3 = to_s5kgn3(sd);
	int ret;

	ret = regulator_bulk_enable(S5KGN3_NUM_SUPPLIES, s5kgn3->supplies);
	if (ret) {
		dev_err(dev, "failed to enable regulators: %d\n", ret);
		return ret;
	}

	fsleep(1000);
	gpiod_set_value_cansleep(s5kgn3->reset_gpio, 0);
	fsleep(2000);

	ret = clk_prepare_enable(s5kgn3->xclk);
	if (ret) {
		dev_err(dev, "failed to enable xclk: %d\n", ret);
		goto err_reset;
	}

	fsleep(5000);
	return 0;

err_reset:
	gpiod_set_value_cansleep(s5kgn3->reset_gpio, 1);
	regulator_bulk_disable(S5KGN3_NUM_SUPPLIES, s5kgn3->supplies);
	return ret;
}

static int s5kgn3_power_off(struct device *dev)
{
	struct v4l2_subdev *sd = dev_get_drvdata(dev);
	struct s5kgn3 *s5kgn3 = to_s5kgn3(sd);

	gpiod_set_value_cansleep(s5kgn3->reset_gpio, 1);
	clk_disable_unprepare(s5kgn3->xclk);
	regulator_bulk_disable(S5KGN3_NUM_SUPPLIES, s5kgn3->supplies);

	return 0;
}

static int s5kgn3_identify_module(struct s5kgn3 *s5kgn3)
{
	u64 val;
	int ret;

	ret = cci_read(s5kgn3->regmap, S5KGN3_REG_CHIP_ID, &val, NULL);
	if (ret) {
		dev_err(s5kgn3->dev, "failed to read chip ID: %d\n", ret);
		return ret;
	}

	if (val != S5KGN3_CHIP_ID) {
		dev_err(s5kgn3->dev, "chip ID mismatch: expected 0x%04x, got 0x%04llx\n",
			S5KGN3_CHIP_ID, val);
		return -ENXIO;
	}

	dev_info(s5kgn3->dev, "detected S5KGN3 sensor (ID 0x%04llx)\n", val);
	return 0;
}

static int s5kgn3_set_ctrl(struct v4l2_ctrl *ctrl)
{
	struct s5kgn3 *s5kgn3 = container_of(ctrl->handler, struct s5kgn3,
					     ctrl_handler);
	const struct s5kgn3_mode *mode = s5kgn3->cur_mode;
	s64 exposure_max;
	int pm_status;
	int ret = 0;

	if (ctrl->id == V4L2_CID_VBLANK) {
		exposure_max = mode->height + ctrl->val - S5KGN3_EXPOSURE_MARGIN;
		__v4l2_ctrl_modify_range(s5kgn3->exposure, S5KGN3_EXPOSURE_MIN,
					 exposure_max, S5KGN3_EXPOSURE_STEP,
					 min_t(s64, s5kgn3->exposure->val, exposure_max));
	}

	pm_status = pm_runtime_get_if_in_use(s5kgn3->dev);
	if (pm_status <= 0)
		return pm_status < 0 ? pm_status : 0;

	switch (ctrl->id) {
	case V4L2_CID_ANALOGUE_GAIN:
		cci_write(s5kgn3->regmap, S5KGN3_REG_ANALOGUE_GAIN, ctrl->val, &ret);
		break;
	case V4L2_CID_EXPOSURE:
		cci_write(s5kgn3->regmap, S5KGN3_REG_EXPOSURE, ctrl->val, &ret);
		break;
	case V4L2_CID_VBLANK:
		cci_write(s5kgn3->regmap, S5KGN3_REG_VTS,
			  mode->height + ctrl->val, &ret);
		break;
	default:
		ret = -EINVAL;
		break;
	}

	pm_runtime_put_autosuspend(s5kgn3->dev);
	return ret;
}

static const struct v4l2_ctrl_ops s5kgn3_ctrl_ops = {
	.s_ctrl = s5kgn3_set_ctrl,
};

static int s5kgn3_init_controls(struct s5kgn3 *s5kgn3)
{
	struct v4l2_ctrl_handler *hdlr = &s5kgn3->ctrl_handler;
	const struct s5kgn3_mode *mode = s5kgn3->cur_mode;
	s64 hblank_def = mode->hts - mode->width;
	s64 vblank_def = mode->vts_def - mode->height;
	s64 exposure_max = mode->vts_def - S5KGN3_EXPOSURE_MARGIN;
	int ret;

	ret = v4l2_ctrl_handler_init(hdlr, 6);
	if (ret)
		return ret;

	s5kgn3->link_freq = v4l2_ctrl_new_int_menu(hdlr, &s5kgn3_ctrl_ops,
						   V4L2_CID_LINK_FREQ,
						   ARRAY_SIZE(s5kgn3_link_freq_menu) - 1,
						   mode->link_freq_idx,
						   s5kgn3_link_freq_menu);
	if (s5kgn3->link_freq)
		s5kgn3->link_freq->flags |= V4L2_CTRL_FLAG_READ_ONLY;

	s5kgn3->pixel_rate = v4l2_ctrl_new_std(hdlr, &s5kgn3_ctrl_ops,
					       V4L2_CID_PIXEL_RATE,
					       S5KGN3_PIXEL_RATE_MIN,
					       S5KGN3_PIXEL_RATE_MAX,
					       1, mode->pixel_rate);

	s5kgn3->hblank = v4l2_ctrl_new_std(hdlr, &s5kgn3_ctrl_ops,
					   V4L2_CID_HBLANK, hblank_def, hblank_def,
					   1, hblank_def);
	if (s5kgn3->hblank)
		s5kgn3->hblank->flags |= V4L2_CTRL_FLAG_READ_ONLY;

	s5kgn3->vblank = v4l2_ctrl_new_std(hdlr, &s5kgn3_ctrl_ops,
					   V4L2_CID_VBLANK, S5KGN3_VBLANK_MIN,
					   S5KGN3_VTS_MAX - mode->height, 1,
					   vblank_def);

	s5kgn3->exposure = v4l2_ctrl_new_std(hdlr, &s5kgn3_ctrl_ops,
					     V4L2_CID_EXPOSURE, S5KGN3_EXPOSURE_MIN,
					     exposure_max, S5KGN3_EXPOSURE_STEP,
					     exposure_max);

	s5kgn3->gain = v4l2_ctrl_new_std(hdlr, &s5kgn3_ctrl_ops,
					 V4L2_CID_ANALOGUE_GAIN,
					 S5KGN3_ANA_GAIN_MIN, S5KGN3_ANA_GAIN_MAX,
					 S5KGN3_ANA_GAIN_STEP, S5KGN3_ANA_GAIN_DEFAULT);

	if (hdlr->error) {
		ret = hdlr->error;
		v4l2_ctrl_handler_free(hdlr);
		return ret;
	}

	s5kgn3->sd.ctrl_handler = hdlr;
	return 0;
}

static void s5kgn3_fill_format(const struct s5kgn3_mode *mode,
			       struct v4l2_mbus_framefmt *fmt)
{
	fmt->code = MEDIA_BUS_FMT_SGRBG10_1X10;
	fmt->width = mode->width;
	fmt->height = mode->height;
	fmt->field = V4L2_FIELD_NONE;
	fmt->colorspace = V4L2_COLORSPACE_RAW;
	fmt->ycbcr_enc = V4L2_YCBCR_ENC_DEFAULT;
	fmt->quantization = V4L2_QUANTIZATION_FULL_RANGE;
	fmt->xfer_func = V4L2_XFER_FUNC_NONE;
}

static int s5kgn3_init_state(struct v4l2_subdev *sd,
			     struct v4l2_subdev_state *state)
{
	const struct s5kgn3_mode *mode = &s5kgn3_modes[0];

	s5kgn3_fill_format(mode, v4l2_subdev_state_get_format(state, 0));
	*v4l2_subdev_state_get_crop(state, 0) = mode->crop;

	return 0;
}

static int s5kgn3_enum_mbus_code(struct v4l2_subdev *sd,
				 struct v4l2_subdev_state *state,
				 struct v4l2_subdev_mbus_code_enum *code)
{
	if (code->index > 0)
		return -EINVAL;

	code->code = MEDIA_BUS_FMT_SGRBG10_1X10;
	return 0;
}

static int s5kgn3_enum_frame_size(struct v4l2_subdev *sd,
				  struct v4l2_subdev_state *state,
				  struct v4l2_subdev_frame_size_enum *fse)
{
	if (fse->index >= ARRAY_SIZE(s5kgn3_modes) ||
	    fse->code != MEDIA_BUS_FMT_SGRBG10_1X10)
		return -EINVAL;

	fse->min_width = s5kgn3_modes[fse->index].width;
	fse->max_width = s5kgn3_modes[fse->index].width;
	fse->min_height = s5kgn3_modes[fse->index].height;
	fse->max_height = s5kgn3_modes[fse->index].height;

	return 0;
}

static int s5kgn3_set_pad_format(struct v4l2_subdev *sd,
				 struct v4l2_subdev_state *state,
				 struct v4l2_subdev_format *fmt)
{
	struct s5kgn3 *s5kgn3 = to_s5kgn3(sd);
	const struct s5kgn3_mode *mode;

	mode = v4l2_find_nearest_size(s5kgn3_modes, ARRAY_SIZE(s5kgn3_modes),
				      width, height,
				      fmt->format.width, fmt->format.height);

	s5kgn3_fill_format(mode, &fmt->format);
	*v4l2_subdev_state_get_format(state, 0) = fmt->format;
	*v4l2_subdev_state_get_crop(state, 0) = mode->crop;

	if (fmt->which == V4L2_SUBDEV_FORMAT_TRY)
		return 0;

	if (s5kgn3->cur_mode != mode) {
		s64 hblank = mode->hts - mode->width;
		s64 vblank = mode->vts_def - mode->height;
		s64 exp_max = mode->vts_def - S5KGN3_EXPOSURE_MARGIN;

		s5kgn3->cur_mode = mode;
		__v4l2_ctrl_s_ctrl(s5kgn3->link_freq, mode->link_freq_idx);
		__v4l2_ctrl_s_ctrl_int64(s5kgn3->pixel_rate, mode->pixel_rate);
		__v4l2_ctrl_modify_range(s5kgn3->hblank, hblank, hblank, 1, hblank);
		__v4l2_ctrl_modify_range(s5kgn3->vblank, S5KGN3_VBLANK_MIN,
					 S5KGN3_VTS_MAX - mode->height, 1, vblank);
		__v4l2_ctrl_s_ctrl(s5kgn3->vblank, vblank);
		__v4l2_ctrl_modify_range(s5kgn3->exposure, S5KGN3_EXPOSURE_MIN,
					 exp_max, S5KGN3_EXPOSURE_STEP, exp_max);
		__v4l2_ctrl_s_ctrl(s5kgn3->exposure, exp_max);
	}

	return s5kgn3->ctrl_handler.error;
}

static int s5kgn3_get_selection(struct v4l2_subdev *sd,
				struct v4l2_subdev_state *state,
				struct v4l2_subdev_selection *sel)
{
	switch (sel->target) {
	case V4L2_SEL_TGT_CROP:
		sel->r = *v4l2_subdev_state_get_crop(state, 0);
		return 0;
	case V4L2_SEL_TGT_NATIVE_SIZE:
	case V4L2_SEL_TGT_CROP_DEFAULT:
	case V4L2_SEL_TGT_CROP_BOUNDS:
		sel->r = (struct v4l2_rect){
			.left = 0,
			.top = 0,
			.width = S5KGN3_NATIVE_WIDTH,
			.height = S5KGN3_NATIVE_HEIGHT,
		};
		return 0;
	default:
		return -EINVAL;
	}
}

static int s5kgn3_enable_streams(struct v4l2_subdev *sd,
				 struct v4l2_subdev_state *state, u32 pad,
				 u64 streams_mask)
{
	struct s5kgn3 *s5kgn3 = to_s5kgn3(sd);
	const struct s5kgn3_mode *mode = s5kgn3->cur_mode;
	int ret;

	ret = pm_runtime_resume_and_get(s5kgn3->dev);
	if (ret < 0)
		return ret;

	ret = s5kgn3_write_seq(s5kgn3->regmap, s5kgn3_init_regs,
			       ARRAY_SIZE(s5kgn3_init_regs));
	if (ret)
		goto err_rpm;

	ret = cci_multi_reg_write(s5kgn3->regmap, mode->regs, mode->num_regs, NULL);
	if (ret)
		goto err_rpm;

	ret = __v4l2_ctrl_handler_setup(&s5kgn3->ctrl_handler);
	if (ret)
		goto err_rpm;

	cci_write(s5kgn3->regmap, S5KGN3_REG_STREAM_CFG, 0x0000, &ret);
	cci_write(s5kgn3->regmap, S5KGN3_REG_MODE_SELECT,
		  S5KGN3_MODE_STREAMING, &ret);
	if (ret)
		goto err_rpm;

	return 0;

err_rpm:
	pm_runtime_put_autosuspend(s5kgn3->dev);
	return ret;
}

static int s5kgn3_disable_streams(struct v4l2_subdev *sd,
				  struct v4l2_subdev_state *state, u32 pad,
				  u64 streams_mask)
{
	struct s5kgn3 *s5kgn3 = to_s5kgn3(sd);
	int ret = 0;

	cci_write(s5kgn3->regmap, S5KGN3_REG_STREAM_CFG, 0x0000, &ret);
	cci_write(s5kgn3->regmap, S5KGN3_REG_STREAM_STOP, 0x0003, &ret);
	cci_write(s5kgn3->regmap, S5KGN3_REG_MODE_SELECT,
		  S5KGN3_MODE_STANDBY, &ret);
	if (ret)
		dev_err(s5kgn3->dev, "failed to stop streaming: %d\n", ret);

	pm_runtime_put_autosuspend(s5kgn3->dev);
	return ret;
}

static const struct v4l2_subdev_video_ops s5kgn3_video_ops = {
	.s_stream = v4l2_subdev_s_stream_helper,
};

static const struct v4l2_subdev_pad_ops s5kgn3_pad_ops = {
	.enum_mbus_code = s5kgn3_enum_mbus_code,
	.enum_frame_size = s5kgn3_enum_frame_size,
	.get_fmt = v4l2_subdev_get_fmt,
	.set_fmt = s5kgn3_set_pad_format,
	.get_selection = s5kgn3_get_selection,
	.enable_streams = s5kgn3_enable_streams,
	.disable_streams = s5kgn3_disable_streams,
};

static const struct v4l2_subdev_ops s5kgn3_subdev_ops = {
	.video = &s5kgn3_video_ops,
	.pad = &s5kgn3_pad_ops,
};

static const struct v4l2_subdev_internal_ops s5kgn3_internal_ops = {
	.init_state = s5kgn3_init_state,
};

static int s5kgn3_check_hwcfg(struct device *dev)
{
	struct v4l2_fwnode_endpoint bus_cfg = {
		.bus_type = V4L2_MBUS_CSI2_CPHY,
	};
	struct fwnode_handle *ep;
	unsigned int i, j;
	int ret;

	ep = fwnode_graph_get_next_endpoint(dev_fwnode(dev), NULL);
	if (!ep)
		return dev_err_probe(dev, -ENXIO, "no endpoint node found\n");

	ret = v4l2_fwnode_endpoint_alloc_parse(ep, &bus_cfg);
	fwnode_handle_put(ep);
	if (ret)
		return dev_err_probe(dev, ret, "failed to parse endpoint\n");

	if (bus_cfg.bus.mipi_csi2.num_data_lanes != S5KGN3_NUM_DATA_LANES) {
		dev_err(dev, "expected %d C-PHY trios, got %u\n",
			S5KGN3_NUM_DATA_LANES, bus_cfg.bus.mipi_csi2.num_data_lanes);
		ret = -EINVAL;
		goto out_free;
	}

	if (!bus_cfg.nr_of_link_frequencies) {
		dev_err(dev, "no link-frequencies specified in DT\n");
		ret = -EINVAL;
		goto out_free;
	}

	for (i = 0; i < ARRAY_SIZE(s5kgn3_link_freq_menu); i++) {
		for (j = 0; j < bus_cfg.nr_of_link_frequencies; j++)
			if (bus_cfg.link_frequencies[j] == s5kgn3_link_freq_menu[i])
				break;
		if (j == bus_cfg.nr_of_link_frequencies) {
			dev_err(dev, "missing expected link frequency %lld Hz\n",
				s5kgn3_link_freq_menu[i]);
			ret = -EINVAL;
			goto out_free;
		}
	}

out_free:
	v4l2_fwnode_endpoint_free(&bus_cfg);
	return ret;
}

static int s5kgn3_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	struct s5kgn3 *s5kgn3;
	unsigned int i;
	int ret;

	ret = s5kgn3_check_hwcfg(dev);
	if (ret)
		return ret;

	s5kgn3 = devm_kzalloc(dev, sizeof(*s5kgn3), GFP_KERNEL);
	if (!s5kgn3)
		return -ENOMEM;

	s5kgn3->dev = dev;
	s5kgn3->cur_mode = &s5kgn3_modes[0];

	v4l2_i2c_subdev_init(&s5kgn3->sd, client, &s5kgn3_subdev_ops);

	s5kgn3->regmap = devm_cci_regmap_init_i2c(client, 16);
	if (IS_ERR(s5kgn3->regmap))
		return dev_err_probe(dev, PTR_ERR(s5kgn3->regmap),
				     "failed to init CCI regmap\n");

	s5kgn3->xclk = devm_clk_get(dev, NULL);
	if (IS_ERR(s5kgn3->xclk))
		return dev_err_probe(dev, PTR_ERR(s5kgn3->xclk),
				     "failed to get xclk\n");

	ret = clk_set_rate(s5kgn3->xclk, S5KGN3_XCLK_FREQ);
	if (ret)
		dev_warn(dev, "failed to set xclk rate to %u Hz: %d\n",
			 S5KGN3_XCLK_FREQ, ret);

	for (i = 0; i < S5KGN3_NUM_SUPPLIES; i++)
		s5kgn3->supplies[i].supply = s5kgn3_supply_names[i];

	ret = devm_regulator_bulk_get(dev, S5KGN3_NUM_SUPPLIES, s5kgn3->supplies);
	if (ret)
		return dev_err_probe(dev, ret, "failed to get regulators\n");

	s5kgn3->reset_gpio = devm_gpiod_get_optional(dev, "reset", GPIOD_OUT_HIGH);
	if (IS_ERR(s5kgn3->reset_gpio))
		return dev_err_probe(dev, PTR_ERR(s5kgn3->reset_gpio),
				     "failed to get reset gpio\n");

	ret = s5kgn3_power_on(dev);
	if (ret)
		return ret;

	ret = s5kgn3_identify_module(s5kgn3);
	if (ret)
		goto err_power_off;

	ret = s5kgn3_init_controls(s5kgn3);
	if (ret)
		goto err_power_off;

	s5kgn3->sd.internal_ops = &s5kgn3_internal_ops;
	s5kgn3->sd.flags |= V4L2_SUBDEV_FL_HAS_DEVNODE;
	s5kgn3->sd.entity.function = MEDIA_ENT_F_CAM_SENSOR;
	s5kgn3->pad.flags = MEDIA_PAD_FL_SOURCE;

	ret = media_entity_pads_init(&s5kgn3->sd.entity, 1, &s5kgn3->pad);
	if (ret)
		goto err_ctrl_free;

	s5kgn3->sd.state_lock = s5kgn3->ctrl_handler.lock;
	ret = v4l2_subdev_init_finalize(&s5kgn3->sd);
	if (ret)
		goto err_media_cleanup;

	pm_runtime_set_active(dev);
	pm_runtime_enable(dev);

	ret = v4l2_async_register_subdev_sensor(&s5kgn3->sd);
	if (ret) {
		dev_err_probe(dev, ret, "failed to register subdev\n");
		goto err_rpm_disable;
	}

	pm_runtime_set_autosuspend_delay(dev, 1000);
	pm_runtime_use_autosuspend(dev);
	pm_runtime_idle(dev);

	return 0;

err_rpm_disable:
	pm_runtime_disable(dev);
	pm_runtime_set_suspended(dev);
	v4l2_subdev_cleanup(&s5kgn3->sd);
err_media_cleanup:
	media_entity_cleanup(&s5kgn3->sd.entity);
err_ctrl_free:
	v4l2_ctrl_handler_free(&s5kgn3->ctrl_handler);
err_power_off:
	s5kgn3_power_off(dev);
	return ret;
}

static void s5kgn3_remove(struct i2c_client *client)
{
	struct v4l2_subdev *sd = i2c_get_clientdata(client);
	struct s5kgn3 *s5kgn3 = to_s5kgn3(sd);

	v4l2_async_unregister_subdev(sd);
	pm_runtime_dont_use_autosuspend(s5kgn3->dev);
	pm_runtime_disable(s5kgn3->dev);
	if (!pm_runtime_status_suspended(s5kgn3->dev))
		s5kgn3_power_off(s5kgn3->dev);
	pm_runtime_set_suspended(s5kgn3->dev);
	v4l2_subdev_cleanup(sd);
	media_entity_cleanup(&sd->entity);
	v4l2_ctrl_handler_free(&s5kgn3->ctrl_handler);
}

static const struct dev_pm_ops s5kgn3_pm_ops = {
	SET_RUNTIME_PM_OPS(s5kgn3_power_off, s5kgn3_power_on, NULL)
};

static const struct of_device_id s5kgn3_dt_ids[] = {
	{ .compatible = "samsung,s5kgn3" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, s5kgn3_dt_ids);

static struct i2c_driver s5kgn3_i2c_driver = {
	.driver = {
		.name = "s5kgn3",
		.pm = &s5kgn3_pm_ops,
		.of_match_table = s5kgn3_dt_ids,
	},
	.probe = s5kgn3_probe,
	.remove = s5kgn3_remove,
};
module_i2c_driver(s5kgn3_i2c_driver);

MODULE_DESCRIPTION("Samsung S5KGN3 CMOS image sensor driver");
MODULE_LICENSE("GPL");
