/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _PS5_LED_H
#define _PS5_LED_H

#include <linux/types.h>

int ps5_led_set_raw(u8 blue, u8 white, u8 orange);
int ps5_led_set_preset(const char *name);

#endif
