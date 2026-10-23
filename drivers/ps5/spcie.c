// SPDX-License-Identifier: GPL-2.0-only

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/module.h>
#include <linux/miscdevice.h>
#include <linux/input.h>
#include <linux/pci.h>
#include <linux/ps5.h>
#include <linux/dmi.h>
#include <linux/delay.h>
#include <linux/kfifo.h>

#define PCI_DEVICE_ID_SPCIE	0x9107
#define SPCIE_SUBFUNC_ICC	12
#define NUM_IRQS		16

#define ICC_QUERY_OFFSET	0
#define ICC_REPLY_OFFSET	0x800

#define ICC_DOORBELL_OFFSET	0x108000

#define ICC_REG_SOW		0x7f0
#define ICC_REG_SOR		0x7f4
#define ICC_REG_EMW		0xff0
#define ICC_REG_EMR		0xff4

#define ICC_REG_DOORBELL	0x04
#define ICC_REG_INTR_STATUS	0x14
#define ICC_REG_INTR_MASK	0x24

#define ICC_SEND		0x01
#define ICC_ACK			0x02

#define ICC_MSG_TYPE_REPLY	0x4000
#define ICC_MSG_TYPE_NOTIF	0x8000

#define ICC_TIMEOUT_MSECS	12000

#define ICC_IOC_MAGIC		'I'

#define ICC_FAN_CHANGE_SERVO_PATTERN	_IOW(ICC_IOC_MAGIC, 1, u8)

struct spcie_dev;

struct spcie_icc_dev {
	struct pci_dev *pdev;
	struct spcie_dev *sdev;
	struct input_dev *idev;
	struct mutex lock;
	wait_queue_head_t wq;
	void __iomem *icc_doorbell_base;
	void __iomem *icc_base;
	u16 icc_send_xtn_id;
	u8 notification[ICC_MSG_MAX_SIZE];
	u16 notification_length;
	u8 reply[ICC_MSG_MAX_SIZE];
	u16 reply_length;
	bool reply_ready;
};

struct spcie_dev {
	struct pci_dev *pdev;
	struct spcie_icc_dev *icc_dev;
	void __iomem *bar2;
	void __iomem *pervasive0;
};

struct icc_notification {
	u8 service_id;
	u8 notification[ICC_MSG_MAX_SIZE];
};

#define ICC_NOTIFICATION_FIFO_SIZE 32
static DEFINE_KFIFO(icc_notification_fifo, struct icc_notification, ICC_NOTIFICATION_FIFO_SIZE);
static DEFINE_SPINLOCK(icc_notification_lock);
static struct work_struct icc_notification_work;

static struct spcie_dev *sdev;
static struct spcie_icc_dev *icc_dev;

static bool spcie_initialized = false;
static u32 spcie_chip_revision_id = 0;

static const u8 indicator_white_dim[] = {
	0x03, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x02, 0x00, 0x11,
	0x01, 0x00, 0x20, 0x00, 0x01, 0x00, 0x12, 0x01, 0x00, 0x00, 0x00, 0x02,
	0x00
};

static const u8 indicator_white_medium[] = {
	0x03, 0x00, 0x00, 0x00, 0x10, 0x01, 0x00, 0x00, 0x00, 0x02, 0x00, 0x11,
	0x01, 0x00, 0x5F, 0x00, 0x01, 0x00, 0x12, 0x01, 0x00, 0x00, 0x00, 0x02,
	0x00
};

static const u8 indicator_white_bright[] = {
	0x03, 0x00, 0x00, 0x00, 0x10, 0x01, 0x00, 0x00, 0x00, 0x02, 0x00, 0x11,
	0x01, 0x00, 0xFF, 0x00, 0x01, 0x00, 0x12, 0x01, 0x00, 0x00, 0x00, 0x02,
	0x00
};

static u16 icc_checksum(struct icc_msg *msg)
{
	u16 i;
	u16 checksum = 0;
	for (i = 0; i < msg->length; i++)
		checksum += ((u8 *)msg)[i];
	return checksum;
}

