// SPDX-License-Identifier: GPL-2.0-only
/*
 * Qualcomm 3D Sonic Gen 2 (QBT2608 / QBT2000) Ultrasonic Fingerprint
 * Linux Hardware & Wake-Up Host Biometric (WUHB) Driver for Galaxy S22 (r0q).
 *
 * Architecture note:
 * On SM8450 (Galaxy S22), the QBT2608 ultrasonic ASIC's high-speed SPI bus is
 * routed exclusively to Qualcomm TrustZone (QSEE TA 'qbt1000', invoked via
 * smcinvoke in stock Android). Linux owns:
 *  - VDD_BTP_1P8 (pm8350c_l6, 1.8 V power rail — kept OFF in idle for 0 mW drain)
 *  - WUHB GPIO (tlmm gpio89, Wake-Up Host Biometric ultrasonic finger-detect IRQ)
 *  - IPC GPIO  (tlmm gpio88, TZ <-> HLOS biometric notification IRQ)
 */

#include <linux/atomic.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/fs.h>
#include <linux/gpio/consumer.h>
#include <linux/input.h>
#include <linux/interrupt.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/poll.h>
#include <linux/regulator/consumer.h>
#include <linux/uaccess.h>
#include <linux/wait.h>

#define QBT2000_IOCTL_MAGIC		'Q'
#define QBT2000_ENABLE_IPC		_IO(QBT2000_IOCTL_MAGIC, 101)
#define QBT2000_DISABLE_IPC		_IO(QBT2000_IOCTL_MAGIC, 102)
#define QBT2000_POWER_CONTROL		_IOW(QBT2000_IOCTL_MAGIC, 103, int)
#define QBT2000_ENABLE_WUHB		_IO(QBT2000_IOCTL_MAGIC, 104)
#define QBT2000_DISABLE_WUHB		_IO(QBT2000_IOCTL_MAGIC, 105)
#define QBT2000_IS_WHUB_CONNECTED	_IOR(QBT2000_IOCTL_MAGIC, 106, int)

struct qbt2000_drvdata {
	struct device *dev;
	struct class *fp_class;
	struct device *fp_dev;
	struct regulator *vdd;
	struct gpio_desc *ipc_gpio;
	struct gpio_desc *wuhb_gpio;
	struct input_dev *input;
	struct miscdevice misc;
	struct mutex lock;
	wait_queue_head_t read_wq;

	int ipc_irq;
	int wuhb_irq;
	bool power_enabled;
	bool ipc_enabled;
	bool wuhb_enabled;
	atomic_t ipc_count;
	atomic_t wuhb_count;
	atomic_t pending_events;

	const char *chipid;
	const char *modelinfo;
	const char *position;
};

static irqreturn_t qbt2000_wuhb_isr(int irq, void *dev_id)
{
	struct qbt2000_drvdata *drv = dev_id;
	int val = drv->wuhb_gpio ? gpiod_get_value_cansleep(drv->wuhb_gpio) : 0;

	atomic_inc(&drv->wuhb_count);
	atomic_inc(&drv->pending_events);
	pm_wakeup_event(drv->dev, 1000);

	if (drv->input) {
		input_report_key(drv->input, BTN_TOUCH, val > 0 ? 1 : 0);
		if (val > 0) {
			input_report_key(drv->input, KEY_WAKEUP, 1);
			input_sync(drv->input);
			input_report_key(drv->input, KEY_WAKEUP, 0);
		}
		input_sync(drv->input);
	}

	wake_up_interruptible(&drv->read_wq);
	return IRQ_HANDLED;
}

static irqreturn_t qbt2000_ipc_isr(int irq, void *dev_id)
{
	struct qbt2000_drvdata *drv = dev_id;

	atomic_inc(&drv->ipc_count);
	atomic_inc(&drv->pending_events);
	pm_wakeup_event(drv->dev, 500);
	wake_up_interruptible(&drv->read_wq);
	return IRQ_HANDLED;
}

