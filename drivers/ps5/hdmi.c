// SPDX-License-Identifier: GPL-2.0-only

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/hdmi.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/debugfs.h>
#include <linux/moduleparam.h>
#include <linux/seq_file.h>
#include <linux/string.h>
#include <linux/uaccess.h>
#include <linux/ps5.h>
#include <drm/drm_edid.h>

#define HDMI_IC_TYPE_FLAVA3	2
#define HDMI_IC_TYPE_VERDE	3

struct icc_i2c_msg {
	u8 code;
	u16 length;
	u8 count;
	u8 data[];
} __packed;

struct i2c_cmd_hdr {
	u8 major;
	u8 length;
	u8 minor;
	u8 count;
	u8 data[];
} __packed;

struct i2c_cmd_2_1 {
	u8 length;
	u8 reg_high;
	u8 reg_low;
	u8 data[];
} __packed;

struct i2c_cmd_write {
	u8 length;
	u8 reg_high;
	u8 reg_low;
	u8 data[];
} __packed;

struct i2c_cmd_mask {
	u8 length;
	u8 reg_high;
	u8 reg_low;
	u8 value;
	u8 mask[];
} __packed;

struct i2c_cmd_delay {
	u8 length;
	u8 time_low;
	u8 time_high;
	u8 unk_03;
} __packed;

struct i2c_cmd_waitset {
	u8 length;
	u8 reg_high;
	u8 reg_low;
	u8 value;
} __packed;

struct i2c_cmd_waitclear {
	u8 length;
	u8 reg_high;
	u8 reg_low;
	u8 value;
} __packed;

struct i2c_cmd_3_5 {
	u8 length;
	u8 reg_high;
	u8 reg_low;
	u8 value;
} __packed;

struct i2c_cmd_5_3 {
	u8 value;
} __packed;

struct i2c_cmd_5_4 {
	u8 value;
} __packed;

struct i2c_cmd_5_5 {
	u8 value;
} __packed;

struct i2c_block {
	u8 length;
	u16 reg;
	u8 data[32];
};

struct i2c_context {
	struct i2c_block blocks[128];
	int block_index;
	u8 msg_buf[ICC_MSG_MAX_SIZE - sizeof(struct icc_msg)];
	u8 *msg_cur;
	struct icc_i2c_msg *msg_hdr;
};

static struct i2c_context i2c_ctx;

static u8 hdmi_ic_type;
static u8 tmds_polarity;
static u8 tmds_ch_swap;
static u8 dp_polarity;
static u8 dp_ch_swap;
static bool scrambling;

static char *extra_vics = "";
module_param(extra_vics, charp, 0644);
MODULE_PARM_DESC(extra_vics, "Experimental: extra CTA VICs to allow, comma separated");

static int content_type = -1;
module_param(content_type, int, 0644);
MODULE_PARM_DESC(content_type, "AVI content type: -1 game (default), 0 none, 1 graphics, 2 photo, 3 cinema, 4 game");

static bool is_extra_vic(u8 vic)
{
	char buf[128], *p, *tok;
	unsigned int v;

	if (!vic || !extra_vics || !*extra_vics)
		return false;

	strscpy(buf, extra_vics, sizeof(buf));
	p = buf;
	while ((tok = strsep(&p, ",")))
		if (!kstrtouint(strim(tok), 10, &v) && v == vic)
			return true;

	return false;
}

/* Modes above the HDMI 1.4 TMDS limit need scrambling (2160p60 recipe). */
static bool flava3_high_tmds(const struct drm_display_mode *mode)
{
	u8 vic = drm_match_cea_mode(mode);

	return vic == 97 || (is_extra_vic(vic) && mode->clock > 340000);
}
static bool hdr_enabled;
static bool bt2020_enabled;

/* Serializes use of i2c_ctx, held from i2c_init() to i2c_exec(). */
static DEFINE_MUTEX(i2c_lock);

static void i2c_init(u8 code)
{
	mutex_lock(&i2c_lock);
	i2c_ctx.msg_cur = i2c_ctx.msg_buf;
	i2c_ctx.msg_hdr = (struct icc_i2c_msg *)i2c_ctx.msg_cur;

	i2c_ctx.msg_hdr->code = code;
	i2c_ctx.msg_hdr->length = 0;
	i2c_ctx.msg_hdr->count = 0;
	i2c_ctx.block_index = -1;

	i2c_ctx.msg_cur += sizeof(*i2c_ctx.msg_hdr);
}

static int i2c_exec(void)
{
	u8 buf[ICC_MSG_MAX_SIZE] = {};
	struct icc_msg *msg = (struct icc_msg *)buf;
	int ret;

	i2c_ctx.msg_hdr->length = i2c_ctx.msg_cur - i2c_ctx.msg_buf;

	msg->service_id = ICC_SERVICE_ID_HDMI;
	msg->msg_type = 0;
	msg->length = sizeof(*msg) + i2c_ctx.msg_hdr->length;
	memcpy(msg->data, i2c_ctx.msg_hdr, i2c_ctx.msg_hdr->length);

	ret = icc_query(buf, buf);
	mutex_unlock(&i2c_lock);

	return ret;
}

static void i2c_write_block(struct i2c_block *block, size_t count)
{
	struct i2c_cmd_hdr *hdr = (struct i2c_cmd_hdr *)i2c_ctx.msg_cur;
	int i;

	hdr->major = 2;
	hdr->minor = 2;
	hdr->count = count;
	hdr->length = sizeof(*hdr);
	i2c_ctx.msg_cur += sizeof(*hdr);

	for (i = 0; i < count; i++) {
		struct i2c_cmd_write *cmd = (struct i2c_cmd_write *)i2c_ctx.msg_cur;

		cmd->length = block[i].length;
		cmd->reg_low = block[i].reg & 0xff;
		cmd->reg_high = block[i].reg >> 8;
		memcpy(cmd->data, block[i].data, block[i].length);

		i2c_ctx.msg_cur += sizeof(*cmd) + cmd->length;
		hdr->length += sizeof(*cmd) + cmd->length;
	}

	i2c_ctx.msg_hdr->count++;
}

static void i2c_begin_block(void)
{
	i2c_ctx.block_index = 0;
}

static void i2c_end_block(void)
{
	if (i2c_ctx.block_index > 0)
		i2c_write_block(i2c_ctx.blocks, i2c_ctx.block_index);
	i2c_ctx.block_index = -1;
}

static void i2c_write_data(u16 reg, u8 value[], size_t count)
{
	struct i2c_block *block;
	bool new_block = i2c_ctx.block_index == -1;

	if (new_block)
		i2c_begin_block();

	block = &i2c_ctx.blocks[i2c_ctx.block_index++];
	block->reg = reg;
	block->length = count;
	memcpy(block->data, value, count);

	if (new_block)
		i2c_end_block();
}

static void i2c_write(u16 reg, u8 value)
{
	i2c_write_data(reg, &value, 1);
}

static void i2c_cmd_2_1(u16 reg, u8 unk)
{
	struct i2c_cmd_hdr *hdr = (struct i2c_cmd_hdr *)i2c_ctx.msg_cur;
	hdr->major = 2;
	hdr->minor = 1;
	hdr->count = 1;
	hdr->length = sizeof(*hdr);
	i2c_ctx.msg_cur += sizeof(*hdr);

	struct i2c_cmd_2_1 *cmd = (struct i2c_cmd_2_1 *)i2c_ctx.msg_cur;
	cmd->length = 1;
	cmd->reg_low = reg & 0xff;
	cmd->reg_high = reg >> 8;
	cmd->data[0] = unk;
	i2c_ctx.msg_cur += sizeof(*cmd) + cmd->length;
	hdr->length += sizeof(*cmd) + cmd->length;

	i2c_ctx.msg_hdr->count++;
}

static void i2c_delay(u16 time)
{
	struct i2c_cmd_hdr *hdr = (struct i2c_cmd_hdr *)i2c_ctx.msg_cur;
	hdr->major = 3;
	hdr->minor = 1;
	hdr->count = 1;
	hdr->length = sizeof(*hdr);
	i2c_ctx.msg_cur += sizeof(*hdr);

	struct i2c_cmd_delay *cmd = (struct i2c_cmd_delay *)i2c_ctx.msg_cur;
	cmd->length = 0;
	cmd->time_low = time & 0xff;
	cmd->time_high = time >> 8;
	cmd->unk_03 = 0;
	i2c_ctx.msg_cur += sizeof(*cmd) + cmd->length;
	hdr->length += sizeof(*cmd) + cmd->length;

	i2c_ctx.msg_hdr->count++;
}