static int icc_send(u8 *query)
{
	struct icc_msg *msg;

	msg = (struct icc_msg *)query;
	if (msg->length < ICC_MSG_MIN_SIZE)
		msg->length = ICC_MSG_MIN_SIZE;
	msg->magic = 0x42;
	msg->unk_04 = 3;
	msg->id = icc_dev->icc_send_xtn_id++;
	msg->checksum = icc_checksum(msg);

	writew(0, icc_dev->icc_base + ICC_REG_SOR);

	for (u16 i = 0; i < msg->length; i++)
		writeb(query[i], icc_dev->icc_base + ICC_QUERY_OFFSET + i);

	writew(1, icc_dev->icc_base + ICC_REG_SOW);

	writel(ICC_SEND, icc_dev->icc_doorbell_base + ICC_REG_DOORBELL);

	return 0;
}

int icc_query(u8 *query, u8 *reply)
{
	struct icc_msg *msg;
	u16 checksum, expected;
	int ret;

	mutex_lock(&icc_dev->lock);

	icc_dev->reply_ready = false;

	icc_send(query);

	ret = wait_event_timeout(icc_dev->wq, icc_dev->reply_ready,
		msecs_to_jiffies(ICC_TIMEOUT_MSECS));
	if (ret == 0) {
		dev_err(&icc_dev->pdev->dev, "timeout\n");
		mutex_unlock(&icc_dev->lock);
		return -ETIMEDOUT;
	}

	memcpy(reply, icc_dev->reply, icc_dev->reply_length);

	msg = (struct icc_msg *)reply;
	expected = msg->checksum;
	msg->checksum = 0;
	checksum = icc_checksum(msg);
	if (checksum != expected) {
		dev_err(&icc_dev->pdev->dev, "checksum mismatch\n");
		mutex_unlock(&icc_dev->lock);
		return -EINVAL;
	}

	mutex_unlock(&icc_dev->lock);
	return 0;
}
EXPORT_SYMBOL(icc_query);

int icc_nvs_write(u32 partition, u16 offset, u16 length, const void *data)
{
	u8 buf[ICC_MSG_MAX_SIZE] = {};
	struct icc_msg *msg = (struct icc_msg *)buf;

	msg->service_id = ICC_SERVICE_ID_NVS;
	msg->msg_type = 0;
	msg->length = sizeof(*msg) + 6 + length;
	msg->data[0] = 0;
	msg->data[1] = partition;
	*(u16 *)&msg->data[2] = offset;
	*(u16 *)&msg->data[4] = length;
	memcpy(&msg->data[6], data, length);

	return icc_query(buf, buf);
}
EXPORT_SYMBOL(icc_nvs_write);

int icc_nvs_read(u32 partition, u16 offset, u16 length, void *data)
{
	u8 buf[ICC_MSG_MAX_SIZE] = {};
	struct icc_msg *msg = (struct icc_msg *)buf;
	int ret;

	msg->service_id = ICC_SERVICE_ID_NVS;
	msg->msg_type = 1;
	msg->length = ICC_MSG_MIN_SIZE;
	msg->data[0] = 0;
	msg->data[1] = partition;
	*(u16 *)&msg->data[2] = offset;
	*(u16 *)&msg->data[4] = length;

	ret = icc_query(buf, buf);
	if (ret)
		return ret;

	memcpy(data, &msg->data[2], length);

	return 0;
}
EXPORT_SYMBOL(icc_nvs_read);

int icc_usbc_set_pdcon_op_mode(u8 PortId, u8 OpMode)
{
	u8 buf[ICC_MSG_MAX_SIZE] = {};
	struct icc_msg *msg = (struct icc_msg *)buf;

	msg->service_id = ICC_SERVICE_ID_USBC;
	msg->msg_type = 0x10;
	msg->length = ICC_MSG_MIN_SIZE;
	msg->data[0] = PortId;
	msg->data[1] = OpMode;

	return icc_query(buf, buf);
}
EXPORT_SYMBOL(icc_usbc_set_pdcon_op_mode);

