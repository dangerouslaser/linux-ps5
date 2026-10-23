// SPDX-License-Identifier: GPL-2.0-only

#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/err.h>
#include <linux/unaligned.h>
#include <linux/ps5.h>
#include <linux/io.h>
#include "ps5-fan.h"
#include "autoservo_param.h"

#define PS5_FAN_BOOT_TARGET_TEMP	80

#define GDDR6_SAMSUNG		0x01
#define GDDR6_HYNIX		0x06
#define GDDR6_MICRON		0x0f

#define BOARD_MASK_A		0xffffffffffffff00
#define BOARD_MASK_B		0xffffffffff00ff00

#define BOARD_SANCARLO_1	0x2001010104010400
#define BOARD_SANCARLO_2	0x2001010104010500
#define BOARD_A_EIGER_1		0x3002010104010400
#define BOARD_A_EIGER_2		0x3002010104010500
#define BOARD_A_DENALI_1	0x3002010104030400
#define BOARD_A_DENALI_2	0x3002010104030500
#define BOARD_B_EIGER_DENALI_1	0x3002020101000400
#define BOARD_B_EIGER_DENALI_2	0x3002020101000500
#define BOARD_D_EIGER_DENALI_1	0x3002040101000400
#define BOARD_D_EIGER_DENALI_2	0x3002040101000500

static void bootparam_get_config_board_id(u64 *board_id)
{
	void __iomem *bootparam = ioremap(0x9b000, 0x2000);
	*board_id = readq(bootparam + 0x328);
	iounmap(bootparam);
}

static void bootparam_get_config_gddr6_id(u16 *gddr6_id)
{
	void __iomem *bootparam = ioremap(0x9b000, 0x2000);
	*gddr6_id = readw(bootparam + 0x1184);
	iounmap(bootparam);
}

int icc_fan_update_param(const u8 *param)
{
	u8 buf[ICC_MSG_MAX_SIZE] = {};
	struct icc_msg *msg = (struct icc_msg *)buf;

	msg->service_id = ICC_SERVICE_ID_FAN;
	msg->msg_type = 0xe;
	msg->length = sizeof(*msg) + 0x6ae;
	memcpy(msg->data, param, 0x6ae);

	return icc_query(buf, buf);
}
EXPORT_SYMBOL(icc_fan_update_param);

