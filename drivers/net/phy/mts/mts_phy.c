// SPDX-License-Identifier: GPL-2.0-only
/*
 * PlayStation 5 Gigabit Ethernet driver
 *
 * Based on the MediaTek Star Ethernet MAC (mtk_star_emac).
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/kernel.h>
#include <linux/minmax.h>
#include <linux/delay.h>
#include <linux/mii.h>
#include <linux/netdevice.h>
#include <linux/ps5.h>
#include "mts.h"

/* bit 15 is write-arm on write and done-flag on read */
#define SMI_ARM		BIT(15)
#define SMI_DONE	BIT(15)
#define SMI_BUSY	BIT(15)
#define SMI_TIMEOUT	10000

static int gbe_smi_wait(struct gbe_priv *p)
{
	int i;

	for (i = 0; i < SMI_TIMEOUT; i++) {
		if (gbe_rd(p, GBE_REG_SMI) & SMI_DONE)
			return 0;
		udelay(1);
	}
	return -ETIMEDOUT;
}

/* plain clause-22 mii read */
static u16 gbe_cl22_read(struct gbe_priv *p, u8 reg)
{
	gbe_wr(p, GBE_REG_SMI, SMI_BUSY);
	gbe_wr(p, GBE_REG_SMI, ((reg & 0x1f) << 8) | 0x4000);
	if (gbe_smi_wait(p))
		return 0;
	return gbe_rd(p, GBE_REG_SMI) >> 16;
}

static void gbe_smi_write(struct gbe_priv *p, u8 reg, u16 val)
{
	gbe_wr(p, GBE_REG_SMI, SMI_BUSY);
	gbe_wr(p, GBE_REG_SMI, ((u32)val << 16) | ((reg & 0x1f) << 8) | 0x2000);
	gbe_smi_wait(p);
}

/* clause-45 is indirect, set the adress first then read/write the data */
static int gbe_cl45_addr(struct gbe_priv *p, u32 sel)
{
	gbe_wr(p, GBE_REG_SMI, SMI_BUSY);
	gbe_wr(p, GBE_REG_SMI, (sel & 0xffff0000) | ((sel & 0x1f) << 8) | 0x20);
	return gbe_smi_wait(p);
}

static void gbe_cl45_write(struct gbe_priv *p, u32 sel, u16 val)
{
	if (gbe_cl45_addr(p, sel))
		return;
	gbe_wr(p, GBE_REG_SMI, SMI_BUSY);
	gbe_wr(p, GBE_REG_SMI, ((u32)val << 16) | ((sel & 0x1f) << 8) | 0x60);
	gbe_smi_wait(p);
}

static u16 gbe_cl45_read(struct gbe_priv *p, u32 sel)
{
	if (gbe_cl45_addr(p, sel))
		return 0;
	gbe_wr(p, GBE_REG_SMI, SMI_BUSY);
	gbe_wr(p, GBE_REG_SMI, ((sel & 0x1f) << 8) | 0xe0);
	if (gbe_smi_wait(p))
		return 0;
	return gbe_rd(p, GBE_REG_SMI) >> 16;
}

/* PCS/SerDes regs live in Salina Glue BAR4 at offset GBE_PCS_BASE */
static u32 gbe_pcs_rd(struct gbe_priv *p, u32 off)
{
	return readl(p->glue_base + GBE_PCS_BASE + off);
}

/* maps a measured serdes field to its analog setting */
static const u8 gbe_cal_lut[64] = {
	0x7f, 0x7f, 0x7f, 0x7f, 0x7f, 0x7f, 0x7f, 0x7f,
	0x7f, 0x7f, 0x7f, 0x7f, 0x7f, 0x7f, 0x7f, 0x7f,
	0x7b, 0x7a, 0x75, 0x73, 0x70, 0x67, 0x64, 0x62,
	0x57, 0x55, 0x53, 0x51, 0x48, 0x46, 0x44, 0x42,
	0x40, 0x37, 0x35, 0x34, 0x32, 0x31, 0x30, 0x26,
	0x24, 0x23, 0x22, 0x21, 0x20, 0x16, 0x15, 0x14,
	0x13, 0x12, 0x11, 0x10, 0x07, 0x06, 0x05, 0x04,
	0x03, 0x02, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00,
};