int icc_configuration_set_cpu_info_bit(u8 *bit)
{
	u8 buf[ICC_MSG_MAX_SIZE] = {};
	struct icc_msg *msg = (struct icc_msg *)buf;

	msg->service_id = ICC_SERVICE_ID_GENERAL;
	msg->msg_type = 0x10;
	msg->length = ICC_MSG_MIN_SIZE;
	msg->data[0] = bit[1];
	msg->data[1] = bit[0];

	return icc_query(buf, buf);
}
EXPORT_SYMBOL(icc_configuration_set_cpu_info_bit);

int icc_configuration_clear_cpu_info_bit(void)
{
	u8 bit[2];
	int ret;

	bit[0] = 0;
	bit[1] = 0;
	ret = icc_configuration_set_cpu_info_bit(bit);
	if (ret)
		return ret;

	bit[0] = 0;
	bit[1] = 1;
	ret = icc_configuration_set_cpu_info_bit(bit);
	if (ret)
		return ret;

	return 0;
}
EXPORT_SYMBOL(icc_configuration_clear_cpu_info_bit);

__noreturn void icc_power_suspend(int keep)
{
	u8 buf[ICC_MSG_MAX_SIZE] = {};
	struct icc_msg *msg = (struct icc_msg *)buf;

	msg->service_id = ICC_SERVICE_ID_POWER;
	msg->msg_type = 1;
	msg->length = ICC_MSG_MIN_SIZE;
	msg->data[0] = 1; // with eap
	msg->data[1] = 1;
	msg->data[2] = 1;
	msg->data[3] = 0;
	msg->data[4] = 1;
	*(u16 *)&msg->data[5] = (2 * (keep != 0)) | 0x30;
	*(u16 *)&msg->data[7] = 0;

	icc_query(buf, buf);
	local_irq_disable();
	wbinvd();
	mp1_set_sleep_entry();
	while (1);
}
EXPORT_SYMBOL(icc_power_suspend);

__noreturn void icc_power_shutdown(void)
{
	u8 buf[ICC_MSG_MAX_SIZE] = {};
	struct icc_msg *msg = (struct icc_msg *)buf;

	icc_configuration_clear_cpu_info_bit();

	msg->service_id = ICC_SERVICE_ID_POWER;
	msg->msg_type = 1;
	msg->length = ICC_MSG_MIN_SIZE;
	msg->data[0] = 0;
	msg->data[1] = 0; // shutdown
	msg->data[2] = 2; // depth
	msg->data[3] = 0; // cause
	msg->data[4] = 1; // hand
	*(u32 *)&msg->data[5] = 0;

	icc_query(buf, buf);
	while (1);
}
EXPORT_SYMBOL(icc_power_shutdown);

__noreturn void icc_power_reboot(void)
{
	u8 buf[ICC_MSG_MAX_SIZE] = {};
	struct icc_msg *msg = (struct icc_msg *)buf;

	icc_configuration_clear_cpu_info_bit();

	msg->service_id = ICC_SERVICE_ID_POWER;
	msg->msg_type = 1;
	msg->length = ICC_MSG_MIN_SIZE;
	msg->data[0] = 0;
	msg->data[1] = 1; // reboot
	msg->data[2] = 2; // depth
	msg->data[3] = 0; // cause
	msg->data[4] = 1; // hand
	*(u32 *)&msg->data[5] = 0;

	icc_query(buf, buf);
	while (1);
}
EXPORT_SYMBOL(icc_power_reboot);

int icc_button_enable_notification(u8 type, u8 enable)
{
	u8 buf[ICC_MSG_MAX_SIZE] = {};
	struct icc_msg *msg = (struct icc_msg *)buf;

	msg->service_id = ICC_SERVICE_ID_BUTTON;
	msg->msg_type = 1;
	msg->length = 0x20;
	msg->data[0] = type;
	msg->data[1] = enable;

	return icc_query(buf, buf);
}
EXPORT_SYMBOL(icc_button_enable_notification);

int icc_button_enable_all_notifications(u8 enable)
{
	int ret;

	/* power button */
	ret = icc_button_enable_notification(0, enable);
	if (ret)
		return ret;

	/* eject button */
	ret = icc_button_enable_notification(1, enable);
	if (ret)
		return ret;

	/* reset button */
	ret = icc_button_enable_notification(2, enable);
	if (ret)
		return ret;

	return 0;
}
EXPORT_SYMBOL(icc_button_enable_all_notifications);