static int qbt2000_set_power(struct qbt2000_drvdata *drv, bool on)
{
	int ret = 0;

	if (drv->power_enabled == on)
		return 0;

	if (on) {
		ret = regulator_enable(drv->vdd);
		if (ret)
			return ret;
		usleep_range(1000, 1500);
		drv->power_enabled = true;
	} else {
		if (drv->wuhb_enabled && drv->wuhb_irq > 0) {
			disable_irq_wake(drv->wuhb_irq);
			disable_irq(drv->wuhb_irq);
			drv->wuhb_enabled = false;
		}
		if (drv->ipc_enabled && drv->ipc_irq > 0) {
			disable_irq(drv->ipc_irq);
			drv->ipc_enabled = false;
		}
		ret = regulator_disable(drv->vdd);
		drv->power_enabled = false;
	}
	return ret;
}

static int qbt2000_set_wuhb(struct qbt2000_drvdata *drv, bool on)
{
	int ret;

	if (drv->wuhb_irq <= 0)
		return -ENODEV;
	if (drv->wuhb_enabled == on)
		return 0;

	if (on) {
		if (!drv->power_enabled) {
			ret = qbt2000_set_power(drv, true);
			if (ret)
				return ret;
		}
		enable_irq(drv->wuhb_irq);
		enable_irq_wake(drv->wuhb_irq);
		drv->wuhb_enabled = true;
	} else {
		disable_irq_wake(drv->wuhb_irq);
		disable_irq(drv->wuhb_irq);
		drv->wuhb_enabled = false;
	}
	return 0;
}

static int qbt2000_set_ipc(struct qbt2000_drvdata *drv, bool on)
{
	int ret;

	if (drv->ipc_irq <= 0)
		return -ENODEV;
	if (drv->ipc_enabled == on)
		return 0;

	if (on) {
		if (!drv->power_enabled) {
			ret = qbt2000_set_power(drv, true);
			if (ret)
				return ret;
		}
		enable_irq(drv->ipc_irq);
		drv->ipc_enabled = true;
	} else {
		disable_irq(drv->ipc_irq);
		drv->ipc_enabled = false;
	}
	return 0;
}

static ssize_t qbt2000_read(struct file *filp, char __user *ubuf,
			    size_t cnt, loff_t *ppos)
{
	struct qbt2000_drvdata *drv = container_of(filp->private_data,
						   struct qbt2000_drvdata, misc);
	u32 ev;

	if (cnt < sizeof(ev))
		return -EINVAL;

	if (!atomic_read(&drv->pending_events)) {
		if (filp->f_flags & O_NONBLOCK)
			return -EAGAIN;
		if (wait_event_interruptible(drv->read_wq,
					     atomic_read(&drv->pending_events) > 0))
			return -ERESTARTSYS;
	}

	ev = atomic_xchg(&drv->pending_events, 0);
	if (copy_to_user(ubuf, &ev, sizeof(ev)))
		return -EFAULT;

	return sizeof(ev);
}

static __poll_t qbt2000_poll(struct file *filp, poll_table *wait)
{
	struct qbt2000_drvdata *drv = container_of(filp->private_data,
						   struct qbt2000_drvdata, misc);

	poll_wait(filp, &drv->read_wq, wait);
	return atomic_read(&drv->pending_events) > 0 ? (EPOLLIN | EPOLLRDNORM) : 0;
}

static long qbt2000_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
	struct qbt2000_drvdata *drv = container_of(filp->private_data,
						   struct qbt2000_drvdata, misc);
	int val = 0, ret = 0;

	mutex_lock(&drv->lock);
	switch (cmd) {
	case QBT2000_POWER_CONTROL:
		if (copy_from_user(&val, (void __user *)arg, sizeof(val)))
			ret = -EFAULT;
		else
			ret = qbt2000_set_power(drv, !!val);
		break;
	case QBT2000_ENABLE_IPC:
		ret = qbt2000_set_ipc(drv, true);
		break;
	case QBT2000_DISABLE_IPC:
		ret = qbt2000_set_ipc(drv, false);
		break;
	case QBT2000_ENABLE_WUHB:
		ret = qbt2000_set_wuhb(drv, true);
		break;
	case QBT2000_DISABLE_WUHB:
		ret = qbt2000_set_wuhb(drv, false);
		break;
	case QBT2000_IS_WHUB_CONNECTED:
		val = drv->wuhb_gpio ? 1 : 0;
		if (copy_to_user((void __user *)arg, &val, sizeof(val)))
			ret = -EFAULT;
		break;
	default:
		ret = -ENOTTY;
		break;
	}
	mutex_unlock(&drv->lock);
	return ret;
}

