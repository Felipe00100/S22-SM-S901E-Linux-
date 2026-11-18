// SPDX-License-Identifier: GPL-2.0-only
/*
 * Samsung S2MPB02 camera PMIC: LDO, Buck, Buck-Boost regulators + FLED Flash/Torch
 * for Samsung Galaxy S22 (SM-S901E, r0q).
 *
 * Register layout from Samsung's GPL s2mpb02 / leds-s2mpb02 drivers:
 *  - BUCK1: enable_reg = 0x0b (mask 0xc0), vsel_reg = 0x0c (mask 0xff), 400 mV + n * 6.25 mV
 *  - BUCK2: enable_reg = 0x0d (mask 0xc0), vsel_reg = 0x0e (mask 0xff), 400 mV + n * 6.25 mV
 *  - BB1:   enable_reg = 0x0f (mask 0xc0), vsel_reg = 0x10 (mask 0xff), 2600 mV + n * 12.5 mV
 *  - FLED_CTRL1 = 0x12 ([7]=EN, [6]=TORCH_MODE, [3]=LV_EN, [2:0]=LV_SEL_VOUT)
 *  - FLED_CUR1  = 0x14 ([7:4]=flash 100..1500mA, [3:0]=torch 20..300mA)
 *  - FLED_TIME1 = 0x15 ([7:4]=flash 62.5..1000ms, [3:0]=torch 1..16s)
 *  - LDO1..5:   ctrl_reg = 0x1e + (n - 1), bit 7 = enable, bits 6:0 = vsel, 600 mV + n * 12.5 mV
 *  - LDO6..18:  ctrl_reg = 0x1e + (n - 1), bit 7 = enable, bits 6:0 = vsel, 600 mV + n * 25.0 mV
 */

#include <linux/bits.h>
#include <linux/gpio/consumer.h>
#include <linux/i2c.h>
#include <linux/led-class-flash.h>
#include <linux/leds.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/regmap.h>
#include <linux/regulator/driver.h>
#include <linux/regulator/of_regulator.h>

#define S2MPB02_REG_ID		0x00
#define S2MPB02_REG_INT1	0x01
#define S2MPB02_REG_ST1		0x03
#define S2MPB02_REG_B1CTRL1	0x0b
#define S2MPB02_REG_B1CTRL2	0x0c
#define S2MPB02_REG_B2CTRL1	0x0d
#define S2MPB02_REG_B2CTRL2	0x0e
#define S2MPB02_REG_BB1CTRL1	0x0f
#define S2MPB02_REG_BB1CTRL2	0x10
#define S2MPB02_REG_FLED_CTRL1	0x12
#define S2MPB02_REG_FLED_CTRL2	0x13
#define S2MPB02_REG_FLED_CUR1	0x14
#define S2MPB02_REG_FLED_TIME1	0x15
#define S2MPB02_REG_FLED_CUR2	0x16
#define S2MPB02_REG_FLED_TIME2	0x17
#define S2MPB02_REG_L1CTRL	0x1e

#define S2MPB02_FLED_EN_MASK		0xc0
#define S2MPB02_FLED_FLASH_EN		0x80
#define S2MPB02_FLED_TORCH_EN		0xc0
#define S2MPB02_FLED_LV_EN_MASK		0x08
#define S2MPB02_FLED_LV_VOUT_MASK	0x07
#define S2MPB02_FLED_LV_3000MV		0x03
#define S2MPB02_FLED_FLASH_CUR_MASK	0xf0
#define S2MPB02_FLED_FLASH_CUR_SHIFT	4
#define S2MPB02_FLED_TORCH_CUR_MASK	0x0f
#define S2MPB02_FLED_FLASH_TIME_MASK	0xf0
#define S2MPB02_FLED_FLASH_TIME_SHIFT	4