int icc_thermal_enable_notification(u8 enable)
{
	u8 buf[ICC_MSG_MAX_SIZE] = {};
	struct icc_msg *msg = (struct icc_msg *)buf;

	msg->service_id = ICC_SERVICE_ID_THERMAL;
	msg->msg_type = 2;
	msg->length = 0x20;
	msg->data[0] = enable;

	return icc_query(buf, buf);
}
EXPORT_SYMBOL(icc_thermal_enable_notification);

int icc_device_power_control(u8 device, u8 state)
{
	u8 buf[ICC_MSG_MAX_SIZE] = {};
	struct icc_msg *msg = (struct icc_msg *)buf;

	msg->service_id = ICC_SERVICE_ID_DEVICE;
	msg->msg_type = 0;
	msg->length = 0x20;
	msg->data[0] = device;
	msg->data[1] = state;

	return icc_query(buf, buf);
}
EXPORT_SYMBOL(icc_device_power_control);

int icc_device_power_get(u8 device, u8 *state)
{
	u8 buf[ICC_MSG_MAX_SIZE] = {};
	struct icc_msg *msg = (struct icc_msg *)buf;
	int ret;

	msg->service_id = ICC_SERVICE_ID_DEVICE;
	msg->msg_type = 1;
	msg->length = 0x20;
	msg->data[0] = device;

	ret = icc_query(buf, buf);
	if (ret)
		return ret;

	*state = msg->data[2];

	return 0;
}
EXPORT_SYMBOL(icc_device_power_get);

int icc_indicator_set_led(const u8 setting[], size_t setting_size)
{
	u8 buf[ICC_MSG_MAX_SIZE] = {};
	struct icc_msg *msg = (struct icc_msg *)buf;

	msg->service_id = ICC_SERVICE_ID_INDICATOR;
	msg->msg_type = 0x20;
	msg->length = sizeof(*msg) + setting_size;
	memcpy(msg->data, setting, setting_size);

	return icc_query(buf, buf);
}
EXPORT_SYMBOL(icc_indicator_set_led);

int icc_indicator_set_led_white(u8 level)
{
	switch (level) {
	case 0:
		return icc_indicator_set_led(indicator_white_dim, sizeof(indicator_white_dim));
	case 1:
		return icc_indicator_set_led(indicator_white_medium, sizeof(indicator_white_medium));
	case 2:
		return icc_indicator_set_led(indicator_white_bright, sizeof(indicator_white_bright));
	default:
		return -EINVAL;
	}
}
EXPORT_SYMBOL(icc_indicator_set_led_white);

int icc_fan_change_servo_pattern(u8 pattern)
{
	u8 buf[ICC_MSG_MAX_SIZE] = {};
	struct icc_msg *msg = (struct icc_msg *)buf;

	msg->service_id = ICC_SERVICE_ID_FAN;
	msg->msg_type = 0xb;
	msg->length = 0x20;
	msg->data[0] = pattern;
	msg->data[1] = pattern;
	msg->data[2] = pattern;
	msg->data[3] = pattern;

	return icc_query(buf, buf);
}
EXPORT_SYMBOL(icc_fan_change_servo_pattern);

static void button_notification_handler(struct icc_msg *msg)
{
	if (msg->msg_type == 0x8010) { // power button down
		input_report_key(icc_dev->idev, KEY_POWER, 1);
		input_sync(icc_dev->idev);
	} else if (msg->msg_type == 0x8011) { // power button up
		input_report_key(icc_dev->idev, KEY_POWER, 0);
		input_sync(icc_dev->idev);
	}
}