static const struct file_operations qbt2000_fops = {
	.owner		= THIS_MODULE,
	.read		= qbt2000_read,
	.poll		= qbt2000_poll,
	.unlocked_ioctl	= qbt2000_ioctl,
	.compat_ioctl	= compat_ptr_ioctl,
};

static ssize_t name_show(struct device *dev, struct device_attribute *a, char *buf)
{
	struct qbt2000_drvdata *drv = dev_get_drvdata(dev);
	return sysfs_emit(buf, "%s\n", drv->chipid);
}
static DEVICE_ATTR_RO(name);

static ssize_t model_show(struct device *dev, struct device_attribute *a, char *buf)
{
	struct qbt2000_drvdata *drv = dev_get_drvdata(dev);
	return sysfs_emit(buf, "%s\n", drv->modelinfo);
}
static DEVICE_ATTR_RO(model);

static ssize_t position_show(struct device *dev, struct device_attribute *a, char *buf)
{
	struct qbt2000_drvdata *drv = dev_get_drvdata(dev);
	return sysfs_emit(buf, "%s\n", drv->position);
}
static DEVICE_ATTR_RO(position);

static ssize_t power_enable_show(struct device *dev, struct device_attribute *a, char *buf)
{
	struct qbt2000_drvdata *drv = dev_get_drvdata(dev);
	return sysfs_emit(buf, "%d\n", drv->power_enabled ? 1 : 0);
}

static ssize_t power_enable_store(struct device *dev, struct device_attribute *a,
				  const char *buf, size_t count)
{
	struct qbt2000_drvdata *drv = dev_get_drvdata(dev);
	bool on;
	int ret;

	if (kstrtobool(buf, &on))
		return -EINVAL;
	mutex_lock(&drv->lock);
	ret = qbt2000_set_power(drv, on);
	mutex_unlock(&drv->lock);
	return ret ? ret : count;
}
static DEVICE_ATTR_RW(power_enable);

static ssize_t wuhb_enable_show(struct device *dev, struct device_attribute *a, char *buf)
{
	struct qbt2000_drvdata *drv = dev_get_drvdata(dev);
	return sysfs_emit(buf, "%d\n", drv->wuhb_enabled ? 1 : 0);
}

static ssize_t wuhb_enable_store(struct device *dev, struct device_attribute *a,
				 const char *buf, size_t count)
{
	struct qbt2000_drvdata *drv = dev_get_drvdata(dev);
	bool on;
	int ret;

	if (kstrtobool(buf, &on))
		return -EINVAL;
	mutex_lock(&drv->lock);
	ret = qbt2000_set_wuhb(drv, on);
	mutex_unlock(&drv->lock);
	return ret ? ret : count;
}
static DEVICE_ATTR_RW(wuhb_enable);

static ssize_t ipc_enable_show(struct device *dev, struct device_attribute *a, char *buf)
{
	struct qbt2000_drvdata *drv = dev_get_drvdata(dev);
	return sysfs_emit(buf, "%d\n", drv->ipc_enabled ? 1 : 0);
}

static ssize_t ipc_enable_store(struct device *dev, struct device_attribute *a,
				const char *buf, size_t count)
{
	struct qbt2000_drvdata *drv = dev_get_drvdata(dev);
	bool on;
	int ret;

	if (kstrtobool(buf, &on))
		return -EINVAL;
	mutex_lock(&drv->lock);
	ret = qbt2000_set_ipc(drv, on);
	mutex_unlock(&drv->lock);
	return ret ? ret : count;
}
static DEVICE_ATTR_RW(ipc_enable);