static void i2c_mask(u16 reg, u8 value, u8 mask)
{
	struct i2c_cmd_hdr *hdr = (struct i2c_cmd_hdr *)i2c_ctx.msg_cur;
	hdr->major = 2;
	hdr->minor = 3;
	hdr->count = 1;
	hdr->length = sizeof(*hdr);
	i2c_ctx.msg_cur += sizeof(*hdr);

	struct i2c_cmd_mask *cmd = (struct i2c_cmd_mask *)i2c_ctx.msg_cur;
	cmd->length = 1;
	cmd->reg_low = reg & 0xff;
	cmd->reg_high = reg >> 8;
	cmd->value = value;
	cmd->mask[0] = mask;
	i2c_ctx.msg_cur += sizeof(*cmd) + cmd->length;
	hdr->length += sizeof(*cmd) + cmd->length;

	i2c_ctx.msg_hdr->count++;
}

static void i2c_waitset(u16 reg, u8 value)
{
	struct i2c_cmd_hdr *hdr = (struct i2c_cmd_hdr *)i2c_ctx.msg_cur;
	hdr->major = 3;
	hdr->minor = 2;
	hdr->count = 1;
	hdr->length = sizeof(*hdr);
	i2c_ctx.msg_cur += sizeof(*hdr);

	struct i2c_cmd_waitset *cmd = (struct i2c_cmd_waitset *)i2c_ctx.msg_cur;
	cmd->length = 0;
	cmd->reg_low = reg & 0xff;
	cmd->reg_high = reg >> 8;
	cmd->value = value;
	i2c_ctx.msg_cur += sizeof(*cmd) + cmd->length;
	hdr->length += sizeof(*cmd) + cmd->length;

	i2c_ctx.msg_hdr->count++;
}

static void i2c_waitclear(u16 reg, u8 value)
{
	struct i2c_cmd_hdr *hdr = (struct i2c_cmd_hdr *)i2c_ctx.msg_cur;
	hdr->major = 3;
	hdr->minor = 3;
	hdr->count = 1;
	hdr->length = sizeof(*hdr);
	i2c_ctx.msg_cur += sizeof(*hdr);

	struct i2c_cmd_waitclear *cmd = (struct i2c_cmd_waitclear *)i2c_ctx.msg_cur;
	cmd->length = 0;
	cmd->reg_low = reg & 0xff;
	cmd->reg_high = reg >> 8;
	cmd->value = value;
	i2c_ctx.msg_cur += sizeof(*cmd) + cmd->length;
	hdr->length += sizeof(*cmd) + cmd->length;

	i2c_ctx.msg_hdr->count++;
}

static void i2c_cmd_3_5(u16 reg, u8 value)
{
	struct i2c_cmd_hdr *hdr = (struct i2c_cmd_hdr *)i2c_ctx.msg_cur;
	hdr->major = 3;
	hdr->minor = 5;
	hdr->count = 1;
	hdr->length = sizeof(*hdr);
	i2c_ctx.msg_cur += sizeof(*hdr);

	struct i2c_cmd_3_5 *cmd = (struct i2c_cmd_3_5 *)i2c_ctx.msg_cur;
	cmd->length = 0;
	cmd->reg_low = reg & 0xff;
	cmd->reg_high = reg >> 8;
	cmd->value = value;
	i2c_ctx.msg_cur += sizeof(*cmd) + cmd->length;
	hdr->length += sizeof(*cmd) + cmd->length;

	i2c_ctx.msg_hdr->count++;
}

static void i2c_cmd_4_16(void *data, size_t size)
{
	struct i2c_cmd_hdr *hdr = (struct i2c_cmd_hdr *)i2c_ctx.msg_cur;
	hdr->major = 4;
	hdr->minor = 16;
	hdr->count = 1;
	hdr->length = sizeof(*hdr);
	i2c_ctx.msg_cur += sizeof(*hdr);

	memcpy(i2c_ctx.msg_cur, data, size);
	i2c_ctx.msg_cur += size;
	hdr->length += size;

	i2c_ctx.msg_hdr->count++;
}

static void i2c_cmd_5_3(u8 unk)
{
	struct i2c_cmd_hdr *hdr = (struct i2c_cmd_hdr *)i2c_ctx.msg_cur;
	hdr->major = 5;
	hdr->minor = 3;
	hdr->count = 1;
	hdr->length = sizeof(*hdr);
	i2c_ctx.msg_cur += sizeof(*hdr);

	struct i2c_cmd_5_3 *cmd = (struct i2c_cmd_5_3 *)i2c_ctx.msg_cur;
	cmd->value = unk;
	i2c_ctx.msg_cur += sizeof(*cmd);
	hdr->length += sizeof(*cmd);

	i2c_ctx.msg_hdr->count++;
}

static void i2c_cmd_5_4(u8 event)
{
	struct i2c_cmd_hdr *hdr = (struct i2c_cmd_hdr *)i2c_ctx.msg_cur;
	hdr->major = 5;
	hdr->minor = 4;
	hdr->count = 1;
	hdr->length = sizeof(*hdr);
	i2c_ctx.msg_cur += sizeof(*hdr);

	struct i2c_cmd_5_4 *cmd = (struct i2c_cmd_5_4 *)i2c_ctx.msg_cur;
	cmd->value = event;
	i2c_ctx.msg_cur += sizeof(*cmd);
	hdr->length += sizeof(*cmd);

	i2c_ctx.msg_hdr->count++;
}

static void i2c_cmd_5_5(u8 event)
{
	struct i2c_cmd_hdr *hdr = (struct i2c_cmd_hdr *)i2c_ctx.msg_cur;
	hdr->major = 5;
	hdr->minor = 5;
	hdr->count = 1;
	hdr->length = sizeof(*hdr);
	i2c_ctx.msg_cur += sizeof(*hdr);

	struct i2c_cmd_5_5 *cmd = (struct i2c_cmd_5_5 *)i2c_ctx.msg_cur;
	cmd->value = event;
	i2c_ctx.msg_cur += sizeof(*cmd);
	hdr->length += sizeof(*cmd);

	i2c_ctx.msg_hdr->count++;
}

static int stopHdcpHw(void)
{
	u8 buf[ICC_MSG_MAX_SIZE] = {};
	struct icc_msg *msg = (struct icc_msg *)buf;

	msg->service_id = ICC_SERVICE_ID_GENERAL;
	msg->msg_type = 0x1f;
	msg->length = ICC_MSG_MIN_SIZE;

	return icc_query(buf, buf);
}

static void sceSetBackToUnpluggedSequence(void)
{
	static u8 data[] = {0x11, 0x51, 0x09, 0x00, 0x02, 0x09, 0x03, 0x01, 0x01, 0x70, 0x5f, 0x80, 0x80, 0x03, 0x08, 0x01, 0x01, 0x00, 0x32, 0x00, 0x00, 0x02, 0x09, 0x03, 0x01, 0x01, 0x7a, 0x88, 0xff, 0xff, 0x03, 0x08, 0x03, 0x01, 0x00, 0x7a, 0x84, 0x01, 0x02, 0x09, 0x03, 0x01, 0x01, 0x74, 0x0a, 0xff, 0xff, 0x03, 0x08, 0x03, 0x01, 0x00, 0x74, 0x0a, 0xff, 0x02, 0x09, 0x03, 0x01, 0x01, 0x74, 0x19, 0x05, 0x07, 0x02, 0x09, 0x03, 0x01, 0x01, 0x7a, 0x8b, 0x00, 0x07, 0x03, 0x08, 0x01, 0x01, 0x00, 0x32, 0x00, 0x00};
	i2c_init(0);
	i2c_cmd_4_16(data, sizeof(data));
	i2c_exec();
}