static void icc_notification_work_func(struct work_struct *work)
{
	struct icc_msg *msg;
	struct icc_notification entry;

	while (kfifo_out_spinlocked(&icc_notification_fifo, &entry, 1, &icc_notification_lock)) {
		msg = (struct icc_msg *)entry.notification;
		if (entry.service_id == ICC_SERVICE_ID_HDMI) {
			hdmi_notification_handler(msg);
		} else if (entry.service_id == ICC_SERVICE_ID_BUTTON) {
			button_notification_handler(msg);
		} else {
			pr_info("service id: %x\n", entry.service_id);
			print_hex_dump(KERN_INFO, "event: ", DUMP_PREFIX_OFFSET, 16, 1,
				msg->data, msg->length - sizeof(*msg), true);
		}

	}
}

static int icc_notification_handler(u8 service_id, u8 *notification)
{
	struct icc_msg *msg = (struct icc_msg *)notification;
	struct icc_notification entry;

	entry.service_id = service_id;
	memcpy(entry.notification, msg, msg->length);

	if (kfifo_in_spinlocked(&icc_notification_fifo, &entry, 1, &icc_notification_lock)) {
		schedule_work(&icc_notification_work);
	} else {
		pr_warn_ratelimited("icc notification buffer full\n");
	}

	return 0;
}

static irqreturn_t icc_interrupt(int irq, void *dev)
{
	struct spcie_icc_dev *icc_dev = dev;
	struct icc_msg *msg;
	u32 intr_status;

	while (1) {
		intr_status = readl(icc_dev->icc_doorbell_base + ICC_REG_INTR_STATUS);
		if (!intr_status)
			break;

		/* Ack the interrupt */
		writel(intr_status, icc_dev->icc_doorbell_base + ICC_REG_INTR_STATUS);

		if (intr_status & ICC_SEND) {
			msg = (struct icc_msg *)(icc_dev->icc_base + ICC_REPLY_OFFSET);

			if (msg->msg_type & ICC_MSG_TYPE_NOTIF) {
				icc_dev->notification_length = min(msg->length, ICC_MSG_MAX_SIZE);
				memcpy(icc_dev->notification, msg, icc_dev->notification_length);

				writew(0, icc_dev->icc_base + ICC_REG_EMW);
				writew(1, icc_dev->icc_base + ICC_REG_EMR);

				writel(ICC_ACK, icc_dev->icc_doorbell_base + ICC_REG_DOORBELL);

				icc_notification_handler(msg->service_id, icc_dev->notification);
			} else if (msg->msg_type & ICC_MSG_TYPE_REPLY) {
				icc_dev->reply_length = min(msg->length, ICC_MSG_MAX_SIZE);
				memcpy(icc_dev->reply, msg, icc_dev->reply_length);

				writew(0, icc_dev->icc_base + ICC_REG_EMW);
				writew(1, icc_dev->icc_base + ICC_REG_EMR);

				writel(ICC_ACK, icc_dev->icc_doorbell_base + ICC_REG_DOORBELL);

				icc_dev->reply_ready = true;
				wake_up(&icc_dev->wq);
			} else {
				dev_err(&icc_dev->pdev->dev, "unknown query\n");
			}
		}
	}

	return IRQ_HANDLED;
}

static long icc_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	switch (cmd) {
	case ICC_FAN_CHANGE_SERVO_PATTERN:
	{
		u8 pattern;
		if (copy_from_user(&pattern, (u8 __user *)arg, sizeof(u8)))
			return -EFAULT;
		return icc_fan_change_servo_pattern(pattern);
	}

	default:
		return -ENOTTY;
	}
}

static const struct file_operations icc_fops = {
	.owner = THIS_MODULE,
	.unlocked_ioctl = icc_ioctl,
};

static struct miscdevice icc_misc_device = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = "icc",
	.fops = &icc_fops,
};

static int spcie_power_button_init(struct spcie_icc_dev *icc_dev)
{
	icc_dev->idev = input_allocate_device();
	if (!icc_dev->idev)
		return -ENOMEM;

	icc_dev->idev->name = "Power Button";
	icc_dev->idev->phys = "pm/button/input0";
	icc_dev->idev->id.bustype = BUS_HOST;
	icc_dev->idev->dev.parent = NULL;
	input_set_capability(icc_dev->idev, EV_KEY, KEY_POWER);

	return input_register_device(icc_dev->idev);
}