static u8 gbe_cal_lut_idx(u32 field)
{
	field &= 0x7f;
	if (field < 2)
		field = 1;
	if (field >= 0x40)
		field = 0x40;
	return gbe_cal_lut[field - 1];
}

/* shove a cal value into the high or low byte of a lane reg */
static void gbe_cal_lane(struct gbe_priv *p, u32 sel, u8 cal, bool high)
{
	u16 cur = gbe_cl45_read(p, sel);

	if (high)
		gbe_cl45_write(p, sel, (cur & 0x00ff) | (cal << 8) | 0x8000);
	else
		gbe_cl45_write(p, sel, (cur & 0xff00) | cal | 0x80);
}

static u16 gbe_eq_preset(u32 field)
{
	return (min_t(u32, field & 0x3f, 0x3c) + 3) & 0xff;
}

/* feed the measured serdes status back into the analog tunning regs :) */
static void gbe_phy_calibrate(struct gbe_priv *p)
{
	u32 v;

	v = gbe_pcs_rd(p, 0x68);
	gbe_cl45_write(p, 0x000e001e, (v & 0x3f) << 8);
	v = gbe_pcs_rd(p, 0x68);
	gbe_cl45_write(p, 0x0115001f, (v >> 6) & 7);

	v = gbe_pcs_rd(p, 0x60);
	gbe_cal_lane(p, 0x0174001e, gbe_cal_lut_idx(v), true);
	v = gbe_pcs_rd(p, 0x60);
	gbe_cal_lane(p, 0x0174001e, gbe_cal_lut_idx(v >> 0x13), false);
	v = gbe_pcs_rd(p, 0x64);
	gbe_cal_lane(p, 0x0175001e, gbe_cal_lut_idx(v >> 6), true);
	v = gbe_pcs_rd(p, 0x64);
	gbe_cal_lane(p, 0x0175001e, gbe_cal_lut_idx(v >> 0x19), false);

	v = gbe_pcs_rd(p, 0x5c);
	gbe_cl45_write(p, 0x0172001e,
		       (gbe_cl45_read(p, 0x0172001e) & 0xc0ff) | ((v >> 0xc) & 0x3f00));
	v = gbe_pcs_rd(p, 0x60);
	gbe_cl45_write(p, 0x0172001e,
		       (gbe_cl45_read(p, 0x0172001e) & 0xffc0) | ((v >> 7) & 0x3f));
	v = gbe_pcs_rd(p, 0x60);
	gbe_cl45_write(p, 0x0173001e,
		       (gbe_cl45_read(p, 0x0173001e) & 0xc0ff) | ((v >> 0x12) & 0x3f00));
	v = gbe_pcs_rd(p, 0x64);
	gbe_cl45_write(p, 0x0173001e,
		       (gbe_cl45_read(p, 0x0173001e) & 0xffc0) | ((v >> 0xd) & 0x3f));

	v = gbe_pcs_rd(p, 0x5c) >> 0x1a;
	gbe_cl45_write(p, 0x0012001e, (v << 0xa) | v);
	gbe_cl45_write(p, 0x0016001e, (gbe_eq_preset(v) << 0xa) | v);
	v = gbe_pcs_rd(p, 0x60) >> 0xd;
	gbe_cl45_write(p, 0x0017001e, ((v & 0x3f) << 8) | (v & 0x3f));
	gbe_cl45_write(p, 0x0018001e, (gbe_eq_preset(v) << 8) | (v & 0x3f));
	v = gbe_pcs_rd(p, 0x64);
	gbe_cl45_write(p, 0x0019001e, ((v & 0x3f) << 8) | (v & 0x3f));
	gbe_cl45_write(p, 0x0020001e, (gbe_eq_preset(v) << 8) | (v & 0x3f));
	v = gbe_pcs_rd(p, 0x64) >> 0x13;
	gbe_cl45_write(p, 0x0021001e, ((v & 0x3f) << 8) | (v & 0x3f));
	gbe_cl45_write(p, 0x0022001e, (gbe_eq_preset(v) << 8) | (v & 0x3f));

	gbe_cl45_write(p, 0x0096001e, 0x8000);
	gbe_cl45_write(p, 0x0037001e, 0x0033);
	gbe_cl45_write(p, 0x0039001e, gbe_cl45_read(p, 0x0039001e) & 0xb7ff);
	gbe_cl45_write(p, 0x0107001f, gbe_cl45_read(p, 0x0107001f) & 0xefff);
	gbe_cl45_write(p, 0x0171001e, gbe_cl45_read(p, 0x0171001e) | 0x0180);
	gbe_cl45_write(p, 0x0039001e, gbe_cl45_read(p, 0x0039001e) | 0x2000);
	gbe_cl45_write(p, 0x0039001e, gbe_cl45_read(p, 0x0039001e) & 0xdfff);
	udelay(50);
	gbe_cl45_write(p, 0x0171001e, gbe_cl45_read(p, 0x0171001e) & 0xfe7f);
}