static void sceSetBackToWaitResolutionSequence(void)
{
	static u8 data[] = {0x12, 0x47, 0x08, 0x00, 0x02, 0x09, 0x03, 0x01, 0x01, 0x70, 0x5f, 0x80, 0x80, 0x02, 0x08, 0x02, 0x01, 0x01, 0x7a, 0x88, 0xff, 0x03, 0x08, 0x03, 0x01, 0x00, 0x7a, 0x84, 0x01, 0x02, 0x08, 0x02, 0x01, 0x01, 0x74, 0x0a, 0xff, 0x03, 0x08, 0x03, 0x01, 0x00, 0x74, 0x0a, 0xff, 0x02, 0x09, 0x03, 0x01, 0x01, 0x74, 0x19, 0x05, 0x07, 0x02, 0x09, 0x03, 0x01, 0x01, 0x7a, 0x8b, 0x00, 0x07, 0x03, 0x08, 0x01, 0x01, 0x00, 0x64, 0x00, 0x00};
	i2c_init(0);
	i2c_cmd_4_16(data, sizeof(data));
	i2c_exec();
}

static void i2c_cmd_4_2(void)
{
	static u8 data[] = {0x31, 0x7a, 0xb8, 0x08, 0x32, 0x7a, 0xb0, 0x05, 0x33, 0x7a, 0xa0, 0x05, 0x34, 0x7a, 0xe1, 0x01, 0x35, 0x7a, 0xe2, 0x01, 0x36, 0x7a, 0xe0, 0x01, 0x37, 0x7c, 0x00, 0x05};

	i2c_init(0);

	struct i2c_cmd_hdr *hdr = (struct i2c_cmd_hdr *)i2c_ctx.msg_cur;
	hdr->major = 4;
	hdr->minor = 2;
	hdr->count = 7;
	hdr->length = sizeof(*hdr);
	i2c_ctx.msg_cur += sizeof(*hdr);

	memcpy(i2c_ctx.msg_cur, data, sizeof(data));
	i2c_ctx.msg_cur += sizeof(data);
	hdr->length += sizeof(data);

	i2c_ctx.msg_hdr->count++;

	i2c_exec();
}

static void sceSetHdcpSequence1st(void)
{
	static u8 data[] = {0x0b, 0x91, 0x0f, 0x00, 0x02, 0x0c, 0x02, 0x02, 0x01, 0x7a, 0x8b, 0x05, 0x01, 0x7a, 0x88, 0xff, 0x03, 0x08, 0x03, 0x01, 0x00, 0x7a, 0x84, 0x01, 0x02, 0x11, 0x02, 0x03, 0x02, 0x10, 0xe5, 0xff, 0xff, 0x01, 0x7a, 0x85, 0x00, 0x01, 0x7a, 0x83, 0x84, 0x03, 0x08, 0x02, 0x01, 0x00, 0x7a, 0x84, 0xa0, 0x03, 0x08, 0x03, 0x01, 0x00, 0x7a, 0x84, 0x01, 0x02, 0x08, 0x01, 0x01, 0x01, 0x7a, 0x83, 0x83, 0x03, 0x08, 0x02, 0x01, 0x00, 0x7a, 0x84, 0xa0, 0x03, 0x08, 0x03, 0x01, 0x00, 0x7a, 0x84, 0x01, 0x03, 0x08, 0x01, 0x01, 0x00, 0x0f, 0x00, 0x00, 0x02, 0x08, 0x01, 0x01, 0x01, 0x7a, 0x83, 0x84, 0x03, 0x08, 0x02, 0x01, 0x00, 0x7a, 0x84, 0xa0, 0x03, 0x08, 0x03, 0x01, 0x00, 0x7a, 0x84, 0x01, 0x02, 0x10, 0x02, 0x03, 0x01, 0x10, 0xe6, 0xff, 0x01, 0x7a, 0x85, 0x00, 0x01, 0x7a, 0x83, 0xe3, 0x03, 0x08, 0x02, 0x01, 0x00, 0x7a, 0x84, 0xa0, 0x03, 0x08, 0x03, 0x01, 0x00, 0x7a, 0x84, 0x01};

	i2c_init(0);
	i2c_cmd_4_16(data, sizeof(data));
	i2c_exec();
}

static void sceSetHdcpSequence2nd(void)
{
	static u8 data[] = {0x0c, 0x39, 0x06, 0x00, 0x02, 0x0d, 0x02, 0x02, 0x02, 0x10, 0xe5, 0xff, 0xff, 0x01, 0x7a, 0x83, 0xdc, 0x03, 0x08, 0x02, 0x01, 0x00, 0x7a, 0x84, 0xa0, 0x03, 0x08, 0x03, 0x01, 0x00, 0x7a, 0x84, 0x01, 0x02, 0x08, 0x01, 0x01, 0x01, 0x7a, 0x83, 0xc4, 0x03, 0x08, 0x02, 0x01, 0x00, 0x7a, 0x84, 0xa0, 0x03, 0x08, 0x03, 0x01, 0x00, 0x7a, 0x84, 0x01};
	i2c_init(0);
	i2c_cmd_4_16(data, sizeof(data));
	i2c_exec();
}

static void sceSetHdcpSequence3rd(void)
{
	static u8 data[] = {0x0d, 0x21, 0x01, 0x00, 0x02, 0x1d, 0x02, 0x06, 0x01, 0x10, 0xe7, 0xff, 0x01, 0x7a, 0x8b, 0x03, 0x02, 0x10, 0xe5, 0xff, 0xff, 0x01, 0x7a, 0x9d, 0x4f, 0x01, 0x7a, 0x83, 0x8e, 0x01, 0x7e, 0x03, 0x10};
	i2c_init(0);
	i2c_cmd_4_16(data, sizeof(data));
	i2c_exec();
}

static void sceSetEdidSequence(int sequence)
{
	static u8 seq_0[] = {0x04, 0x3f, 0x06, 0x00, 0x02, 0x0c, 0x02, 0x02, 0x01, 0x7a, 0x88, 0xff, 0x01, 0x7a, 0x83, 0x88, 0x03, 0x08, 0x02, 0x01, 0x00, 0x7a, 0x84, 0xa0, 0x03, 0x08, 0x03, 0x01, 0x00, 0x7a, 0x84, 0x01, 0x02, 0x0f, 0x02, 0x02, 0x01, 0x7a, 0x9c, 0x0e, 0x04, 0x7a, 0x80, 0x00, 0x00, 0x7f, 0x82, 0x03, 0x08, 0x02, 0x01, 0x00, 0x7a, 0x84, 0xa0, 0x03, 0x08, 0x03, 0x01, 0x00, 0x7a, 0x84, 0x01};
	static u8 seq_1[] = {0x05, 0x3f, 0x06, 0x00, 0x02, 0x0c, 0x02, 0x02, 0x01, 0x7a, 0x88, 0xff, 0x01, 0x7a, 0x83, 0x88, 0x03, 0x08, 0x02, 0x01, 0x00, 0x7a, 0x84, 0xa0, 0x03, 0x08, 0x03, 0x01, 0x00, 0x7a, 0x84, 0x01, 0x02, 0x0f, 0x02, 0x02, 0x01, 0x7a, 0x9c, 0x0e, 0x04, 0x7a, 0x80, 0x00, 0x80, 0x7f, 0x82, 0x03, 0x08, 0x02, 0x01, 0x00, 0x7a, 0x84, 0xa0, 0x03, 0x08, 0x03, 0x01, 0x00, 0x7a, 0x84, 0x01};
	static u8 seq_2[] = {0x06, 0x3f, 0x06, 0x00, 0x02, 0x0c, 0x02, 0x02, 0x01, 0x7a, 0x88, 0xff, 0x01, 0x7a, 0x83, 0x88, 0x03, 0x08, 0x02, 0x01, 0x00, 0x7a, 0x84, 0xa0, 0x03, 0x08, 0x03, 0x01, 0x00, 0x7a, 0x84, 0x01, 0x02, 0x0f, 0x02, 0x02, 0x01, 0x7a, 0x9c, 0x0e, 0x04, 0x7a, 0x80, 0x01, 0x00, 0x7f, 0x82, 0x03, 0x08, 0x02, 0x01, 0x00, 0x7a, 0x84, 0xa0, 0x03, 0x08, 0x03, 0x01, 0x00, 0x7a, 0x84, 0x01};
	static u8 seq_3[] = {0x07, 0x3f, 0x06, 0x00, 0x02, 0x0c, 0x02, 0x02, 0x01, 0x7a, 0x88, 0xff, 0x01, 0x7a, 0x83, 0x88, 0x03, 0x08, 0x02, 0x01, 0x00, 0x7a, 0x84, 0xa0, 0x03, 0x08, 0x03, 0x01, 0x00, 0x7a, 0x84, 0x01, 0x02, 0x0f, 0x02, 0x02, 0x01, 0x7a, 0x9c, 0x0e, 0x04, 0x7a, 0x80, 0x01, 0x80, 0x7f, 0x82, 0x03, 0x08, 0x02, 0x01, 0x00, 0x7a, 0x84, 0xa0, 0x03, 0x08, 0x03, 0x01, 0x00, 0x7a, 0x84, 0x01};

	i2c_init(0);
	if (sequence == 0) {
		i2c_cmd_4_16(seq_0, sizeof(seq_0));
	} else if (sequence == 1) {
		i2c_cmd_4_16(seq_1, sizeof(seq_1));
	} else if (sequence == 2) {
		i2c_cmd_4_16(seq_2, sizeof(seq_2));
	} else if (sequence == 3) {
		i2c_cmd_4_16(seq_3, sizeof(seq_3));
	}
	i2c_exec();
}