#define S2MPB02_NUM_LDOS	18
#define S2MPB02_NUM_BUCKS	2
#define S2MPB02_NUM_BBS		1
#define S2MPB02_NUM_REGULATORS	(S2MPB02_NUM_LDOS + S2MPB02_NUM_BUCKS + S2MPB02_NUM_BBS)

#define S2MPB02_LDO_VSEL_MASK	0x7f
#define S2MPB02_LDO_ENABLE	BIT(7)
#define S2MPB02_LDO_MIN_UV	600000
#define S2MPB02_LDO_ENABLE_US	180

#define S2MPB02_BUCK_VSEL_MASK	0xff
#define S2MPB02_BUCK_ENABLE	0xc0
#define S2MPB02_BUCK_MIN_UV	400000
#define S2MPB02_BUCK_STEP_UV	6250
#define S2MPB02_BUCK_ENABLE_US	100

#define S2MPB02_BB_MIN_UV	2600000
#define S2MPB02_BB_STEP_UV	12500
#define S2MPB02_BB_ENABLE_US	210

enum s2mpb02_regulator_id {
	S2MPB02_LDO1 = 0,
	S2MPB02_BUCK1 = S2MPB02_NUM_LDOS,
	S2MPB02_BUCK2,
	S2MPB02_BB1,
};

struct s2mpb02_data {
	struct device *dev;
	struct regmap *regmap;
#if IS_ENABLED(CONFIG_LEDS_CLASS_FLASH)
	struct led_classdev_flash fled;
	struct gpio_desc *torch_gpio;
	struct mutex fled_lock;
	u8 flash_cur_sel;
	u8 flash_time_sel;
	u8 gpio_mode; /* 0 = I2C only, 1 = I2C + GPIO, 2 = GPIO only */
#endif
};

static const struct regulator_ops s2mpb02_reg_ops = {
	.list_voltage		= regulator_list_voltage_linear,
	.map_voltage		= regulator_map_voltage_linear,
	.get_voltage_sel	= regulator_get_voltage_sel_regmap,
	.set_voltage_sel	= regulator_set_voltage_sel_regmap,
	.enable			= regulator_enable_regmap,
	.disable		= regulator_disable_regmap,
	.is_enabled		= regulator_is_enabled_regmap,
};

#define S2MPB02_LDO(_n, _step) {					\
	.name		= "ldo" #_n,					\
	.of_match	= of_match_ptr("ldo" #_n),			\
	.regulators_node = of_match_ptr("regulators"),			\
	.id		= (_n) - 1,					\
	.ops		= &s2mpb02_reg_ops,				\
	.type		= REGULATOR_VOLTAGE,				\
	.owner		= THIS_MODULE,					\
	.min_uV		= S2MPB02_LDO_MIN_UV,				\
	.uV_step	= (_step),					\
	.n_voltages	= S2MPB02_LDO_VSEL_MASK + 1,			\
	.vsel_reg	= S2MPB02_REG_L1CTRL + (_n) - 1,		\
	.vsel_mask	= S2MPB02_LDO_VSEL_MASK,			\
	.enable_reg	= S2MPB02_REG_L1CTRL + (_n) - 1,		\
	.enable_mask	= S2MPB02_LDO_ENABLE,				\
	.enable_val	= S2MPB02_LDO_ENABLE,				\
	.enable_time	= S2MPB02_LDO_ENABLE_US,			\
}

#define S2MPB02_SWITCHER(_id, _name, _min, _step, _en_reg, _vs_reg, _en_us) { \
	.name		= (_name),					\
	.of_match	= of_match_ptr(_name),				\
	.regulators_node = of_match_ptr("regulators"),			\
	.id		= (_id),					\
	.ops		= &s2mpb02_reg_ops,				\
	.type		= REGULATOR_VOLTAGE,				\
	.owner		= THIS_MODULE,					\
	.min_uV		= (_min),					\
	.uV_step	= (_step),					\
	.n_voltages	= S2MPB02_BUCK_VSEL_MASK + 1,			\
	.vsel_reg	= (_vs_reg),					\
	.vsel_mask	= S2MPB02_BUCK_VSEL_MASK,			\
	.enable_reg	= (_en_reg),					\
	.enable_mask	= S2MPB02_BUCK_ENABLE,				\
	.enable_val	= S2MPB02_BUCK_ENABLE,				\
	.enable_time	= (_en_us),					\
}