/* poking a paged register block needs this 3-write form, comes up alot */
static void gbe_smi_paged(struct gbe_priv *p, u16 a, u16 b, u16 c)
{
	u16 page = gbe_cl22_read(p, 0x1f);

	gbe_smi_write(p, 0x1f, 0x52b5);
	gbe_smi_write(p, 0x11, a);
	gbe_smi_write(p, 0x12, b);
	gbe_smi_write(p, 0x10, c);
	gbe_smi_write(p, 0x1f, page);
}

/* split version of the above for when the final reg 0x10 write is conditional */
static u16 gbe_paged_begin(struct gbe_priv *p, u16 r11, u16 r12)
{
	u16 page = gbe_cl22_read(p, 0x1f);

	gbe_smi_write(p, 0x1f, 0x52b5);
	gbe_smi_write(p, 0x11, r11);
	gbe_smi_write(p, 0x12, r12);
	return page;
}

static void gbe_paged_finish(struct gbe_priv *p, u16 r10, u16 page)
{
	gbe_smi_write(p, 0x10, r10);
	gbe_smi_write(p, 0x1f, page);
}

/*
 * Analog PHY tuning sequence - pile of magic values that make link work.
 * 4-way branch on (chip_id, revision_id), not sure which ps5 rev is which:
 *   0x110000 / 0x0100  path A  PS5
 *   0x110000 / 0x0200  path C  PS5
 *   0x110000 / other   path B  PS5 (other rev)
 *   != 0x110000        path D  other chip
 */