static void sceSetWaitPllSequence(void)
{
	static u8 data[] = {0x0a, 0x15, 0x02, 0x00, 0x02, 0x09, 0x03, 0x01, 0x01, 0x70, 0x21, 0xff, 0xff, 0x03, 0x08, 0x01, 0x01, 0x00, 0x64, 0x00, 0x00};
	i2c_init(0);
	i2c_cmd_4_16(data, sizeof(data));
	i2c_exec();
}

static void unk_akv(void)
{
	i2c_init(4);
	i2c_begin_block();
	i2c_write(0x7006, 0x12);
	i2c_write(0x7a88, 0xff);
	i2c_end_block();
	i2c_waitclear(0x7a84, 0x01);
	i2c_cmd_2_1(0x7a83, 0x88);
	i2c_waitset(0x7a84, 0xa0);
	i2c_waitclear(0x7a84, 0x01);
	i2c_begin_block();
	i2c_write(0x7a8b, 0x05);
	i2c_write(0x7a89, 0x01);
	i2c_end_block();
	i2c_delay(20);
	i2c_cmd_2_1(0x7a83, 0x0e);
	i2c_waitset(0x7a84, 0xa0);
	i2c_exec();
}

static void sceControlHdmiEvent(u8 enable)
{
	i2c_init(4);
	i2c_cmd_5_4(enable);
	i2c_exec();
}

void hdmiSystemResume(void)
{
	if (hdmi_ic_type == HDMI_IC_TYPE_FLAVA3) {
		/* Needed for audio to come back after a replug. */
		sceSetBackToUnpluggedSequence();
		sceSetBackToWaitResolutionSequence();
		i2c_cmd_4_2();
		// sceSetHdcpSequence1st();
		// sceSetHdcpSequence2nd();
		// sceSetHdcpSequence3rd();
		sceSetEdidSequence(0);
		sceSetEdidSequence(1);
		sceSetEdidSequence(2);
		sceSetEdidSequence(3);
		sceSetWaitPllSequence();
		unk_akv();
	}
	sceControlHdmiEvent(1);
}
EXPORT_SYMBOL(hdmiSystemResume);

static void sceDisableEncode(void)
{
	i2c_init(4);
	i2c_mask(0x705f, 0x80, 0x80);
	i2c_cmd_5_3(0x00);
	i2c_mask(0x7021, 0x00, 0xf0);
	i2c_cmd_2_1(0x7a88, 0xff);
	i2c_waitclear(0x7a84, 0x01);
	i2c_cmd_2_1(0x740a, 0xff);
	i2c_waitclear(0x740a, 0xff);
	i2c_mask(0x7419, 0x05, 0x07);
	i2c_mask(0x7a8b, 0x00, 0x07);
	i2c_delay(100);
	i2c_exec();
}

static void initIsrForFlava3(void)
{
	i2c_init(0);
	i2c_begin_block();
	i2c_write(0x705f, 0x12);
	i2c_write(0x6004, 0x80);
	i2c_write(0x6020, 0x00);
	i2c_write(0x7007, 0xff);
	i2c_write(0x100c, 0x01);
	i2c_write(0x6008, 0xc0);
	i2c_write(0x6207, 0x00);
	i2c_write(0x621b, 0x00);
	i2c_write_data(0x6080, (u8[]){0xff, 0xff, 0xff, 0xff, 0xff, 0xff}, 6);
	i2c_write_data(0x6090, (u8[]){0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff}, 8);
	i2c_write_data(0x10e7, (u8[]){0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff}, 8);
	i2c_write_data(0x10e9, (u8[]){0xff, 0xff}, 2);
	i2c_write_data(0x10f0, (u8[]){0xff, 0x07, 0x00, 0x0f, 0x00, 0x56, 0x00, 0x00, 0xd0, 0x00}, 10);
	i2c_write(0x7018, (tmds_polarity == 1) << 1);
	i2c_end_block();
	if (tmds_ch_swap == 1) {
		i2c_mask(0x701a, 0xb1, 0xff);
	}
	i2c_exec();
}

static void initDpForFlava3(void)
{
	i2c_init(0);
	i2c_begin_block();
	i2c_write(0x6a03, 0x04);
	i2c_write(0x60a2, 0xf1);
	i2c_write(0x60bf, 0x03);
	i2c_write(0x60c0, 0xef);
	i2c_write(0x60c3, 0x4d);
	i2c_write(0x60c7, 0x85);
	i2c_write(0x60bf, 0x04);
	i2c_write(0x60c7, 0x84);
	if (dp_polarity != 0xff) {
		i2c_write(0x600e, dp_polarity << 4);
	}
	if (dp_ch_swap != 0xff) {
		i2c_write(0x600b, dp_ch_swap);
	}
	i2c_write_data(0x6220, (u8[]){0x40, 0x00, 0x80, 0x00, 0x00, 0x01, 0x02}, 7);
	i2c_write_data(0x6028, (u8[]){0x01, 0x02}, 2);
	i2c_write_data(0x6058, (u8[]){0x01, 0x02, 0x03, 0x04}, 4);
	i2c_write(0x62af, 0x01);
	i2c_write(0x6207, 0x00);
	i2c_write(0x621b, 0x00);
	i2c_write(0x60e0, 0x1e);
	i2c_write(0x603c, 0x04);
	i2c_write(0x100e, 0x10);
	i2c_end_block();
	i2c_delay(2);
	i2c_write(0x6005, 0x01);
	i2c_delay(2);
	i2c_write(0x6008, 0x00);
	i2c_exec();
}

static void configVSyncSettingFlava3(void)
{
	i2c_init(1);
	i2c_begin_block();
	i2c_write(0x1047, 0x00);
	i2c_write(0x6064, 0x01);
	i2c_write_data(0x600c, (u8[]){0x01, 0x00}, 2);
	i2c_write(0x6c07, 0x00);
	i2c_write_data(0x7214, (u8[]){0x00, 0x00}, 2);
	i2c_end_block();
	i2c_exec();
}

static void configParamFlava3Pre(void)
{
	i2c_init(1);
	i2c_begin_block();
	i2c_write_data(0x6224, (u8[]){0x00, 0x01}, 2);
	i2c_write_data(0x1047, (u8[]){0x00}, 1);
	i2c_write_data(0x1050, (u8[]){0x00, 0x00, 0x00, 0x00}, 4);
	i2c_write(0x7215, 0x00);
	i2c_write(0x7077, 0x00);
	i2c_write(0x7079, 0x80);
	i2c_end_block();
	i2c_exec();
}

static void configLinkTrainingFlava3(void)
{
	i2c_init(1);
	i2c_begin_block();
	i2c_write_data(0x600c, (u8[]){0x01, 0x00}, 2);
	i2c_write_data(0x6c00, (u8[]){0x1e, 0x84, 0x00}, 3);
	i2c_end_block();
	i2c_mask(0x6005, 0x01, 0x01);
	i2c_delay(2);
	i2c_mask(0x6006, 0x04, 0x04);
	i2c_delay(2);
	i2c_write(0x6a03, 0x47);
	i2c_delay(10);
	i2c_waitset(0x60f8, 0xff);
	i2c_waitset(0x60f9, 0x01);
	i2c_write(0x6a01, 0x4d);
	i2c_waitset(0x60f9, 0x1a);
	i2c_waitset(0x6083, 0x02);
	i2c_exec();
}