static const struct regulator_desc s2mpb02_regulators[S2MPB02_NUM_REGULATORS] = {
	S2MPB02_LDO(1, 12500),  S2MPB02_LDO(2, 12500),  S2MPB02_LDO(3, 12500),
	S2MPB02_LDO(4, 12500),  S2MPB02_LDO(5, 12500),  S2MPB02_LDO(6, 25000),
	S2MPB02_LDO(7, 25000),  S2MPB02_LDO(8, 25000),  S2MPB02_LDO(9, 25000),
	S2MPB02_LDO(10, 25000), S2MPB02_LDO(11, 25000), S2MPB02_LDO(12, 25000),
	S2MPB02_LDO(13, 25000), S2MPB02_LDO(14, 25000), S2MPB02_LDO(15, 25000),
	S2MPB02_LDO(16, 25000), S2MPB02_LDO(17, 25000), S2MPB02_LDO(18, 25000),
	S2MPB02_SWITCHER(S2MPB02_BUCK1, "buck1", S2MPB02_BUCK_MIN_UV,
			 S2MPB02_BUCK_STEP_UV, S2MPB02_REG_B1CTRL1,
			 S2MPB02_REG_B1CTRL2, S2MPB02_BUCK_ENABLE_US),
	S2MPB02_SWITCHER(S2MPB02_BUCK2, "buck2", S2MPB02_BUCK_MIN_UV,
			 S2MPB02_BUCK_STEP_UV, S2MPB02_REG_B2CTRL1,
			 S2MPB02_REG_B2CTRL2, S2MPB02_BUCK_ENABLE_US),
	S2MPB02_SWITCHER(S2MPB02_BB1, "bb", S2MPB02_BB_MIN_UV,
			 S2MPB02_BB_STEP_UV, S2MPB02_REG_BB1CTRL1,
			 S2MPB02_REG_BB1CTRL2, S2MPB02_BB_ENABLE_US),
};

static const struct regmap_config s2mpb02_regmap_config = {
	.reg_bits	= 8,
	.val_bits	= 8,
	.max_register	= S2MPB02_REG_L1CTRL + S2MPB02_NUM_LDOS - 1,
};

#if IS_ENABLED(CONFIG_LEDS_CLASS_FLASH)
static struct s2mpb02_data *fled_to_s2mpb02(struct led_classdev_flash *fled)
{
	return container_of(fled, struct s2mpb02_data, fled);
}

static int s2mpb02_torch_brightness_set(struct led_classdev *cdev,
					enum led_brightness value)
{
	struct led_classdev_flash *fled = lcdev_to_flcdev(cdev);
	struct s2mpb02_data *priv = fled_to_s2mpb02(fled);
	u8 lvl = min_t(unsigned int, value, 15);
	int ret = 0;

	mutex_lock(&priv->fled_lock);

	if (lvl == 0) {
		ret = regmap_update_bits(priv->regmap, S2MPB02_REG_FLED_CTRL1,
					 S2MPB02_FLED_EN_MASK, 0x00);
		if (priv->torch_gpio)
			gpiod_set_value_cansleep(priv->torch_gpio, 0);
	} else {
		ret = regmap_update_bits(priv->regmap, S2MPB02_REG_FLED_CUR1,
					 S2MPB02_FLED_TORCH_CUR_MASK, lvl);
		if (ret)
			goto out;

		if (priv->gpio_mode == 2) {
			ret = regmap_update_bits(priv->regmap, S2MPB02_REG_FLED_CTRL1,
						 S2MPB02_FLED_EN_MASK, 0x00);
		} else {
			ret = regmap_update_bits(priv->regmap, S2MPB02_REG_FLED_CTRL1,
						 S2MPB02_FLED_EN_MASK,
						 S2MPB02_FLED_TORCH_EN);
		}
		if (priv->torch_gpio && priv->gpio_mode != 0)
			gpiod_set_value_cansleep(priv->torch_gpio, 1);
	}

out:
	mutex_unlock(&priv->fled_lock);
	return ret;
}