static void spcie_power_button_remove(struct spcie_icc_dev *icc_dev)
{
	input_free_device(icc_dev->idev);
}

static int spcie_icc_init(struct spcie_dev *sdev)
{
	int vector, ret;

	icc_dev = devm_kzalloc(&sdev->pdev->dev, sizeof(*icc_dev), GFP_KERNEL);
	if (!icc_dev)
		return -ENOMEM;

	icc_dev->pdev = sdev->pdev;
	icc_dev->sdev = sdev;

	sdev->icc_dev = icc_dev;

	INIT_WORK(&icc_notification_work, icc_notification_work_func);

	mutex_init(&icc_dev->lock);
	init_waitqueue_head(&icc_dev->wq);

	icc_dev->icc_doorbell_base = sdev->bar2 + ICC_DOORBELL_OFFSET;
	icc_dev->icc_base = sdev->pervasive0;

	ret = spcie_power_button_init(icc_dev);
	if (ret)
		return ret;

	/* Clear status */
	writel(ICC_SEND | ICC_ACK, icc_dev->icc_doorbell_base + ICC_REG_INTR_STATUS);

	/* Request IRQ for icc subfunc */
	vector = pci_irq_vector(sdev->pdev, SPCIE_SUBFUNC_ICC);
	ret = devm_request_irq(&sdev->pdev->dev, vector, icc_interrupt, 0, "icc", icc_dev);
	if (ret)
		return ret;

	/* Enable IRQs */
	writel(ICC_SEND | ICC_ACK, icc_dev->icc_doorbell_base + ICC_REG_INTR_MASK);

	/* Set DMI info */
	static char hw_model[32] = {0}, hw_info[32] = {0};
	if (icc_nvs_read(2, 0x30, 0x20, hw_model) == 0) {
		dmi_set_system_info(DMI_PRODUCT_VERSION, hw_model);
	}
	if (icc_nvs_read(2, 0x10, 0x11, hw_info) == 0) {
		dmi_set_system_info(DMI_PRODUCT_SERIAL, hw_info);
	}

	/* Enable notifications */
	icc_button_enable_all_notifications(1);
	icc_thermal_enable_notification(1);

	/* Set LED to white dim */
	icc_indicator_set_led_white(0);

	/* Enable SuperSpeed-USB-C port */
	icc_usbc_set_pdcon_op_mode(0, 5);

	/* Initialize HDMI */
	getHdmiConfiguration();
	hdmiSystemResume();
	sceHdmiInitVideoConfig();

	ret = misc_register(&icc_misc_device);
	if (ret)
		return ret;

	return 0;
}

static int spcie_icc_remove(struct spcie_dev *sdev)
{
	spcie_power_button_remove(sdev->icc_dev);
	misc_deregister(&icc_misc_device);
	return 0;
}

static int spcie_icc_shutdown(struct spcie_dev *sdev)
{
	icc_fan_change_servo_pattern(0);

	/* Disable IRQs */
	writel(0, icc_dev->icc_doorbell_base + ICC_REG_INTR_MASK);

	return 0;
}

bool spcie_is_initialized(void)
{
	return spcie_initialized;
}
EXPORT_SYMBOL(spcie_is_initialized);

u32 spcie_get_chip_id(void)
{
	return spcie_chip_revision_id & 0xff0000;
}
EXPORT_SYMBOL(spcie_get_chip_id);

u32 spcie_get_revision_id(void)
{
	return spcie_chip_revision_id & 0xffff;
}
EXPORT_SYMBOL(spcie_get_revision_id);

u32 spcie_bar2_180000_read(u32 reg)
{
	return readl(sdev->bar2 + 0x180000 + reg);
}
EXPORT_SYMBOL(spcie_bar2_180000_read);

void spcie_bar2_180000_write(u32 reg, u32 val)
{
	writel(val, sdev->bar2 + 0x180000 + reg);
}
EXPORT_SYMBOL(spcie_bar2_180000_write);

u32 spcie_pervasive0_4000_read(u32 reg)
{
	return readl(sdev->pervasive0 + 0x4000 + reg);
}
EXPORT_SYMBOL(spcie_pervasive0_4000_read);