static void initHdmiPhyForFlava3_1st(void)
{
	i2c_init(0);
	i2c_begin_block();
	i2c_write(0x7022, 0x03);
	i2c_write(0x7030, 0x40);
	i2c_write_data(0x7024, (u8[]){0x04, 0x54}, 2);
	i2c_write(0x7030, 0x41);
	i2c_write_data(0x7024, (u8[]){0x04, 0x54}, 2);
	i2c_write(0x7030, 0x42);
	i2c_write_data(0x7024, (u8[]){0x04, 0x34}, 2);
	i2c_write(0x7030, 0x43);
	i2c_write_data(0x7024, (u8[]){0x04, 0x34}, 2);
	i2c_write(0x7030, 0x04);
	i2c_write_data(0x7024, (u8[]){0x04, 0x71}, 2);
	i2c_write(0x7030, 0x14);
	i2c_write(0x7025, 0x71);
	i2c_write(0x7030, 0x24);
	i2c_write(0x7025, 0x70);
	i2c_write(0x7030, 0x34);
	i2c_write(0x7025, 0x77);
	i2c_write(0x7030, 0x05);
	i2c_write_data(0x7024, (u8[]){0x04, 0x71}, 2);
	i2c_write(0x7030, 0x15);
	i2c_write(0x7025, 0x71);
	i2c_write(0x7030, 0x25);
	i2c_write(0x7025, 0x70);
	i2c_write(0x7030, 0x35);
	i2c_write(0x7025, 0x77);
	i2c_write(0x7030, 0x06);
	i2c_write_data(0x7024, (u8[]){0x04, 0x71}, 2);
	i2c_write(0x7030, 0x16);
	i2c_write(0x7025, 0x70);
	i2c_write(0x7030, 0x26);
	i2c_write(0x7025, 0x70);
	i2c_write(0x7030, 0x36);
	i2c_write(0x7025, 0x77);
	i2c_end_block();
	i2c_exec();
}

static void initHdmiPhyForFlava3_2nd(void)
{
	i2c_init(0);
	i2c_begin_block();
	i2c_write(0x7030, 0x47);
	i2c_write(0x7024, 0x04);
	i2c_write(0x7025, 0x37);
	i2c_write(0x7027, 0x17);
	i2c_write(0x7030, 0x37);
	i2c_write(0x7025, 0x71);
	i2c_write(0x7027, 0x14);
	i2c_write(0x7030, 0x48);
	i2c_write(0x7024, 0x04);
	i2c_write(0x7025, 0x37);
	i2c_write(0x7027, 0x17);
	i2c_write(0x7030, 0x38);
	i2c_write(0x7025, 0x77);
	i2c_write(0x7027, 0x14);
	i2c_write(0x7030, 0x49);
	i2c_write(0x7024, 0x01);
	i2c_write(0x7030, 0x19);
	i2c_write(0x7026, 0xcc);
	i2c_write(0x7030, 0x29);
	i2c_write(0x7026, 0xdd);
	i2c_write(0x7030, 0x39);
	i2c_write(0x7026, 0xef);
	i2c_write(0x7030, 0x4a);
	i2c_write(0x7024, 0x01);
	i2c_write(0x7030, 0x1a);
	i2c_write(0x7026, 0xcc);
	i2c_write(0x7030, 0x2a);
	i2c_write(0x7026, 0xdd);
	i2c_write(0x7030, 0x3a);
	i2c_write(0x7026, 0xef);
	i2c_write(0x7030, 0x4b);
	i2c_write(0x7024, 0x01);
	i2c_write(0x7030, 0x1b);
	i2c_write(0x7026, 0xcc);
	i2c_write(0x7030, 0x2b);
	i2c_write(0x7026, 0xdd);
	i2c_write(0x7030, 0x3b);
	i2c_write(0x7026, 0xef);
	i2c_end_block();
	i2c_exec();
}

static void mask_7203(void)
{
	i2c_init(1);
	i2c_mask(0x7203, 0x00, 0x80);
	i2c_exec();
}