int icc_fan_update_autoservo_param(void)
{
	u64 board_id, board_mask_a, board_mask_b;
	u16 gddr6_id;
	u8 gddr6_vendor, gddr6_revision;
	const char *board_name = NULL;
	const char *gddr6_name = NULL;
	const u8 *param = NULL;
	int ret;

	bootparam_get_config_board_id(&board_id);
	board_id = be64_to_cpu(board_id);
	board_mask_a = board_id & BOARD_MASK_A;
	board_mask_b = board_id & BOARD_MASK_B;

	bootparam_get_config_gddr6_id(&gddr6_id);
	gddr6_vendor = gddr6_id & 0xf;
	gddr6_revision = (gddr6_id >> 4) & 0xf;

	if (board_mask_a == BOARD_SANCARLO_1 || board_mask_a == BOARD_SANCARLO_2) {
		board_name = "Sancarlo";
		param = autoservo_param_sancarlo;
	} else if (board_mask_a == BOARD_A_EIGER_1 || board_mask_a == BOARD_A_EIGER_2) {
		board_name = "A-Eiger";
		switch (gddr6_vendor) {
		case GDDR6_SAMSUNG:
			gddr6_name = "Samsung";
			param = autoservo_param_a_eiger_samsung;
			break;
		case GDDR6_HYNIX:
			gddr6_name = "Hynix";
			param = autoservo_param_a_eiger_hynix;
			break;
		case GDDR6_MICRON:
			gddr6_name = "Micron";
			param = autoservo_param_a_eiger_micron;
			break;
		default:
			pr_info("FanServo: %s Unknown Vendor 0x%x\n", board_name, gddr6_vendor);
			goto not_updated;
		}
	} else if (board_mask_a == BOARD_A_DENALI_1 || board_mask_a == BOARD_A_DENALI_2) {
		board_name = "A-Denali";
		switch (gddr6_vendor) {
		case GDDR6_SAMSUNG:
			gddr6_name = "Samsung";
			param = autoservo_param_a_denali_samsung;
			break;
		case GDDR6_HYNIX:
			gddr6_name = "Hynix";
			param = autoservo_param_a_denali_hynix;
			break;
		case GDDR6_MICRON:
			gddr6_name = "Micron";
			param = autoservo_param_a_denali_micron;
			break;
		default:
			pr_info("FanServo: %s Unknown Vendor 0x%x\n", board_name, gddr6_vendor);
			goto not_updated;
		}
	} else if (board_mask_b == BOARD_B_EIGER_DENALI_1 || board_mask_b == BOARD_B_EIGER_DENALI_2) {
		board_name = "B-Eiger/Denali";
		if (gddr6_vendor == GDDR6_SAMSUNG && gddr6_revision == 8) {
			gddr6_name = "Samsung D1x";
			param = autoservo_param_b_eiger_denali_samsung_d1x;
		} else if (gddr6_vendor == GDDR6_SAMSUNG && gddr6_revision == 9) {
			gddr6_name = "Samsung D1z";
			param = NULL;
		} else if (gddr6_vendor == GDDR6_HYNIX && gddr6_revision == 0) {
			gddr6_name = "Hynix 1x";
			param = autoservo_param_b_eiger_denali_hynix_1x_micron_120s;
		} else if (gddr6_vendor == GDDR6_HYNIX && gddr6_revision == 1) {
			gddr6_name = "Hynix 1y";
			param = autoservo_param_b_eiger_denali_hynix_1y_micron_130s;
		} else if (gddr6_vendor == GDDR6_MICRON && gddr6_revision == 0) {
			gddr6_name = "Micron 120s";
			param = autoservo_param_b_eiger_denali_hynix_1x_micron_120s;
		} else if (gddr6_vendor == GDDR6_MICRON && gddr6_revision == 1) {
			gddr6_name = "Micron 130s";
			param = autoservo_param_b_eiger_denali_hynix_1y_micron_130s;
		} else {
			pr_info("FanServo: %s Unknown Vendor/Revision 0x%x/0x%x\n", board_name, gddr6_vendor, gddr6_revision);
			goto not_updated;
		}
	} else if (board_mask_b == BOARD_D_EIGER_DENALI_1 || board_mask_b == BOARD_D_EIGER_DENALI_2) {
		board_name = "D-Eiger/Denali";
		if (gddr6_vendor == GDDR6_SAMSUNG && gddr6_revision == 9) {
			gddr6_name = "Samsung D1z";
			param = autoservo_param_d_eiger_denali_samsung_d1z;
		} else if (gddr6_vendor == GDDR6_HYNIX && gddr6_revision == 1) {
			gddr6_name = "Hynix 1y";
			param = autoservo_param_d_eiger_denali_hynix_1y;
		} else if (gddr6_vendor == GDDR6_MICRON && gddr6_revision == 1) {
			gddr6_name = "Micron 130s";
			param = autoservo_param_d_eiger_denali_micron_130s_y31j;
		} else if (gddr6_vendor == GDDR6_MICRON && gddr6_revision == 2) {
			gddr6_name = "Micron Y31J";
			param = autoservo_param_d_eiger_denali_micron_130s_y31j;
		} else {
			pr_info("FanServo: %s Unknown Vendor/Revision 0x%x/0x%x\n", board_name, gddr6_vendor, gddr6_revision);
			goto not_updated;
		}
	} else {
		pr_info("FanServo: 0x%016llX\n", board_id);
		goto not_updated;
	}

	if (gddr6_name)
		pr_info("FanServo: %s %s\n", board_name, gddr6_name);
	else
		pr_info("FanServo: %s\n", board_name);

	if (!param)
		goto not_updated;

	ret = icc_fan_update_param(param);
	if (ret)
		pr_err("Error: %s: %d\n", __func__, ret);

	return ret;

not_updated:
	pr_info("FanServo: Not update\n");
	return 0;
}
EXPORT_SYMBOL(icc_fan_update_autoservo_param);

