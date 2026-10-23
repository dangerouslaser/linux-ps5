/* SPDX-License-Identifier: GPL-2.0-only */

#ifndef _LINUX_PS5_H
#define _LINUX_PS5_H

#include <linux/types.h>
#include <drm/drm_modes.h>

struct hdmi_drm_infoframe;

#define ICC_MSG_MIN_SIZE	0x20
#define ICC_MSG_MAX_SIZE 	0x7f0

struct icc_msg {
	u8 magic;
	u8 service_id;
	u16 msg_type;
	u16 unk_04;
	u16 id;
	u16 length;
	u16 checksum;
	u8 data[];
};

enum icc_service_id {
	ICC_SERVICE_ID_CONFIGURATION	= 0x01,
	ICC_SERVICE_ID_GENERAL		= 0x02,
	ICC_SERVICE_ID_NVS		= 0x03,
	ICC_SERVICE_ID_POWER		= 0x04,
	ICC_SERVICE_ID_DEVICE		= 0x05,
	ICC_SERVICE_ID_UNKNOWN_07	= 0x07,
	ICC_SERVICE_ID_BUTTON		= 0x08,
	ICC_SERVICE_ID_INDICATOR	= 0x09,
	ICC_SERVICE_ID_FAN		= 0x0a,
	ICC_SERVICE_ID_THERMAL		= 0x0b,
	ICC_SERVICE_ID_HDMI		= 0x10,
	ICC_SERVICE_ID_USBC		= 0x12,
	ICC_SERVICE_ID_UNKNOWN_13	= 0x13,
	ICC_SERVICE_ID_CRASH_REPORT	= 0x14,
	ICC_SERVICE_ID_BDDRIVE		= 0x15,
	ICC_SERVICE_ID_UNKNOWN_8C	= 0x8c,
	ICC_SERVICE_ID_UNKNOWN_8D	= 0x8d,
	ICC_SERVICE_ID_SC_CONFIG	= 0x8e,
	ICC_SERVICE_ID_FLOYD		= 0x9a,
};

bool spcie_is_initialized(void);

u32 spcie_get_chip_id(void);
u32 spcie_get_revision_id(void);
u32 spcie_bar2_180000_read(u32 reg);
void spcie_bar2_180000_write(u32 reg, u32 val);
u32 spcie_pervasive0_4000_read(u32 reg);

int icc_query(u8 *query, u8 *reply);

void hdmiSystemResume(void);
void sceHdmiInitVideoConfig(void);
void sceHdmiSetVideoConfig(const struct drm_display_mode *mode, unsigned int bpc, bool yuv422);
void sceHdmiSetHdrConfig(const struct hdmi_drm_infoframe *frame, bool bt2020);
void sceHdmiSetAudioConfig(int channels);
void sceHdmiDeviceSetVideoMute(int mute);
void sceHdmiSetAudioMute(int mute);
void sceHdmiOutputMode(void);
int getHdmiConfiguration(void);
u32 getHdmiDpAudioN(u32 default_n);
bool isHdmiModeValid(const struct drm_display_mode *mode, int force_1080p);
bool isHdmiHdrSupported(void);

int icc_nvs_write(u32 partition, u16 offset, u16 length, const void *data);
int icc_nvs_read(u32 partition, u16 offset, u16 length, void *data);

int icc_usbc_set_pdcon_op_mode(u8 PortId, u8 OpMode);
int icc_configuration_set_cpu_info_bit(u8 *bit);
int icc_configuration_clear_cpu_info_bit(void);

__noreturn void icc_power_suspend(int keep);
void __noreturn icc_power_shutdown(void);
void __noreturn icc_power_reboot(void);

int icc_button_enable_notification(u8 type, u8 enable);
int icc_button_enable_all_notifications(u8 enable);
int icc_thermal_enable_notification(u8 enable);

int icc_device_power_control(u8 device, u8 state);
int icc_device_power_get(u8 device, u8 *state);

int icc_indicator_set_led(const u8 setting[], size_t setting_size);
int icc_indicator_set_led_white(u8 level);

int icc_fan_change_servo_pattern(u8 pattern);
int icc_fan_update_param(const u8 *param);
int icc_fan_update_autoservo_param(void);

void hdmi_notification_handler(struct icc_msg *msg);
void hdmi_set_hotplug_handler(void (*handler)(void *data), void *data);

void mp1_set_pm1_addr(u64 addr);
int mp1_set_sleep_entry(void);

u64 svm_execute_guest(void *guest_func, u64 a0, u64 a1, u64 a2, u64 a3);

#endif /* _LINUX_PS5_H */
