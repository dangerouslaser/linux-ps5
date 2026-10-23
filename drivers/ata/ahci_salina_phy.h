/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _SALINA_SATA_PHY_H
#define _SALINA_SATA_PHY_H

#include <linux/types.h>
#include <linux/io.h>

#define PCI_DEVICE_ID_AHCI_A	0x9105
#define PCI_DEVICE_ID_AHCI_B	0x9106

#define SALINA_DEVID_A		0x9105104d
#define SALINA_DEVID_B		0x9106104d

#define SALINA_CHIP_SALINA	0x110000
#define SALINA_CHIP_SALINA2	0x120000

#define SALINA_GLUE_PHY_BASE	0x180000
#define SALINA_GLUE_PCS_BASE	0x4000

#define SALINA_EFUSE_TRIM	0x48
#define SALINA_EFUSE_VALID	0x6c
#define SALINA_EFUSE_VALID_A	0x40000
#define SALINA_EFUSE_VALID_B	0x4000000

#define SALINA_PHY_SEL_A	0x2c
#define SALINA_PHY_SEL_B	0x30

#define SALINA_PHY_RDY		BIT(0)
#define SALINA_PHY_RDY_REG	0xdc

struct salina_sata_phy {
	void __iomem	*ctrl;
	void __iomem	*glue_phy;
	void __iomem	*glue_pcs;
	u32		port_off;
	u32		chip_id;
	u32		devid;
	u32		rx_tracelen;
	u32		tx_tracelen;
	bool		is_bd;
};

int salina_sata_phy_init(struct salina_sata_phy *p);

#endif