static int s2mpb02_flash_brightness_set(struct led_classdev_flash *fled,
					u32 brightness)
{
	struct s2mpb02_data *priv = fled_to_s2mpb02(fled);
	u8 sel = clamp_t(u32, DIV_ROUND_CLOSEST(brightness, 100000), 1, 15);
	int ret;

	mutex_lock(&priv->fled_lock);
	priv->flash_cur_sel = sel;
	ret = regmap_update_bits(priv->regmap, S2MPB02_REG_FLED_CUR1,
				 S2MPB02_FLED_FLASH_CUR_MASK,
				 sel << S2MPB02_FLED_FLASH_CUR_SHIFT);
	mutex_unlock(&priv->fled_lock);

	return ret;
}

static int s2mpb02_flash_timeout_set(struct led_classdev_flash *fled,
				     u32 timeout)
{
	struct s2mpb02_data *priv = fled_to_s2mpb02(fled);
	u8 sel = clamp_t(u32, DIV_ROUND_CLOSEST(timeout, 62500), 1, 16) - 1;
	int ret;

	mutex_lock(&priv->fled_lock);
	priv->flash_time_sel = sel;
	ret = regmap_update_bits(priv->regmap, S2MPB02_REG_FLED_TIME1,
				 S2MPB02_FLED_FLASH_TIME_MASK,
				 sel << S2MPB02_FLED_FLASH_TIME_SHIFT);
	mutex_unlock(&priv->fled_lock);

	return ret;
}

static int s2mpb02_flash_strobe_set(struct led_classdev_flash *fled, bool state)
{
	struct s2mpb02_data *priv = fled_to_s2mpb02(fled);
	int ret;

	mutex_lock(&priv->fled_lock);
	if (state) {
		ret = regmap_update_bits(priv->regmap, S2MPB02_REG_FLED_CUR1,
					 S2MPB02_FLED_FLASH_CUR_MASK,
					 priv->flash_cur_sel << S2MPB02_FLED_FLASH_CUR_SHIFT);
		if (ret)
			goto out;
		ret = regmap_update_bits(priv->regmap, S2MPB02_REG_FLED_TIME1,
					 S2MPB02_FLED_FLASH_TIME_MASK,
					 priv->flash_time_sel << S2MPB02_FLED_FLASH_TIME_SHIFT);
		if (ret)
			goto out;
		ret = regmap_update_bits(priv->regmap, S2MPB02_REG_FLED_CTRL1,
					 S2MPB02_FLED_EN_MASK,
					 S2MPB02_FLED_FLASH_EN);
		if (priv->torch_gpio && priv->gpio_mode != 0)
			gpiod_set_value_cansleep(priv->torch_gpio, 1);
	} else {
		ret = regmap_update_bits(priv->regmap, S2MPB02_REG_FLED_CTRL1,
					 S2MPB02_FLED_EN_MASK, 0x00);
		if (priv->torch_gpio)
			gpiod_set_value_cansleep(priv->torch_gpio, 0);
	}
out:
	mutex_unlock(&priv->fled_lock);
	return ret;
}

static int s2mpb02_flash_strobe_get(struct led_classdev_flash *fled, bool *state)
{
	struct s2mpb02_data *priv = fled_to_s2mpb02(fled);
	unsigned int val = 0;
	int ret;

	ret = regmap_read(priv->regmap, S2MPB02_REG_FLED_CTRL1, &val);
	if (ret)
		return ret;

	*state = ((val & S2MPB02_FLED_EN_MASK) == S2MPB02_FLED_FLASH_EN);
	return 0;
}