static void setHdmiBasicVideoConfigFlava3(const struct drm_display_mode *mode,
					  unsigned int bpc, bool yuv422)
{
	u8 vic = drm_match_cea_mode(mode);
	int hz = drm_mode_vrefresh(mode);

	i2c_init(1);
	i2c_mask(0x7021, 0x00, 0xf0);
	i2c_delay(500);
	i2c_begin_block();
	i2c_write(0x100c, 0x01);
	i2c_write_data(0x68a0, (u8[]){0x96, 0x04}, 2);
	i2c_write(0x7005, 0x80);
	i2c_write(0x7019, 0x00);
	i2c_write(0x100c, 0x01);
	i2c_write(0x7005, 0x80);
	i2c_write(0x7009, 0x00);
	i2c_write(0x7040, 0x42);
	i2c_write(0x7225, 0x28);
	if (vic == 16 || vic == 63 ||
	    (is_extra_vic(vic) && !flava3_high_tmds(mode))) {
		/* Set VIC and content type */
		i2c_write_data(0x7227, (u8[]){vic, 0x00}, 2);
		i2c_write_data(0x7070, (u8[]){vic, vic, 0x00, 0x00, 0x00, 0x00}, 6);
	} else if (mode->hdisplay == 2560 && mode->vdisplay == 1440) {
		/* Set VIC and content type */
		i2c_write_data(0x7227, (u8[]){0x04, 0x00}, 2);
		i2c_write_data(0x7070, (u8[]){0x00, 0x00, 0x00, 0x00, 0x00, 0xfb}, 6);
	} else if (flava3_high_tmds(mode)) {
		/* Set VIC and content type */
		i2c_write_data(0x7227, (u8[]){0x06, 0x00}, 2);
		i2c_write_data(0x7070, (u8[]){0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, 6);
	}
	i2c_write(0x70c0, 0xdc);
	i2c_write(0x621b, 0x00);
	i2c_write(0x629a, 0x00);
	/* Set color format */
	if (yuv422) {
		i2c_write_data(0x70c4, (u8[]){0x04, 0x04}, 2);
	} else {
		i2c_write_data(0x70c4, (u8[]){0x08, 0x08}, 2);
	}
	i2c_write(0x70c2, 0x00);
	i2c_write(0x70fe, 0x02);
	/* Set color depth */
	i2c_write(0x70c3, yuv422 && bpc == 12 ? 0x02 : 0x00);
	i2c_write(0x7018, (tmds_polarity == 1) << 1);
	if (tmds_ch_swap == 1) {
		i2c_end_block();
		i2c_mask(0x701a, 0xb1, 0xff);
		i2c_begin_block();
	}
	i2c_write(0x10e7, 0xff);
	i2c_write(0x7202, 0x20);
	if (flava3_high_tmds(mode)) {
		i2c_write(0x7072, 0x01);
		i2c_write(0x7074, 0x07);
		i2c_write(0x7206, 0x80);
		i2c_write(0x7203, 0x60);
		i2c_write(0x7011, 0xff);
	} else {
		i2c_write(0x7203, 0x60);
		i2c_write(0x7011, 0xd5);
	}
	i2c_end_block();
	i2c_waitclear(0x7011, 0xff);
	i2c_exec();
}

static void setVideoAdditionalConfigFlava3(const struct drm_display_mode *mode)
{
	u8 vic = drm_match_cea_mode(mode);
	bool high_tmds = flava3_high_tmds(mode);

	i2c_init(1);
	i2c_waitset(0x10e7, 0x80);
	if (high_tmds) {
		/* Set VIC */
		i2c_mask(0x7227, vic, 0xff);
	}
	/* AVI PB3 ITC and PB5 CN: IT content, game by default */
	if (content_type == 0) {
		i2c_mask(0x7226, 0, 0x80);
		i2c_mask(0x7228, 0, 0x30);
	} else {
		i2c_mask(0x7226, 1 << 7, 0x80);
		i2c_mask(0x7228, ((content_type > 0 && content_type <= 4 ?
				   content_type : 4) - 1) << 4, 0x30);
	}
	i2c_write(0x7204, 0x40);
	i2c_waitclear(0x7204, 0x40);
	i2c_delay(10);
	i2c_begin_block();
	if (high_tmds) {
		i2c_write(0x7019, 0x01);
	} else {
		i2c_write(0x7019, 0x00);
	}
	i2c_write(0x7419, 0x05);
	i2c_write(0x740a, 0xff);
	i2c_end_block();
	i2c_waitclear(0x740a, 0xff);
	i2c_begin_block();
	i2c_write(0x7404, 0x00);
	i2c_write(0x7a88, 0xff);
	i2c_end_block();
	i2c_waitclear(0x7a84, 0x01);
	i2c_begin_block();
	i2c_write(0x7a8b, 0x05);
	if (high_tmds || scrambling) {
		i2c_write(0x7c00, high_tmds ? 0x03 : 0x00);
		i2c_write_data(0x7a80, (u8[]){0xa8, 0x20, 0x00, 0x80}, 4);
	}
	i2c_end_block();
	if (high_tmds || scrambling) {
		i2c_waitclear(0x7a84, 0x01);
	}
	scrambling = high_tmds;
	i2c_begin_block();
	i2c_write(0x7021, 0xff);
	i2c_write(0x700a, 0x02);
	i2c_write(0x705f, 0x80);
	i2c_end_block();
	i2c_delay(700);
	i2c_write(0x7a8b, 0x00);
	i2c_cmd_5_3(0x01);
	i2c_exec();
}

static void setHdmiVideoConfigFlava3(const struct drm_display_mode *mode,
				     unsigned int bpc, bool yuv422)
{
	configParamFlava3Pre();
	configLinkTrainingFlava3();
	setHdmiBasicVideoConfigFlava3(mode, bpc, yuv422);
	initHdmiPhyForFlava3_1st();
	initHdmiPhyForFlava3_2nd();
	mask_7203();
	setVideoAdditionalConfigFlava3(mode);
}

static void setHdrConfigFlava3(const struct hdmi_drm_infoframe *frame, bool bt2020)
{
	struct hdmi_drm_infoframe sdr;
	u8 data[HDMI_INFOFRAME_SIZE(DRM)];
	bool hdr = frame != NULL;

	/* Sinks keep the last InfoFrame, send EOTF 0 when leaving HDR. */
	if (!frame && hdr_enabled) {
		hdmi_drm_infoframe_init(&sdr);
		frame = &sdr;
	}
	if (!frame && bt2020 == bt2020_enabled)
		return;
	if (frame && hdmi_drm_infoframe_pack_only(frame, data, sizeof(data)) != sizeof(data))
		return;

	i2c_init(1);
	if (frame) {
		/* Send DRM InfoFrame */
		i2c_write_data(0x7e40, data, sizeof(data));
		i2c_mask(0x7203, 0x01, 0x01);
		i2c_write(0x7204, 0x01);
		i2c_waitclear(0x7204, 0x01);
	}
	if (bt2020 || bt2020_enabled) {
		/* Set colorimetry */
		i2c_mask(0x7225, bt2020 ? 0xc0 : 0x00, 0xc0);
		i2c_mask(0x7226, bt2020 ? 0x60 : 0x00, 0x70);
		i2c_write(0x7204, 0x40);
		i2c_waitclear(0x7204, 0x40);
	}
	i2c_exec();
	hdr_enabled = hdr;
	bt2020_enabled = bt2020;
}

/* Audio stream format, set by the HDA driver. 0 channels: none yet. */
static int audio_channels;
static int audio_rate = 48000;
static bool audio_non_pcm;
static DEFINE_MUTEX(audio_lock);

/* IEC 60958-3 sample frequency code (channel status byte 3, bits 3:0). */
static u8 iec60958_fs_code(int rate, bool hbr)
{
	if (hbr)
		return 0x09; /* 768 kHz */

	switch (rate) {
	case 32000: return 0x03;
	case 44100: return 0x00;
	case 88200: return 0x08;
	case 96000: return 0x0a;
	case 176400: return 0x0c;
	case 192000: return 0x0e;
	default: return 0x02; /* 48 kHz */
	}
}

static void setHdmiAudioConfigBasic(int channels, u8 fs, bool hbr)
{
	i2c_init(2);
	i2c_begin_block();
	i2c_write(0x62a0, 0x06);
	i2c_write(0x62a7, 0x13);
	/* 0x82 (stock) mis-recovers 192 kHz audio from DP */
	i2c_write(0x62ac, 0x8a);
	i2c_write(0x62cb, 0x02);
	i2c_write(0x62cb, 0x03);
	i2c_write(0x62cb, 0x00);
	i2c_write(0x70ad, 0x00);
	i2c_write(0x70af, 0x07);
	i2c_write(0x70a9, 0x5e);
	i2c_end_block();
	i2c_mask(0x70af, 0x06, 0x06);
	i2c_mask(0x70b3, fs, 0x0f);
	i2c_mask(0x70ae, 0x80, 0xe0);
	i2c_mask(0x70ae, max(1, channels - 1), 0x07);
	/* Bit 5: HBR audio sample packets */
	i2c_mask(0x70ac, hbr ? 0x21 : 0x01, 0x21);
	i2c_mask(0x70ab, 0x81, 0x89);
	i2c_mask(0x70a9, 0x08, 0x08);
	i2c_exec();
}

static void setAudioConfigAdditional(int channels, u8 fs, bool non_pcm)
{
	u8 val = 0x00;
	switch (channels) {
	case 3: val = 0x02; break;
	case 4: val = 0x03; break;
	case 5: val = 0x09; break;
	case 6: val = 0x0b; break;
	case 7: val = 0x11; break;
	case 8: val = 0x13; break;
	}
	i2c_init(2);
	/* IEC 60958 channel status, byte 0 bit 1: non-audio */
	i2c_mask(0x70b0, non_pcm ? 0x02 : 0x00, 0xff);
	i2c_mask(0x70b1, 0x79, 0xff);
	i2c_mask(0x70b2, 0x00, 0xff);
	i2c_mask(0x70b3, fs, 0xff);
	i2c_mask(0x70b4, 0x0b, 0x0f);
	i2c_mask(0x70b5, 0x00, 0xff);
	i2c_mask(0x70b6, 0x00, 0xff);
	i2c_begin_block();
	i2c_write(0x10e7, 0xff);
	i2c_write(0x7011, 0xa2);
	i2c_end_block();
	i2c_waitset(0x10e7, 0xa2);
	i2c_mask(0x7267, val, 0xff);
	i2c_write(0x7204, 0x10);
	i2c_waitclear(0x7204, 0x10);
	i2c_write(0x10e7, 0xff);
	i2c_mask(0x7203, 0x10, 0x10);
	i2c_delay(30);
	i2c_write(0x70a8, 0xc0);
	i2c_exec();
}

static void sceHdmiSetAudioConfigFlava3(int channels)
{
	bool hbr = audio_non_pcm && audio_channels == 8 && audio_rate == 192000;
	u8 fs;

	/* Use the stream format once the HDA driver has set one. A non-PCM
	 * stream is sent with the layout it was packed in by IEC 61937. */
	if (audio_channels)
		channels = audio_channels;
	fs = iec60958_fs_code(audio_rate, hbr);

	setHdmiAudioConfigBasic(channels, fs, hbr);
	setAudioConfigAdditional(channels, fs, audio_non_pcm);
}

static void initVerde(void)
{
	i2c_init(0);
	i2c_mask(0x105, 0x00, 0x03);
	i2c_mask(0x104, 0x00, 0x02);
	i2c_cmd_2_1(0x400, 0x00);
	i2c_cmd_2_1(0x3261, 0x02);
	i2c_begin_block();
	i2c_write(0x11, 0x03);
	i2c_write(0x13, 0xff);
	i2c_write_data(0x8000, (u8[]){0x00, 0x01, 0x07, 0x00}, 4);
	i2c_write(0x108, 0x80);
	i2c_write(0x9000, 0x01);
	i2c_end_block();
	i2c_cmd_5_3(0x00);
	i2c_exec();
}

static void linkConfigVerde(const struct drm_display_mode *mode)
{
	i2c_init(0);
	i2c_mask(0x105, 0x00, 0x40);
	if (mode->hdisplay == 2560 && mode->vdisplay == 1440) {
		i2c_cmd_2_1(0x400, 0x03);
	}
	i2c_begin_block();
	i2c_write_data(0x9003, (u8[]){0x1e, 0x04, 0x01, 0x01, 0x00, 0x00, 0x00}, 7);
	i2c_write(0x4002, 0x03);
	i2c_end_block();
	i2c_exec();
}

static void configDpLinkTrainingVerde(void)
{
	i2c_init(0);
	i2c_cmd_2_1(0x9002, 0x01);
	i2c_waitset(0x4001, 0x02);
	i2c_cmd_2_1(0x11, 0x01);
	i2c_exec();
}

static void setTmdsConfigVerde(void)
{
	i2c_init(0);
	i2c_cmd_2_1(0x9200, 0x00);
	i2c_begin_block();
	i2c_write(0x9800, 0x00);
	i2c_write(0x33b3, 0x1a);
	i2c_write(0x9831, 0x80);
	i2c_write_data(0x9810, (u8[]){0x00, 0x79, 0x00, 0x02, 0x0b, 0x00, 0x00}, 7);
	i2c_end_block();
	i2c_waitclear(0x9830, 0x80);
	i2c_cmd_2_1(0x9830, 0x80);
	i2c_waitclear(0x9830, 0x80);
	i2c_cmd_2_1(0x9830, 0x81);
	i2c_waitclear(0x9830, 0x80);
	i2c_waitclear(0x108, 0x10);
	i2c_mask(0x108, 0x11, 0x17);
	i2c_waitset(0x13, 0x40);
	i2c_cmd_2_1(0x13, 0x40);
	i2c_cmd_3_5(0x4001, 0x03);
	i2c_cmd_2_1(0x11, 0x01);
	i2c_exec();
}

static void setHdmiBasicVideoConfigVerde(const struct drm_display_mode *mode)
{
	u8 vic = drm_match_cea_mode(mode);

	i2c_init(0);
	i2c_begin_block();
	if (vic == 16) {
		i2c_write_data(0x3058, (u8[]){0x02, 0x0d, 0xef, 0x00, 0x28, 0x08, 0x10, 0x40, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, 18);
	} else if (mode->hdisplay == 2560 && mode->vdisplay == 1440) {
		i2c_write_data(0x3058, (u8[]){0x02, 0x0d, 0xff, 0x00, 0x28, 0x08, 0x00, 0x40, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, 18);
	} else if (vic == 97) {
		i2c_write_data(0x3058, (u8[]){0x02, 0x0d, 0x9e, 0x00, 0x28, 0x08, 0x61, 0x40, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, 18);
	}
	i2c_write_data(0x31d0, (u8[]){0x01, 0x19, 0x5f, 0x53, 0x43, 0x45, 0x49, 0x00, 0x00, 0x00, 0x00, 0x50, 0x53, 0x35, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x08}, 28);
	i2c_end_block();
	i2c_begin_block();
	i2c_write_data(0x3033, (u8[]){0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, 9);
	i2c_write_data(0x307a, (u8[]){0x01, 0x0a, 0x70, 0x01, 0x00, 0x00, 0x00, 0x00}, 8);
	i2c_write_data(0x3000, (u8[]){0x56, 0xaa, 0x00, 0x24, 0x00, 0xa8, 0x00, 0x00, 0x08, 0x80}, 10);
	i2c_end_block();
	i2c_cmd_2_1(0x300a, 0x01);
	i2c_waitclear(0x300c, 0xf3);
	i2c_waitclear(0x300d, 0xff);
	i2c_waitclear(0x300e, 0xff);
	i2c_waitclear(0x300f, 0xff);
	i2c_waitclear(0x3010, 0xff);
	i2c_waitclear(0x3011, 0xff);
	i2c_waitset(0x13, 0x80);
	i2c_cmd_2_1(0x13, 0x80);
	i2c_mask(0x105, 0x01, 0x03);
	i2c_delay(700);
	i2c_cmd_5_3(0x01);
	i2c_exec();
}

static void setHdmiVideoConfigVerde(const struct drm_display_mode *mode)
{
	linkConfigVerde(mode);
	configDpLinkTrainingVerde();
	setTmdsConfigVerde();
	setHdmiBasicVideoConfigVerde(mode);
}

static void setAudioConfigForVerde(int channels)
{
	i2c_init(2);
	i2c_waitclear(0x102, 0x80);
	i2c_cmd_2_1(0xf1, 0x01);
	i2c_waitset(0xf2, 0x01);
	i2c_waitset(0x16, 0x08);
	i2c_cmd_2_1(0x16, 0x08);
	i2c_begin_block();
	i2c_write(0x9800, 0x00);
	i2c_write(0x33b3, 0x1a);
	i2c_write(0x9831, 0x80);
	i2c_write_data(0x9810, (u8[]){0x00, 0x79, 0x00, 0x02, 0x0b, 0x00, 0x00}, 7);
	i2c_end_block();
	i2c_waitclear(0x9830, 0x80);
	i2c_cmd_2_1(0x9830, 0x80);
	i2c_waitclear(0x9830, 0x80);
	i2c_cmd_2_1(0x9830, 0x81);
	i2c_waitclear(0x9830, 0x80);
	i2c_waitclear(0x108, 0x10);
	i2c_mask(0x108, 0x10, 0x10);
	i2c_waitclear(0x108, 0x10);
	i2c_write_data(0x307a, (u8[]){0x01, 0x0a, 0x70, 0x01, 0x00, 0x00, 0x00, 0x00}, 8);
	i2c_mask(0x3000, 0x40, 0xc0);
	i2c_mask(0x3005, 0x80, 0xc0);
	i2c_cmd_2_1(0x300a, 0x01);
	i2c_waitset(0x13, 0x80);
	i2c_cmd_2_1(0x13, 0x80);
	i2c_exec();
}

static void sceHdmiSetAudioConfigVerde(int channels)
{
	setAudioConfigForVerde(channels);
}

void sceHdmiInitVideoConfig(void)
{
	stopHdcpHw();
	if (hdmi_ic_type == HDMI_IC_TYPE_FLAVA3) {
		sceDisableEncode();
		initIsrForFlava3();
		initDpForFlava3();
		configVSyncSettingFlava3();
	} else {
		initVerde();
	}
}
EXPORT_SYMBOL(sceHdmiInitVideoConfig);

void sceHdmiSetVideoConfig(const struct drm_display_mode *mode, unsigned int bpc, bool yuv422)
{
	if (hdmi_ic_type == HDMI_IC_TYPE_FLAVA3) {
		setHdmiVideoConfigFlava3(mode, bpc, yuv422);
	} else {
		setHdmiVideoConfigVerde(mode);
	}
}
EXPORT_SYMBOL(sceHdmiSetVideoConfig);

void sceHdmiSetHdrConfig(const struct hdmi_drm_infoframe *frame, bool bt2020)
{
	if (hdmi_ic_type == HDMI_IC_TYPE_FLAVA3) {
		setHdrConfigFlava3(frame, bt2020);
	}
}
EXPORT_SYMBOL(sceHdmiSetHdrConfig);

void sceHdmiDeviceSetVideoMute(int mute)
{
	i2c_init(4);
	if (hdmi_ic_type == HDMI_IC_TYPE_FLAVA3) {
		i2c_write(0x705f, (mute == 1) << 7);
	} else {
		i2c_mask(0x105, mute != 1, 0x03);
	}
	i2c_exec();
}
EXPORT_SYMBOL(sceHdmiDeviceSetVideoMute);

void sceHdmiSetAudioConfig(int channels)
{
	if (hdmi_ic_type == HDMI_IC_TYPE_FLAVA3) {
		mutex_lock(&audio_lock);
		sceHdmiSetAudioConfigFlava3(channels);
		mutex_unlock(&audio_lock);
	} else {
		sceHdmiSetAudioConfigVerde(channels);
	}
}
EXPORT_SYMBOL(sceHdmiSetAudioConfig);

/*
 * Called by the HDA driver when a stream is set up. The converter can't
 * learn the format from the DP audio stream, it has to be told.
 */
void sceHdmiSetAudioStream(int channels, int rate, bool non_pcm)
{
	if (hdmi_ic_type != HDMI_IC_TYPE_FLAVA3)
		return;

	mutex_lock(&audio_lock);
	if (channels != audio_channels || rate != audio_rate ||
	    non_pcm != audio_non_pcm) {
		audio_channels = channels;
		audio_rate = rate;
		audio_non_pcm = non_pcm;
		sceHdmiSetAudioConfigFlava3(channels);
	}
	mutex_unlock(&audio_lock);
}
EXPORT_SYMBOL(sceHdmiSetAudioStream);

void sceHdmiSetAudioMute(int mute)
{
	i2c_init(4);
	if (hdmi_ic_type == HDMI_IC_TYPE_FLAVA3) {
		i2c_mask(0x70a8, (mute == 1) << 2, 0x04);
	} else {
		i2c_cmd_5_5(mute == 1);
	}
	i2c_exec();
}
EXPORT_SYMBOL(sceHdmiSetAudioMute);

void sceHdmiOutputMode(void)
{
	i2c_init(4);
	if (hdmi_ic_type == HDMI_IC_TYPE_FLAVA3) {
		i2c_write(0x7005, 0x80);
	}
	i2c_exec();
}
EXPORT_SYMBOL(sceHdmiOutputMode);

int getHdmiConfiguration(void)
{
	u8 buf[ICC_MSG_MAX_SIZE] = {};
	struct icc_msg *msg = (struct icc_msg *)buf;
	int ret;

	msg->service_id = ICC_SERVICE_ID_GENERAL;
	msg->msg_type = 0x16;
	msg->length = ICC_MSG_MIN_SIZE;
	msg->data[0] = 0x10;

	ret = icc_query(buf, buf);
	if (ret)
		return ret;

	hdmi_ic_type = (msg->data[0x3] == 1) | 2;
	tmds_polarity = msg->data[0x4] == 1;
	tmds_ch_swap = msg->data[0x5] == 1;
	dp_polarity = msg->data[0x6];
	dp_ch_swap = msg->data[0x7];

	pr_info("HDMI IC Type [%s]\n", hdmi_ic_type == HDMI_IC_TYPE_FLAVA3 ? "FLAVA3" : "VERDE");
	pr_info("TMDS Polarity [%s]\n", tmds_polarity ? "Reverse" : "Normal");
	pr_info("TMDS Ch Swap [%s]\n", tmds_ch_swap ? "Swap" : "Normal");
	pr_info("DP Polarity [0x%x]\n", dp_polarity);
	pr_info("DP Ch Swap [0x%x]\n", dp_ch_swap);

	return 0;
}
EXPORT_SYMBOL(getHdmiConfiguration);

/* FLAVA3 audio is choppy with the default Naud, Maud is too coarse. */
u32 getHdmiDpAudioN(u32 default_n)
{
	if (hdmi_ic_type == HDMI_IC_TYPE_FLAVA3)
		return 0x80000;

	return default_n;
}
EXPORT_SYMBOL(getHdmiDpAudioN);

bool isHdmiModeValid(const struct drm_display_mode *mode, int force_1080p)
{
	u8 vic = drm_match_cea_mode(mode);

	if (force_1080p)
		return vic == 16; /* 1080p60 */

	if (is_extra_vic(vic))
		return true;

	/* 1440p */
	if (mode->hdisplay == 2560 && mode->vdisplay == 1440) {
		return mode->clock == 241500 || mode->clock == 241700;
	}

	switch (vic) {
	case 16: /* 1080p60 */
	case 63: /* 1080p120 */
	case 97: /* 2160p60 */
		return true;
	}

	return false;
}
EXPORT_SYMBOL(isHdmiModeValid);

bool isHdmiHdrSupported(void)
{
	return hdmi_ic_type == HDMI_IC_TYPE_FLAVA3;
}
EXPORT_SYMBOL(isHdmiHdrSupported);

const struct drm_edid *real_edid = NULL;

static void fix_edid(u8 *edid)
{
	int i;
	u32 sum = 0;

	/* For some reason, PS5 sets 0x01. */
	edid[0] = 0x00;
	for (i = 0; i < 0x7f; i++)
		sum += edid[i];
	edid[0x7f] = -sum & 0xff;
}

static DEFINE_MUTEX(hotplug_lock);
static void (*hotplug_handler)(void *data);
static void *hotplug_data;

void hdmi_set_hotplug_handler(void (*handler)(void *data), void *data)
{
	mutex_lock(&hotplug_lock);
	hotplug_handler = handler;
	hotplug_data = data;
	mutex_unlock(&hotplug_lock);
}
EXPORT_SYMBOL(hdmi_set_hotplug_handler);

void hdmi_notification_handler(struct icc_msg *msg)
{
		if (msg->data[1] == 0x02) {
			fix_edid(&msg->data[4]);
			real_edid = drm_edid_alloc(&msg->data[4], *(u16 *)&msg->data[2]);
			pr_info("got real edid\n");
			sceHdmiOutputMode();

			mutex_lock(&hotplug_lock);
			if (hotplug_handler)
				hotplug_handler(hotplug_data);
			mutex_unlock(&hotplug_lock);
		}
}
EXPORT_SYMBOL(hdmi_notification_handler);

/* Experiment interface: raw ICC messages from debugfs. */
static DEFINE_MUTEX(dbg_lock);
static u8 dbg_query[ICC_MSG_MAX_SIZE];
static u8 dbg_reply[ICC_MSG_MAX_SIZE];
static int dbg_ret = -ENODATA;

static ssize_t dbg_icc_write(struct file *file, const char __user *ubuf,
			     size_t len, loff_t *ppos)
{
	struct icc_msg *msg = (struct icc_msg *)dbg_query;
	char *text, *p, *tok;
	unsigned int service, type, val;
	size_t n = 0, total;
	int ret = 0;

	if (len > 8192)
		return -E2BIG;
	text = memdup_user_nul(ubuf, len);
	if (IS_ERR(text))
		return PTR_ERR(text);

	mutex_lock(&dbg_lock);
	memset(dbg_query, 0, sizeof(dbg_query));
	memset(dbg_reply, 0, sizeof(dbg_reply));

	p = text;
	tok = strsep(&p, " \t\n");
	if (!tok || kstrtouint(tok, 16, &service) || service > 0xff) {
		ret = -EINVAL;
		goto out;
	}
	do {
		tok = strsep(&p, " \t\n");
	} while (tok && !*tok);
	if (!tok || kstrtouint(tok, 16, &type) || type > 0xffff) {
		ret = -EINVAL;
		goto out;
	}
	while ((tok = strsep(&p, " \t\n"))) {
		if (!*tok)
			continue;
		if (kstrtouint(tok, 16, &val) || val > 0xff ||
		    sizeof(*msg) + n >= ICC_MSG_MAX_SIZE) {
			ret = -EINVAL;
			goto out;
		}
		msg->data[n++] = val;
	}

	total = sizeof(*msg) + n;
	msg->service_id = service;
	msg->msg_type = type;
	msg->length = max_t(size_t, total, ICC_MSG_MIN_SIZE);

	dbg_ret = icc_query(dbg_query, dbg_reply);
out:
	mutex_unlock(&dbg_lock);
	kfree(text);
	return ret ? ret : len;
}

static int dbg_icc_show(struct seq_file *m, void *unused)
{
	struct icc_msg *msg = (struct icc_msg *)dbg_reply;
	size_t len, i;

	mutex_lock(&dbg_lock);
	len = min_t(size_t, msg->length, ICC_MSG_MAX_SIZE);
	seq_printf(m, "ret=%d service=%02x type=%04x length=%zu\n",
		   dbg_ret, msg->service_id, msg->msg_type, len);
	for (i = 0; i < len; i++)
		seq_printf(m, "%02x%c", dbg_reply[i], (i % 16 == 15 || i + 1 == len) ? '\n' : ' ');
	mutex_unlock(&dbg_lock);
	return 0;
}

static int dbg_icc_open(struct inode *inode, struct file *file)
{
	return single_open(file, dbg_icc_show, NULL);
}

static const struct file_operations dbg_icc_fops = {
	.owner = THIS_MODULE,
	.open = dbg_icc_open,
	.read = seq_read,
	.write = dbg_icc_write,
	.llseek = seq_lseek,
	.release = single_release,
};

static int __init ps5_hdmi_debugfs_init(void)
{
	struct dentry *dir = debugfs_create_dir("ps5_hdmi", NULL);

	debugfs_create_file("icc", 0600, dir, NULL, &dbg_icc_fops);
	return 0;
}
late_initcall(ps5_hdmi_debugfs_init);