static ssize_t wuhb_count_show(struct device *dev, struct device_attribute *a, char *buf)
{
	struct qbt2000_drvdata *drv = dev_get_drvdata(dev);
	return sysfs_emit(buf, "%d\n", atomic_read(&drv->wuhb_count));
}
static DEVICE_ATTR_RO(wuhb_count);

static ssize_t ipc_count_show(struct device *dev, struct device_attribute *a, char *buf)
{
	struct qbt2000_drvdata *drv = dev_get_drvdata(dev);
	return sysfs_emit(buf, "%d\n", atomic_read(&drv->ipc_count));
}
static DEVICE_ATTR_RO(ipc_count);

static ssize_t gpio_state_show(struct device *dev, struct device_attribute *a, char *buf)
{
	struct qbt2000_drvdata *drv = dev_get_drvdata(dev);
	int ipc = drv->ipc_gpio ? gpiod_get_value_cansleep(drv->ipc_gpio) : -1;
	int wuhb = drv->wuhb_gpio ? gpiod_get_value_cansleep(drv->wuhb_gpio) : -1;

	return sysfs_emit(buf, "ipc_gpio=%d wuhb_gpio=%d power=%d\n",
			  ipc, wuhb, drv->power_enabled ? 1 : 0);
}
static DEVICE_ATTR_RO(gpio_state);

static ssize_t tz_status_show(struct device *dev, struct device_attribute *a, char *buf)
{
	return sysfs_emit(buf,
		"Qualcomm 3D Sonic Gen 2 (QBT2608): ultrasonic ASIC SPI bus is TrustZone-owned "
		"(QSEE TA 'qbt1000', requires smcinvoke + proprietary TZ biometric HAL). "
		"Linux controls VDD_BTP_1P8 (pm8350c_l6), WUHB finger-detect IRQ (gpio89), "
		"and TZ IPC IRQ (gpio88).\n");
}
static DEVICE_ATTR_RO(tz_status);

static struct attribute *qbt2000_attrs[] = {
	&dev_attr_name.attr,
	&dev_attr_model.attr,
	&dev_attr_position.attr,
	&dev_attr_power_enable.attr,
	&dev_attr_wuhb_enable.attr,
	&dev_attr_ipc_enable.attr,
	&dev_attr_wuhb_count.attr,
	&dev_attr_ipc_count.attr,
	&dev_attr_gpio_state.attr,
	&dev_attr_tz_status.attr,
	NULL,
};
ATTRIBUTE_GROUPS(qbt2000);