static int s2mpb02_flash_fault_get(struct led_classdev_flash *fled, u32 *fault)
{
	struct s2mpb02_data *priv = fled_to_s2mpb02(fled);
	unsigned int st1 = 0;

	regmap_read(priv->regmap, S2MPB02_REG_ST1, &st1);
	*fault = 0;
	return 0;
}

static const struct led_flash_ops s2mpb02_flash_ops = {
	.flash_brightness_set	= s2mpb02_flash_brightness_set,
	.strobe_set		= s2mpb02_flash_strobe_set,
	.strobe_get		= s2mpb02_flash_strobe_get,
	.timeout_set		= s2mpb02_flash_timeout_set,
	.fault_get		= s2mpb02_flash_fault_get,
};

static ssize_t gpio_mode_show(struct device *dev, struct device_attribute *attr,
			      char *buf)
{
	struct s2mpb02_data *priv = dev_get_drvdata(dev);

	return sysfs_emit(buf, "%u\n", priv->gpio_mode);
}

static ssize_t gpio_mode_store(struct device *dev, struct device_attribute *attr,
			       const char *buf, size_t count)
{
	struct s2mpb02_data *priv = dev_get_drvdata(dev);
	u8 mode;

	if (kstrtou8(buf, 0, &mode) || mode > 2)
		return -EINVAL;

	mutex_lock(&priv->fled_lock);
	priv->gpio_mode = mode;
	mutex_unlock(&priv->fled_lock);

	return count;
}
static DEVICE_ATTR_RW(gpio_mode);

static ssize_t fled_regs_show(struct device *dev, struct device_attribute *attr,
			      char *buf)
{
	struct s2mpb02_data *priv = dev_get_drvdata(dev);
	unsigned int r[6] = { 0 }, bst1 = 0;
	int i;

	for (i = 0; i < 6; i++)
		regmap_read(priv->regmap, S2MPB02_REG_FLED_CTRL1 + i, &r[i]);
	regmap_read(priv->regmap, 0x1a, &bst1);

	return sysfs_emit(buf,
			  "ctrl1=0x%02x ctrl2=0x%02x cur1=0x%02x time1=0x%02x bst1=0x%02x\n",
			  r[0], r[1], r[2], r[3], bst1);
}
static DEVICE_ATTR_RO(fled_regs);

static struct attribute *s2mpb02_fled_attrs[] = {
	&dev_attr_gpio_mode.attr,
	&dev_attr_fled_regs.attr,
	NULL,
};
static const struct attribute_group s2mpb02_fled_group = {
	.attrs = s2mpb02_fled_attrs,
};

