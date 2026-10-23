/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _PS5_LED_PRESETS_H
#define _PS5_LED_PRESETS_H

#include <linux/types.h>

struct ps5_led_preset {
	const char *name;
	const u8   *data;
	size_t      size;
};

extern const struct ps5_led_preset ps5_presets[];
extern const size_t ps5_num_presets;

#endif