static int qbt2000_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct qbt2000_drvdata *drv;
	int ret;

	drv = devm_kzalloc(dev, sizeof(*drv), GFP_KERNEL);
	if (!drv)
		return -ENOMEM;

	drv->dev = dev;
	mutex_init(&drv->lock);
	init_waitqueue_head(&drv->read_wq);
	platform_set_drvdata(pdev, drv);

	if (of_property_read_string(dev->of_node, "qcom,chipid", &drv->chipid))
		drv->chipid = "QBT2608";
	if (of_property_read_string(dev->of_node, "qcom,modelinfo", &drv->modelinfo))
		drv->modelinfo = "S90X";
	if (of_property_read_string(dev->of_node, "qcom,position", &drv->position))
		drv->position = "31.17,0.00,7.50,8.25,14.80,14.80,13.00,13.00,5.00";

	drv->vdd = devm_regulator_get(dev, "vdd");
	if (IS_ERR(drv->vdd))
		return dev_err_probe(dev, PTR_ERR(drv->vdd), "failed to get vdd\n");

	drv->ipc_gpio = devm_gpiod_get_optional(dev, "ipc", GPIOD_IN);
	if (IS_ERR(drv->ipc_gpio))
		return dev_err_probe(dev, PTR_ERR(drv->ipc_gpio), "failed to get ipc gpio\n");

	drv->wuhb_gpio = devm_gpiod_get_optional(dev, "wuhb", GPIOD_IN);
	if (IS_ERR(drv->wuhb_gpio))
		return dev_err_probe(dev, PTR_ERR(drv->wuhb_gpio), "failed to get wuhb gpio\n");

	drv->input = devm_input_allocate_device(dev);
	if (!drv->input)
		return -ENOMEM;
	drv->input->name = "qbt2000_key_input";
	drv->input->id.bustype = BUS_HOST;
	input_set_capability(drv->input, EV_KEY, KEY_WAKEUP);
	input_set_capability(drv->input, EV_KEY, BTN_TOUCH);
	ret = input_register_device(drv->input);
	if (ret)
		return ret;

	if (drv->wuhb_gpio) {
		drv->wuhb_irq = gpiod_to_irq(drv->wuhb_gpio);
		if (drv->wuhb_irq > 0) {
			irq_set_status_flags(drv->wuhb_irq, IRQ_NOAUTOEN);
			ret = devm_request_threaded_irq(dev, drv->wuhb_irq, NULL,
							qbt2000_wuhb_isr,
							IRQF_TRIGGER_RISING |
							IRQF_TRIGGER_FALLING |
							IRQF_ONESHOT,
							"qbt2000_wuhb", drv);
			if (ret)
				return dev_err_probe(dev, ret, "wuhb irq failed\n");
		}
	}

	if (drv->ipc_gpio) {
		drv->ipc_irq = gpiod_to_irq(drv->ipc_gpio);
		if (drv->ipc_irq > 0) {
			irq_set_status_flags(drv->ipc_irq, IRQ_NOAUTOEN);
			ret = devm_request_threaded_irq(dev, drv->ipc_irq, NULL,
							qbt2000_ipc_isr,
							IRQF_TRIGGER_FALLING |
							IRQF_ONESHOT,
							"qbt2000_ipc", drv);
			if (ret)
				return dev_err_probe(dev, ret, "ipc irq failed\n");
		}
	}

	device_init_wakeup(dev, true);

	drv->misc.minor = MISC_DYNAMIC_MINOR;
	drv->misc.name = "qbt2000_ipc";
	drv->misc.fops = &qbt2000_fops;
	ret = misc_register(&drv->misc);
	if (ret)
		return ret;

	drv->fp_class = class_create("fingerprint");
	if (IS_ERR(drv->fp_class)) {
		ret = PTR_ERR(drv->fp_class);
		misc_deregister(&drv->misc);
		return ret;
	}

	drv->fp_dev = device_create_with_groups(drv->fp_class, dev, MKDEV(0, 0),
						drv, qbt2000_groups,
						"fingerprint");
	if (IS_ERR(drv->fp_dev)) {
		ret = PTR_ERR(drv->fp_dev);
		class_destroy(drv->fp_class);
		misc_deregister(&drv->misc);
		return ret;
	}

	dev_info(dev, "%s (%s) registered: VDD_BTP_1P8 off in idle, wuhb_irq=%d ipc_irq=%d\n",
		 drv->chipid, drv->modelinfo, drv->wuhb_irq, drv->ipc_irq);
	return 0;
}

static void qbt2000_remove(struct platform_device *pdev)
{
	struct qbt2000_drvdata *drv = platform_get_drvdata(pdev);

	mutex_lock(&drv->lock);
	qbt2000_set_power(drv, false);
	mutex_unlock(&drv->lock);

	device_unregister(drv->fp_dev);
	class_destroy(drv->fp_class);
	misc_deregister(&drv->misc);
}

static const struct of_device_id qbt2000_of_match[] = {
	{ .compatible = "qcom,qbt2000" },
	{ }
};
MODULE_DEVICE_TABLE(of, qbt2000_of_match);

static struct platform_driver qbt2000_driver = {
	.probe	= qbt2000_probe,
	.remove	= qbt2000_remove,
	.driver	= {
		.name		= "qbt2000-r0q",
		.of_match_table	= qbt2000_of_match,
	},
};
module_platform_driver(qbt2000_driver);

MODULE_DESCRIPTION("Qualcomm 3D Sonic Gen 2 (QBT2608/QBT2000) Linux bridge for Galaxy S22");
MODULE_LICENSE("GPL");