static void gbe_phy_static_init(struct gbe_priv *p)
{
	u32 chip, rev;

	gbe_cl45_write(p, 0x003e001e, 0xf8f8);
	gbe_cl45_write(p, 0x0189001e, 0x0110);
	gbe_smi_paged(p, 0xb90a, 0x006f, 0x8f82);
	gbe_smi_paged(p, 0xbaef, 0x002e, 0x968c);

	/* page-3 LED config */
	gbe_smi_write(p, 0x1f, 0x0003);
	gbe_smi_write(p, 0x1c, 0x0c92);
	gbe_smi_write(p, 0x1f, 0x0000);

	gbe_wr(p, GBE_REG_RX_PCODE + 4, 25000000);

	gbe_cl45_write(p, 0x0122001e, 0xffff);
	gbe_cl45_write(p, 0x0234001e, 0x0180);
	gbe_smi_paged(p, 0x2e00, 0x000e, 0x8fb0);

	gbe_cl45_write(p, 0x0238001e, 0x0120);
	if (spcie_get_chip_id() == 0x110000)
		gbe_cl45_write(p, 0x0120001e, 0x9014);
	gbe_cl45_write(p, 0x0239001e, 0x0117);
	gbe_smi_paged(p, 0x0001, 0x0004, 0x96a2);

	gbe_clr(p, GBE_REG_RX_PCODE, BIT(0));

	gbe_cl45_write(p, 0x003c0007, 0x0000);
	gbe_cl45_write(p, 0x0033001e, gbe_cl45_read(p, 0x0033001e) & 0xefff);
	gbe_cl45_write(p, 0x0268001f, 0x07f4);

	gbe_smi_paged(p, 0x0004, 0x0000, 0x9686);
	gbe_smi_paged(p, 0x0671, 0x0006, 0x8fae);
	gbe_smi_paged(p, 0x55a0, 0x0000, 0x83aa);

	gbe_cl45_write(p, 0x0123001e, 0xffff);

	gbe_smi_paged(p, 0x8670, 0x0001, 0x96a6);
	gbe_smi_paged(p, 0x0072, 0x0000, 0x96b6);
	gbe_smi_paged(p, 0x3210, 0x0000, 0x96b8);
	gbe_smi_paged(p, 0x024a, 0x0000, 0x96a8);

	chip = spcie_get_chip_id();
	rev = spcie_get_revision_id();
	netdev_info(p->netdev, "PHY tune: chip=%#x rev=%#x\n", chip, rev);

	if (chip == 0x110000) {
		if (rev == 0x0100) {
			u16 page;

			netdev_info(p->netdev, "PHY tune: path A (0x110100)\n");
			gbe_smi_paged(p, 0x704d, 0x0000, 0x9698);
			gbe_smi_paged(p, 0x314f, 0x0002, 0x969a);
			page = gbe_paged_begin(p, 0x4444, 0x0044);
			gbe_paged_finish(p, 0x8ecc, page);
		} else if (rev == 0x0200) {
			u16 page;

			netdev_info(p->netdev, "PHY tune: path C (0x110200)\n");
			gbe_smi_paged(p, 0x5010, 0x0000, 0x96a0);
			gbe_smi_paged(p, 0x3028, 0x0000, 0x969e);
			gbe_smi_paged(p, 0x504d, 0x0000, 0x9698);
			page = gbe_paged_begin(p, 0x194f, 0x0002);
			gbe_paged_finish(p, 0x969a, page);
		} else {
			/* path B: PS5 - this rev skips the final reg 0x10 write */
			netdev_info(p->netdev, "PHY tune: path B (subsys %#x)\n", rev);
			gbe_smi_paged(p, 0x5010, 0x0000, 0x96a0);
			gbe_smi_paged(p, 0x3028, 0x0000, 0x969e);
			gbe_smi_paged(p, 0x504d, 0x0000, 0x9698);
			gbe_smi_paged(p, 0x194f, 0x0002, 0x969a);
			gbe_cl45_write(p, 0x014a001e, 0xee20);
			gbe_cl45_write(p, 0x019b001e, 0x0111);
			goto skip_path_d_continuation;
		}
	} else {
		u16 page;

		netdev_info(p->netdev, "PHY tune: path D (chip %#x)\n", chip);
		gbe_smi_paged(p, 0x5010, 0x0000, 0x96a0);
		gbe_smi_paged(p, 0x3028, 0x0000, 0x969e);
		gbe_smi_paged(p, 0x504d, 0x0000, 0x9698);
		gbe_smi_paged(p, 0x194f, 0x0002, 0x969a);

		gbe_cl45_write(p, 0x014a001e, 0xee20);
		gbe_cl45_write(p, 0x019b001e, 0x0111);
		gbe_cl45_write(p, 0x0144001e, 0x0200);

		gbe_smi_write(p, 0x1f, 0x0003);
		gbe_smi_write(p, 0x1d, 0x03fb);
		gbe_smi_write(p, 0x1f, 0x0000);

		gbe_cl45_write(p, 0x0323001e, 0x0011);
		gbe_smi_paged(p, 0x0036, 0x0000, 0x8f80);

		gbe_cl45_write(p, 0x0120001e, 0x8014);

		page = gbe_paged_begin(p, 0xff3f, 0x0000);
		gbe_paged_finish(p, 0x83ae, page);

		gbe_cl45_write(p, 0x0000001e, 0x0186);
		gbe_cl45_write(p, 0x0001001e, 0x01c5);
		gbe_cl45_write(p, 0x0002001e, 0x01c8);
		gbe_cl45_write(p, 0x0003001e, 0x010e);
		gbe_cl45_write(p, 0x0004001e, 0x0202);
		gbe_cl45_write(p, 0x0005001e, 0x0207);
		gbe_cl45_write(p, 0x0006001e, 0x0386);
		gbe_cl45_write(p, 0x0007001e, 0x03c5);
		gbe_cl45_write(p, 0x0008001e, 0x03c8);
		gbe_cl45_write(p, 0x0009001e, 0x030a);
		gbe_cl45_write(p, 0x000a001e, 0x0005);
		gbe_cl45_write(p, 0x000b001e, 0x0008);
	}

skip_path_d_continuation:
	gbe_cl45_write(p, 0x003e001e, 0x0000);
}