static int s2mpb02_fled_init(struct s2mpb02_data *priv)
{
	struct led_classdev *cdev = &priv->fled.led_cdev;
	struct led_flash_setting *s;
	int ret;

	mutex_init(&priv->fled_lock);
	priv->flash_cur_sel = 10; /* 1000 mA default */
	priv->flash_time_sel = 3; /* 250 ms default */
	priv->gpio_mode = 1;      /* I2C + optional torch GPIO */

	priv->torch_gpio = devm_gpiod_get_optional(priv->dev, "torch",
						   GPIOD_OUT_LOW);
	if (IS_ERR(priv->torch_gpio))
		return dev_err_probe(priv->dev, PTR_ERR(priv->torch_gpio),
				     "failed to request torch GPIO\n");

	/* Stock Samsung setup: disable LV mode, min VOUT = 3.0 V, LED off */
	regmap_update_bits(priv->regmap, S2MPB02_REG_FLED_CTRL1,
			   S2MPB02_FLED_EN_MASK | S2MPB02_FLED_LV_EN_MASK |
			   S2MPB02_FLED_LV_VOUT_MASK,
			   S2MPB02_FLED_LV_3000MV);
	regmap_write(priv->regmap, S2MPB02_REG_FLED_CUR1,
		     (priv->flash_cur_sel << S2MPB02_FLED_FLASH_CUR_SHIFT) | 5);
	regmap_write(priv->regmap, S2MPB02_REG_FLED_TIME1,
		     (priv->flash_time_sel << S2MPB02_FLED_FLASH_TIME_SHIFT) | 3);

	priv->fled.ops = &s2mpb02_flash_ops;

	s = &priv->fled.brightness;
	s->min = 100000;
	s->max = 1500000;
	s->step = 100000;
	s->val = 1000000;

	s = &priv->fled.timeout;
	s->min = 62500;
	s->max = 1000000;
	s->step = 62500;
	s->val = 250000;

	cdev->name = "s2mpb02:white:flash";
	cdev->max_brightness = 15;
	cdev->brightness_set_blocking = s2mpb02_torch_brightness_set;
	cdev->flags = LED_DEV_CAP_FLASH | LED_CORE_SUSPENDRESUME;
	
	ret = devm_device_add_group(priv->dev, &s2mpb02_fled_group);
	if (ret)
		dev_warn(priv->dev, "failed to add fled sysfs: %d\n", ret);
	ret = devm_led_classdev_flash_register_ext(priv->dev, &priv->fled, NULL);
	if (ret)
		dev_warn(priv->dev, "failed to register FLED flash device: %d\n", ret);
	else
		dev_info(priv->dev, "S2MPB02 FLED registered (%s, torch_gpio=%s)\n",
			 cdev->name, priv->torch_gpio ? "yes" : "no");

	return 0;
}
#endif

static int s2mpb02_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	struct regulator_config config = { };
	struct s2mpb02_data *priv;
	unsigned int id;
	int i, ret;

	priv = devm_kzalloc(dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	priv->dev = dev;
	priv->regmap = devm_regmap_init_i2c(client, &s2mpb02_regmap_config);
	if (IS_ERR(priv->regmap))
		return dev_err_probe(dev, PTR_ERR(priv->regmap), "regmap init failed\n");

	i2c_set_clientdata(client, priv);

	ret = regmap_read(priv->regmap, S2MPB02_REG_ID, &id);
	if (ret)
		return dev_err_probe(dev, ret, "cannot read chip ID\n");

	config.dev = dev;
	config.regmap = priv->regmap;

	for (i = 0; i < S2MPB02_NUM_REGULATORS; i++) {
		struct regulator_dev *rdev;

		rdev = devm_regulator_register(dev, &s2mpb02_regulators[i], &config);
		if (IS_ERR(rdev))
			return dev_err_probe(dev, PTR_ERR(rdev), "cannot register %s\n",
					     s2mpb02_regulators[i].name);
	}

#if IS_ENABLED(CONFIG_LEDS_CLASS_FLASH)
	ret = s2mpb02_fled_init(priv);
	if (ret)
		return ret;
#endif

	dev_info(dev, "S2MPB02 camera PMIC, ID 0x%02x (%d regulators)\n",
		 id, S2MPB02_NUM_REGULATORS);

	return 0;
}

static const struct of_device_id s2mpb02_of_match[] = {
	{ .compatible = "samsung,s2mpb02" },
	{ }
};
MODULE_DEVICE_TABLE(of, s2mpb02_of_match);

static struct i2c_driver s2mpb02_driver = {
	.driver = {
		.name = "s2mpb02-regulator",
		.of_match_table = s2mpb02_of_match,
	},
	.probe = s2mpb02_probe,
};
module_i2c_driver(s2mpb02_driver);

MODULE_DESCRIPTION("Samsung S2MPB02 camera PMIC regulators and FLED flash/torch");
MODULE_LICENSE("GPL");
