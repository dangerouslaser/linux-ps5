// SPDX-License-Identifier: GPL-2.0-only

#include <linux/module.h>
#include <linux/leds.h>
#include <linux/mutex.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/ps5.h>
#include "../led.h"
#include "presets.h"

static const u8 ps5_led_off_payload[] = {
	0x03, 0x00, 0x00, 0x00,
	0x10, 0x01, 0x02, 0x00, 0x05, 0x04, 0x00,
	0x11, 0x01, 0x02, 0x00, 0x05, 0x04, 0x00,
	0x12, 0x01, 0x02, 0x00, 0x05, 0x04, 0x00,
};

struct ps5_led_node {
	struct led_classdev cdev;
	const u8           *payload;
	size_t              size;
};

static DEFINE_MUTEX(ps5_led_lock);
static const u8 *ps5_led_current;

static int ps5_led_brightness_set(struct led_classdev *cdev,
				   enum led_brightness value)
{
	struct ps5_led_node *node =
		container_of(cdev, struct ps5_led_node, cdev);
	const u8 *data = (value == LED_OFF) ? ps5_led_off_payload : node->payload;
	size_t    size = (value == LED_OFF) ? sizeof(ps5_led_off_payload) : node->size;
	int ret;

	if (!spcie_is_initialized())
		return -ENODEV;

	mutex_lock(&ps5_led_lock);

	if (ps5_led_current == data) {
		mutex_unlock(&ps5_led_lock);
		return 0;
	}

	if (value != LED_OFF)
		icc_indicator_set_led(ps5_led_off_payload,
				      sizeof(ps5_led_off_payload));

	ret = icc_indicator_set_led(data, size);
	if (!ret)
		ps5_led_current = data;

	mutex_unlock(&ps5_led_lock);
	return ret;
}

int ps5_led_set_raw(u8 blue, u8 white, u8 orange)
{
	u8 buf[25];

	if (!spcie_is_initialized())
		return -ENODEV;

	if (!blue && !white && !orange)
		return icc_indicator_set_led(ps5_led_off_payload,
					     sizeof(ps5_led_off_payload));

	buf[0]  = 0x03; buf[1]  = 0x00; buf[2]  = 0x00; buf[3]  = 0x00;
	buf[4]  = 0x10; buf[5]  = 0x01; buf[6]  = 0x02; buf[7]  = blue;
	buf[8]  = 0x05; buf[9]  = 0x03; buf[10] = 0x00;
	buf[11] = 0x11; buf[12] = 0x01; buf[13] = 0x02; buf[14] = white;
	buf[15] = 0x05; buf[16] = 0x03; buf[17] = 0x00;
	buf[18] = 0x12; buf[19] = 0x01; buf[20] = 0x02; buf[21] = orange;
	buf[22] = 0x05; buf[23] = 0x03; buf[24] = 0x00;

	return icc_indicator_set_led(buf, sizeof(buf));
}
EXPORT_SYMBOL(ps5_led_set_raw);

static int ps5_led_probe(struct platform_device *pdev)
{
	size_t i;
	int ret;

	for (i = 0; i < ps5_num_presets; i++) {
		struct ps5_led_node *node;
		char *name;

		node = devm_kzalloc(&pdev->dev, sizeof(*node), GFP_KERNEL);
		if (!node)
			return -ENOMEM;

		name = devm_kasprintf(&pdev->dev, GFP_KERNEL,
				      "ps5:%s:indicator",
				      ps5_presets[i].name);
		if (!name)
			return -ENOMEM;

		node->payload              = ps5_presets[i].data;
		node->size                 = ps5_presets[i].size;
		node->cdev.name            = name;
		node->cdev.max_brightness  = 255;
		node->cdev.brightness_set_blocking = ps5_led_brightness_set;
		node->cdev.flags           = LED_CORE_SUSPENDRESUME;

		ret = devm_led_classdev_register(&pdev->dev, &node->cdev);
		if (ret)
			return ret;
	}

	return 0;
}

static struct platform_driver ps5_led_driver = {
	.probe  = ps5_led_probe,
	.driver = {
		.name = "ps5-led",
	},
};

static struct platform_device *ps5_led_pdev;

static int __init ps5_led_init(void)
{
	int ret;

	ret = platform_driver_register(&ps5_led_driver);
	if (ret)
		return ret;

	ps5_led_pdev = platform_device_register_simple("ps5-led", -1,
						       NULL, 0);
	if (IS_ERR(ps5_led_pdev)) {
		ret = PTR_ERR(ps5_led_pdev);
		platform_driver_unregister(&ps5_led_driver);
		ps5_led_pdev = NULL;
		return ret;
	}

	return 0;
}

static void __exit ps5_led_exit(void)
{
	if (ps5_led_pdev)
		platform_device_unregister(ps5_led_pdev);
	platform_driver_unregister(&ps5_led_driver);
}

module_init(ps5_led_init);
module_exit(ps5_led_exit);

MODULE_AUTHOR("Armandas Kvietkus <armandas.kvietkus@proton.me>");
MODULE_DESCRIPTION("PlayStation 5 indicator LED driver");
MODULE_LICENSE("GPL");
MODULE_ALIAS("platform:ps5-led");