int gbe_phy_init(struct gbe_priv *p)
{
	u16 bmcr;

	gbe_cl22_read(p, MII_PHYSID1);
	gbe_cl22_read(p, MII_PHYSID2);

	gbe_wr(p, GBE_REG_RESET, 9);
	usleep_range(680, 1000);

	/* both SerDes lanes must be locked before calibraton is valid */
	if (~gbe_pcs_rd(p, 0x6c) & 0x80800000)
		netdev_info(p->netdev, "SerDes not locked, skipping calibration\n");
	else
		gbe_phy_calibrate(p);

	gbe_phy_static_init(p);

	{
		u16 adv = gbe_cl22_read(p, MII_ADVERTISE);

		gbe_smi_write(p, MII_ADVERTISE, adv & 0xf3ff);
	}

	/* clear bits 12-13 and bit 31 in LINK reg to let the link state machien settle */
	{
		u32 v = gbe_rd(p, GBE_REG_LINK);

		gbe_wr(p, GBE_REG_LINK, v & 0x7fffcfff);
	}

	gbe_cl22_read(p, MII_ADVERTISE);
	gbe_cl22_read(p, MII_CTRL1000);
	gbe_smi_write(p, MII_BMCR, BMCR_PDOWN);

	{
		u16 adv = gbe_cl22_read(p, MII_ADVERTISE);

		adv = (adv & 0xfe1f) | 0x01e0;
		gbe_smi_write(p, MII_ADVERTISE, adv);
	}
	{
		u16 c1000 = gbe_cl22_read(p, MII_CTRL1000);

		c1000 = (c1000 & 0xfcff) | 0x0200;
		gbe_smi_write(p, MII_CTRL1000, c1000);
	}
	gbe_smi_write(p, MII_BMCR, 0x1340);

	bmcr = gbe_cl22_read(p, MII_BMCR);
	gbe_smi_write(p, MII_BMCR, bmcr | (BMCR_ANENABLE | BMCR_ANRESTART));

	{
		u16 id1 = gbe_cl22_read(p, MII_PHYSID1);
		u16 id2 = gbe_cl22_read(p, MII_PHYSID2);
		u16 b_bmcr = gbe_cl22_read(p, MII_BMCR);
		u16 b_bmsr = gbe_cl22_read(p, MII_BMSR);
		u16 b_adv  = gbe_cl22_read(p, MII_ADVERTISE);
		u16 b_c1k  = gbe_cl22_read(p, MII_CTRL1000);

		netdev_info(p->netdev,
			"phy id=%04x:%04x bmcr=%04x bmsr=%04x adv=%04x ctrl1000=%04x link_reg=%08x\n",
			id1, id2, b_bmcr, b_bmsr, b_adv, b_c1k,
			gbe_rd(p, GBE_REG_LINK));
	}

	return 0;
}