static int spcie_init_dev(struct spcie_dev *sdev, int dev_ip)
{
	void __iomem *bar;
	u32 offset0, offset1, offset2;
	u32 val;
	int retries = 10000;

	switch (dev_ip) {
	case 0:
		bar = sdev->bar2;
		offset0 = 0x142020;
		offset1 = 0x142028;
		offset2 = 0x180020;
		break;
	case 1:
		bar = sdev->bar2;
		offset0 = 0x143820;
		offset1 = 0x143828;
		offset2 = 0x18002c;
		break;
	case 2:
		bar = sdev->bar2;
		offset0 = 0x144820;
		offset1 = 0x144828;
		offset2 = 0x180030;
		break;
	case 3:
		bar = sdev->bar2;
		offset0 = 0x144020;
		offset1 = 0x144028;
		offset2 = 0x180024;
		break;
	case 4:
		bar = sdev->pervasive0;
		offset0 = 0x42420;
		offset1 = 0x42428;
		offset2 = 0x7010;
		break;
	}

	writel(readl(bar + offset0) | 0x10, bar + offset0);

	while (1) {
		val = readl(bar + offset1);
		if ((val & 0x10) == 0 && (val & 0x20) == 0 && (val & 0x40) == 0 && (val & 0x80) == 0)
			break;
		udelay(10);
		if (!--retries) {
			printk("[ERROR]: cannot stop the transaction(%X): %X\n", offset0, val);
			break;
		}
	}

	writel(readl(bar + offset0) | 0x1, bar + offset0);

	if (dev_ip == 4) {
		writel(1, sdev->pervasive0 + offset2);
		writel(3, sdev->pervasive0 + 0x74b4);
	} else {
		writel(3, sdev->bar2 + offset2);

		switch (dev_ip) {
		case 0:
			writel(1, sdev->pervasive0 + 0x74c8);
			writel(0x1f, sdev->pervasive0 + 0x7c28);
			break;
		case 1:
			writel(3, sdev->pervasive0 + 0x74c4);
			writel(7, sdev->pervasive0 + 0x7c14);
			break;
		case 2:
			writel(3, sdev->pervasive0 + 0x7498);
			writel(7, sdev->pervasive0 + 0x7c18);
			break;
		case 3:
			if (spcie_get_chip_id() == 0x110000) {
				writel(7, sdev->pervasive0 + 0x74cc);
			} else {
				writel(1, sdev->pervasive0 + 0x74cc);
			}
			writel(0xf, sdev->pervasive0 + 0x7c1c);
			break;
		}
	}

	writel(readl(bar + offset0) & ~0x11, bar + offset0);

	if (dev_ip == 4) {
		writel(0, sdev->pervasive0 + offset2);
	} else if (dev_ip == 0) {
		writel(0, sdev->bar2 + offset2);

		writel(readl(sdev->pervasive0 + 0x7c28) & ~8, sdev->pervasive0 + 0x7c28);
		writel(readl(sdev->pervasive0 + 0x7c28) & ~1, sdev->pervasive0 + 0x7c28);

		writel(1, sdev->bar2 + offset2);
		writel(0, sdev->bar2 + offset2);

		writel(readl(sdev->pervasive0 + 0x7c28) | 8, sdev->pervasive0 + 0x7c28);
		writel(readl(sdev->pervasive0 + 0x7c28) | 1, sdev->pervasive0 + 0x7c28);

		udelay(680);
	}

	return 0;
}

static int spcie_init(struct spcie_dev *sdev)
{
	writel(1, sdev->pervasive0 + 0x22004);
	while ((readl(sdev->pervasive0 + 0x22000) & 7) != 4);

	spcie_init_dev(sdev, 0);
	if (spcie_get_chip_id() == 0x110000)
		spcie_init_dev(sdev, 1);
	spcie_init_dev(sdev, 2);
	spcie_init_dev(sdev, 3);
	spcie_init_dev(sdev, 4);

	writel(1, sdev->bar2 + 0x18004c);
	writel(1, sdev->pervasive0 + 0x7c08);

	writel(0, sdev->bar2 + 0x18004c);
	writel(0, sdev->pervasive0 + 0x22004);
	while ((readl(sdev->pervasive0 + 0x22000) & 4) != 0);

	return 0;
}