static int icc_fan_txn(u8 svc, u16 mt, u16 len,
		       const u8 *data, size_t dlen,
		       u8 *reply_out, size_t rlen)
{
	u8 *buf;
	struct icc_msg *msg;
	int ret;

	buf = kzalloc(ICC_MSG_MAX_SIZE, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;
	msg = (struct icc_msg *)buf;

	msg->service_id = svc;
	msg->msg_type = mt;
	msg->length = len;
	if (data && dlen)
		memcpy(msg->data, data, dlen);

	ret = icc_query(buf, buf);
	if (!ret && reply_out && rlen)
		memcpy(reply_out, msg->data, rlen);

	kfree(buf);
	return ret;
}

static int icc_fan_get_curset(u8 zone, u32 out[PS5_CURSET_COUNT])
{
	u8 req[ICC_MSG_MAX_SIZE - sizeof(struct icc_msg)] = {};
	u8 reply[ICC_MSG_MIN_SIZE] = {};
	int ret, i;

	req[PS5_FAN_ZONE_OFF] = zone;

	ret = icc_fan_txn(ICC_SERVICE_ID_FAN, PS5_FAN_MT_CURSET_GET,
			  PS5_FAN_CURSET_GET_LEN, req, sizeof(req),
			  reply, sizeof(reply));
	if (ret)
		return ret;

	for (i = 0; i < PS5_CURSET_COUNT; i++)
		out[i] = get_unaligned_le32(reply +
			  PS5_FAN_CURSET_GET_OFF + i * 4);
	return 0;
}

static int icc_fan_set_curset(u8 zone, u8 config, const u32 in[PS5_CURSET_COUNT])
{
	u8 req[ICC_MSG_MAX_SIZE - sizeof(struct icc_msg)] = {};
	int i;

	req[PS5_FAN_ZONE_OFF] = zone;
	req[PS5_FAN_CONFIG_OFF] = config;
	for (i = 0; i < PS5_CURSET_COUNT; i++)
		put_unaligned_le32(in[i], req + PS5_FAN_CURSET_SET_OFF + i * 4);

	return icc_fan_txn(ICC_SERVICE_ID_FAN, PS5_FAN_MT_CURSET_SET,
			   PS5_FAN_CURSET_SET_LEN, req, sizeof(req), NULL, 0);
}

static DEFINE_MUTEX(fan_lock);

static int fan_get_target_temp(u8 zone, u16 *celsius)
{
	u32 cs[PS5_CURSET_COUNT];
	int ret;

	mutex_lock(&fan_lock);
	ret = icc_fan_get_curset(zone, cs);
	mutex_unlock(&fan_lock);
	if (ret)
		return ret;

	*celsius = cs[PS5_CURSET_TARGETTEMP] >> PS5_TARGETTEMP_SHIFT;
	return 0;
}

static int fan_set_target_temp(u8 zone, u8 config, u16 celsius)
{
	u32 cs[PS5_CURSET_COUNT];
	int ret;

	mutex_lock(&fan_lock);
	ret = icc_fan_get_curset(zone, cs);
	if (!ret) {
		cs[PS5_CURSET_TARGETTEMP] = (u32)celsius << PS5_TARGETTEMP_SHIFT;
		ret = icc_fan_set_curset(zone, config, cs);
	}
	mutex_unlock(&fan_lock);
	return ret;
}

static ssize_t target_temp_show(struct device *dev,
				struct device_attribute *attr, char *buf)
{
	u16 c;
	int ret = fan_get_target_temp(PS5_ZONE_MAINSOC, &c);

	if (ret)
		return ret;
	return sysfs_emit(buf, "%u\n", c);
}

static ssize_t target_temp_store(struct device *dev,
				 struct device_attribute *attr,
				 const char *buf, size_t count)
{
	u16 c;
	int ret;

	if (kstrtou16(buf, 10, &c))
		return -EINVAL;
	if (c > 100)
		return -ERANGE;

	ret = fan_set_target_temp(PS5_ZONE_MAINSOC, 0, c);
	return ret ? ret : count;
}
static DEVICE_ATTR_RW(target_temp);

static struct attribute *ps5_fan_attrs[] = {
	&dev_attr_target_temp.attr,
	NULL,
};
ATTRIBUTE_GROUPS(ps5_fan);

static int ps5_fan_probe(struct platform_device *pdev)
{
	int ret = icc_fan_update_autoservo_param();

	if (ret)
		dev_warn(&pdev->dev, "autoservo param load failed: %d\n", ret);

	ret = fan_set_target_temp(PS5_ZONE_MAINSOC, 0, PS5_FAN_BOOT_TARGET_TEMP);
	if (ret)
		dev_warn(&pdev->dev, "boot target temp set failed: %d\n", ret);

	return 0;
}

static struct platform_driver ps5_fan_driver = {
	.probe  = ps5_fan_probe,
	.driver = {
		.name = "ps5-fan",
		.dev_groups = ps5_fan_groups,
	},
};

static struct platform_device *ps5_fan_pdev;

static int __init ps5_fan_init(void)
{
	int ret = platform_driver_register(&ps5_fan_driver);

	if (ret)
		return ret;

	ps5_fan_pdev = platform_device_register_simple("ps5-fan", -1, NULL, 0);
	if (IS_ERR(ps5_fan_pdev)) {
		ret = PTR_ERR(ps5_fan_pdev);
		platform_driver_unregister(&ps5_fan_driver);
		ps5_fan_pdev = NULL;
		return ret;
	}
	return 0;
}

static void __exit ps5_fan_exit(void)
{
	if (ps5_fan_pdev)
		platform_device_unregister(ps5_fan_pdev);
	platform_driver_unregister(&ps5_fan_driver);
}

module_init(ps5_fan_init);
module_exit(ps5_fan_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("PS5 fan driver");
MODULE_ALIAS("platform:ps5-fan");
