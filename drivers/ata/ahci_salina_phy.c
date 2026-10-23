// SPDX-License-Identifier: GPL-2.0-only
#include <linux/kernel.h>
#include <linux/delay.h>
#include <linux/device.h>
#include "ahci_salina_phy.h"

static inline u32 phy_rd(struct salina_sata_phy *p, u32 off)
{
	return readl(p->ctrl + off);
}

static inline void phy_wr(struct salina_sata_phy *p, u32 off, u32 val)
{
	writel(val, p->ctrl + off);
}

static inline void phy_rmw(struct salina_sata_phy *p, u32 off, u32 keep, u32 set)
{
	phy_wr(p, off, (phy_rd(p, off) & keep) | set);
}

static inline u32 port_rd(struct salina_sata_phy *p, u32 off)
{
	return readl(p->ctrl + p->port_off + off);
}

static inline void port_wr(struct salina_sata_phy *p, u32 off, u32 val)
{
	writel(val, p->ctrl + p->port_off + off);
}

static inline void lane_sel(struct salina_sata_phy *p, u32 sel, u32 val)
{
	writel(val, p->glue_phy + SALINA_GLUE_PHY_BASE + sel);
}

static inline u32 efuse_rd(struct salina_sata_phy *p, u32 off)
{
	return readl(p->glue_pcs + SALINA_GLUE_PCS_BASE + off);
}

static u32 trace_len(struct salina_sata_phy *p, u32 raw)
{
	u32 def = p->is_bd ? 10 : 4;

	if (raw == 0 || raw > 0xfe)
		return def;
	return (raw >> 5 & 7) + (raw & 0x1f);
}

int salina_sata_phy_init(struct salina_sata_phy *p)
{
	u32 trim[6];
	u32 efuse, valid, sel, lane;
	u32 rx, tx;
	u32 a, b, c, d, e, f, rc;
	bool tx_hi;

	sel = (p->devid != SALINA_DEVID_A) ? SALINA_PHY_SEL_B : SALINA_PHY_SEL_A;
	lane_sel(p, sel, 2);
	lane_sel(p, sel, 3);
	lane_sel(p, sel, 1);

	efuse = efuse_rd(p, SALINA_EFUSE_TRIM);
	valid = efuse_rd(p, SALINA_EFUSE_VALID);

	trim[0] = trim[1] = trim[2] = trim[3] = 0x10;
	trim[4] = trim[5] = 0x28;

	if (valid & SALINA_EFUSE_VALID_A) {
		trim[4] = efuse & 0x3f;
		trim[2] = efuse >> 6 & 0x1f;
		trim[0] = efuse >> 0xb & 0x1f;
	}
	if (valid & SALINA_EFUSE_VALID_B) {
		trim[5] = efuse >> 0x10 & 0x3f;
		trim[3] = efuse >> 0x16 & 0x1f;
		trim[1] = efuse >> 0x1b;
	}

	lane = (p->devid != SALINA_DEVID_A) ? 1 : 0;

	phy_rmw(p, 0xa0, 0xfbff03ff, (trim[lane + 4] << 10) | 0x4000000);
	phy_rmw(p, 0x14, ~0u, 0x100000);
	phy_rmw(p, 0x54, 0xfffff07f, trim[lane + 2] << 7);
	phy_rmw(p, 0x1c, ~0u, 4);
	phy_rmw(p, 0x78, 0xfffffe0f, trim[lane] << 4);

	rx = trace_len(p, p->rx_tracelen);
	tx = trace_len(p, p->tx_tracelen);

	tx_hi = tx > 5;
	if (tx_hi) {
		c = 0x2000;
		if (tx < 9) {
			b = 0x1000000;
			a = 0x200000;
		} else {
			a = (tx <= 0xc) ? 0x230000 : 0x260000;
			b = (tx > 0xc) ? 0x9000000 : 0x5000000;
		}
	} else {
		b = 0;
		a = (tx >= 3) ? 0x1e0000 : 0x1d0000;
		c = (tx < 3) ? 0x4000 : 0x2000;
	}

	e = (tx > 8) ? 0x4000 : 0;
	f = (tx > 8) ? 0x80 : 0;
	d = e | 0x12000;
	rc = f | 0x900;
	e += 0x6000;
	f += 0x8c0;
	if (tx < 6) {
		u32 ge3 = (tx >= 3) ? 1 : 0;

		d = ge3 * 0x1000 + 0xd000;
		rc = (ge3 * 0x40) | 0x880;
		e = ge3 << 0xd;
		f = ge3 * 0x40 + 0x800;
	}

	phy_rmw(p, 0x4c, 0xffc0ffff, a);
	phy_rmw(p, 0x4c, 0xc0ffffff, b);
	phy_rmw(p, 0x54, 0xffff9fff, c);
	phy_rmw(p, 0x7c, 0xfffff03f, f);
	phy_rmw(p, 0x7c, 0xfffc0fff, e);
	phy_rmw(p, 0x5c, 0x0fffffff, (tx < 3) ? 0x60000000 : 0x50000000);
	phy_rmw(p, 0x80, 0xfffff03f, rc);
	phy_rmw(p, 0x80, 0xfffc0fff, d);
	phy_rmw(p, 0x4c, 0xfffffff0, tx_hi ? 5 : 3);

	{
		u32 rx_coef, rx_amp, rx_eq;

		if (rx < 3) {
			rx_coef = 0x100;
			rx_amp = 0x40;
			rx_eq = 2;
		} else if (rx < 6) {
			rx_coef = 0x100;
			rx_amp = 0x50;
			rx_eq = 3;
		} else {
			rx_amp = 0x60;
			rx_coef = 0x200;
			rx_eq = 5;
		}
		phy_rmw(p, 0x6c, 0xfffff0ff, rx_coef);
		phy_rmw(p, 0x84, 0xffffff00, rx_amp | rx_eq);
	}

	phy_rmw(p, 0x40, 0xffffffe0, 0x12);
	phy_rmw(p, 0x40, 0xffffc0ff, 0x3100);
	phy_rmw(p, 0x40, 0xffe0ffff, 0xe0000);
	phy_rmw(p, 0x40, 0xffffff1f, 0x80);
	phy_rmw(p, 0x3c, 0x1fffefff, 0xa0000000);
	phy_rmw(p, 0x44, 0xffffef80, 0x23);
	phy_rmw(p, 0x1c, 0xff0fffff, 0x200000);
	phy_rmw(p, 0xdc, 0xffffe0ff, 0x400);
	phy_rmw(p, 0x24, ~0u, 0x30);

	lane_sel(p, sel, 0);

	{
		int i;

		for (i = 0; i < 100; i++) {
			if (port_rd(p, SALINA_PHY_RDY_REG) & SALINA_PHY_RDY)
				break;
			udelay(10);
		}
		if (i == 100)
			return -ETIMEDOUT;
	}

	port_wr(p, 0, port_rd(p, 0) & 0xe7ffffff);
	port_wr(p, 0xc, 1);
	port_wr(p, 0xb8, port_rd(p, 0xb8) | 0x20000);
	port_wr(p, 0x118, (port_rd(p, 0x118) & 0xffe3ffff) | 0x40000);

	return 0;
}