static int spcie_probe(struct pci_dev *pdev, const struct pci_device_id *id)
{
	u32 chip_revision, chip_id0, chip_id1;
	char *chip_name, *rev_name;
	int ret;

	ret = pcim_enable_device(pdev);
	if (ret)
		return ret;

	sdev = devm_kzalloc(&pdev->dev, sizeof(*sdev), GFP_KERNEL);
	if (!sdev)
		return -ENOMEM;

	sdev->pdev = pdev;

	sdev->bar2 = pcim_iomap(pdev, 2, 0);
	if (!sdev->bar2)
		return -ENOMEM;

	sdev->pervasive0 = pcim_iomap(pdev, 4, 0);
	if (!sdev->pervasive0)
		return -ENOMEM;

	pci_set_master(pdev);

	chip_revision = readl(sdev->pervasive0 + 0x4000);
	dev_info(&sdev->pdev->dev, "Chip revision: %08x\n", chip_revision);

	chip_id0 = readl(sdev->pervasive0 + 0x4008);
	dev_info(&sdev->pdev->dev, "Chip ID0: %08x\n", chip_id0);

	chip_id1 = readl(sdev->pervasive0 + 0x4004);
	dev_info(&sdev->pdev->dev, "Chip ID1: %08x\n", chip_id1);

	if ((chip_revision & 0xff000000) == 0x11000000) {
		chip_name = "Salina";
		spcie_chip_revision_id = 0x110000;
	} else if ((chip_revision & 0xff000000) == 0x12000000) {
		chip_name = "Salina2";
		spcie_chip_revision_id = 0x120000;
	} else {
		panic("unknown subsys %08x\n", chip_revision);
	}

	switch (chip_revision & 0xffff) {
	case 0x100: rev_name = "A0"; break;
	case 0x101: rev_name = "A1"; break;
	case 0x200: rev_name = "B0"; break;
	case 0x201: rev_name = "B1"; break;
	case 0x300: rev_name = "C0"; break;
	default:
		panic("unknown subsys %08x\n", chip_revision);
	}

	spcie_chip_revision_id |= (chip_revision & 0xffff);

	dev_info(&sdev->pdev->dev, "%s %s\n", chip_name, rev_name);

	ret = spcie_init(sdev);
	if (ret)
		return ret;

	ret = pci_alloc_irq_vectors(pdev, NUM_IRQS, NUM_IRQS, PCI_IRQ_MSI);
	if (ret < 0)
		return ret;

	ret = spcie_icc_init(sdev);
	if (ret) {
		pci_free_irq_vectors(pdev);
		return ret;
	}

	spcie_initialized = true;

	pci_set_drvdata(pdev, sdev);
	return 0;
}

static void spcie_remove(struct pci_dev *pdev)
{
	struct spcie_dev *sdev = pci_get_drvdata(pdev);
	if (sdev) {
		spcie_icc_remove(sdev);
		pci_free_irq_vectors(pdev);
	}
}

static void spcie_shutdown(struct pci_dev *pdev)
{
	struct spcie_dev *sdev = pci_get_drvdata(pdev);
	if (sdev) {
		spcie_icc_shutdown(sdev);
	}
}

static const struct pci_device_id spcie_ids[] = {
	{ PCI_DEVICE(PCI_VENDOR_ID_SONY, PCI_DEVICE_ID_SPCIE) },
	{ 0, }
};
MODULE_DEVICE_TABLE(pci, spcie_ids);

static struct pci_driver spcie_driver = {
	.name		= "spcie",
	.id_table	= spcie_ids,
	.probe		= spcie_probe,
	.remove		= spcie_remove,
	.shutdown	= spcie_shutdown,
};

module_pci_driver(spcie_driver);

MODULE_AUTHOR("Andy Nguyen");
MODULE_DESCRIPTION("PlayStation 5 Salina PCI Express glue driver");
MODULE_LICENSE("GPL");
