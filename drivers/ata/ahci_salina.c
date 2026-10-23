// SPDX-License-Identifier: GPL-2.0-only
/*
 * PlayStation 5 Salina SATA / BD-ROM AHCI host driver
 *
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/module.h>
#include <linux/pci.h>
#include <linux/io.h>
#include <linux/dma-mapping.h>
#include <linux/pm.h>
#include <linux/libata.h>
#include <linux/ps5.h>
#include <scsi/scsi_host.h>

#include "ahci.h"
#include "ahci_salina_phy.h"

#define PCI_DEVICE_ID_SPCIE	0x9107

#define SALINA_ICC_DEV_BD	0x01

struct salina_ahci {
	struct salina_sata_phy	phy;
	struct pci_dev		*glue;
};

static int salina_glue_map(struct salina_ahci *sa)
{
	struct pci_dev *glue;

	glue = pci_get_device(PCI_VENDOR_ID_SONY, PCI_DEVICE_ID_SPCIE, NULL);
	if (!glue)
		return -ENODEV;

	if (!(pci_resource_flags(glue, 2) & IORESOURCE_MEM) ||
	    !(pci_resource_flags(glue, 4) & IORESOURCE_MEM)) {
		pci_dev_put(glue);
		return -ENODEV;
	}

	sa->phy.glue_phy = ioremap(pci_resource_start(glue, 2),
				   pci_resource_len(glue, 2));
	if (!sa->phy.glue_phy) {
		pci_dev_put(glue);
		return -ENOMEM;
	}

	sa->phy.glue_pcs = ioremap(pci_resource_start(glue, 4),
				   pci_resource_len(glue, 4));
	if (!sa->phy.glue_pcs) {
		iounmap(sa->phy.glue_phy);
		sa->phy.glue_phy = NULL;
		pci_dev_put(glue);
		return -ENOMEM;
	}

	sa->glue = glue;
	return 0;
}

static void salina_glue_unmap(struct salina_ahci *sa)
{
	if (sa->phy.glue_pcs) {
		iounmap(sa->phy.glue_pcs);
		sa->phy.glue_pcs = NULL;
	}
	if (sa->phy.glue_phy) {
		iounmap(sa->phy.glue_phy);
		sa->phy.glue_phy = NULL;
	}
	if (sa->glue) {
		pci_dev_put(sa->glue);
		sa->glue = NULL;
	}
}

static int salina_pick_bar(u16 devid, u32 chip_id, unsigned int *abar,
			   u32 *port_off)
{
	bool is_9106 = (devid == PCI_DEVICE_ID_AHCI_B);

	if (!is_9106 && chip_id == SALINA_CHIP_SALINA2)
		return -ENODEV;

	*abar = 0;
	*port_off = is_9106 ? 0x2000 : 0;
	return 0;
}

static struct ata_port_operations salina_ahci_ops = {
	.inherits = &ahci_ops,
};

static const struct ata_port_info salina_port_info = {
	.flags		= AHCI_FLAG_COMMON,
	.pio_mask	= ATA_PIO4,
	.udma_mask	= ATA_UDMA6,
	.port_ops	= &salina_ahci_ops,
};

static const struct scsi_host_template salina_ahci_sht = {
	AHCI_SHT(KBUILD_MODNAME),
};

static int salina_ahci_probe(struct pci_dev *pdev, const struct pci_device_id *id)
{
	struct device *dev = &pdev->dev;
	const struct ata_port_info *ppi[] = { &salina_port_info, NULL };
	struct ahci_host_priv *hpriv;
	struct ata_host *host;
	struct salina_ahci *sa;
	unsigned int abar, n_ports;
	u32 chip_id;
	u8 state;
	int rc;

	if (!spcie_is_initialized()) {
		dev_warn(dev, "spcie not initialized yet, deferring\n");
		return -EPROBE_DEFER;
	}

	rc = pcim_enable_device(pdev);
	if (rc)
		return rc;

	pci_set_master(pdev);

	rc = dma_set_mask_and_coherent(dev, DMA_BIT_MASK(32));
	if (rc)
		return rc;

	sa = devm_kzalloc(dev, sizeof(*sa), GFP_KERNEL);
	if (!sa)
		return -ENOMEM;

	rc = salina_glue_map(sa);
	if (rc) {
		dev_err(dev, "glue (104d:9107) map failed: %d\n", rc);
		return rc;
	}

	chip_id = spcie_get_chip_id();
	if (chip_id != SALINA_CHIP_SALINA && chip_id != SALINA_CHIP_SALINA2) {
		dev_err(dev, "unknown Salina chip id %#x\n", chip_id);
		rc = -ENODEV;
		goto err_glue;
	}

	rc = salina_pick_bar(pdev->device, chip_id, &abar, &sa->phy.port_off);
	if (rc) {
		dev_info(dev, "SALINA2 SATA0 dummy device, claiming but not attaching\n");
		pci_set_drvdata(pdev, NULL);
		salina_glue_unmap(sa);
		return 0;
	}

	rc = pcim_iomap_regions(pdev, BIT(abar), KBUILD_MODNAME);
	if (rc)
		goto err_glue;

	sa->phy.ctrl		= pcim_iomap_table(pdev)[abar];
	sa->phy.chip_id		= chip_id;
	sa->phy.devid		= (pdev->device << 16) | pdev->vendor;
	sa->phy.is_bd		= true;
	sa->phy.rx_tracelen	= 0xff;
	sa->phy.tx_tracelen	= 0xff;

	icc_device_power_get(SALINA_ICC_DEV_BD, &state);
	if (state != 1)
		icc_device_power_control(SALINA_ICC_DEV_BD, 1);

	rc = salina_sata_phy_init(&sa->phy);
	if (rc) {
		dev_err(dev, "Salina SATA PHY init failed: %d\n", rc);
		goto err_glue;
	}

	hpriv = devm_kzalloc(dev, sizeof(*hpriv), GFP_KERNEL);
	if (!hpriv) {
		rc = -ENOMEM;
		goto err_glue;
	}

	hpriv->mmio = sa->phy.ctrl + sa->phy.port_off;
	hpriv->plat_data = sa;
	hpriv->flags = AHCI_HFLAG_NO_PMP;

	ahci_save_initial_config(dev, hpriv);

	rc = pci_alloc_irq_vectors(pdev, 1, 1, PCI_IRQ_MSI | PCI_IRQ_INTX);
	if (rc < 0) {
		dev_err(dev, "irq alloc failed: %d\n", rc);
		goto err_glue;
	}
	hpriv->irq = pci_irq_vector(pdev, 0);

	n_ports = max(ahci_nr_ports(hpriv->cap), fls(hpriv->port_map));

	host = ata_host_alloc_pinfo(dev, ppi, n_ports);
	if (!host) {
		rc = -ENOMEM;
		goto err_glue;
	}
	host->private_data = hpriv;

	if (!(hpriv->cap & HOST_CAP_SSS))
		host->flags |= ATA_HOST_PARALLEL_SCAN;

	rc = ahci_reset_controller(host);
	if (rc)
		goto err_glue;

	ahci_init_controller(host);
	ahci_print_info(host, "Salina");

	pci_set_drvdata(pdev, host);

	dev_info(dev,
		 "Salina SATA up (chip %#x, devid %#x, BAR%u, port_off %#x, %u ports)\n",
		 sa->phy.chip_id, sa->phy.devid, abar, sa->phy.port_off, n_ports);

	return ahci_host_activate(host, &salina_ahci_sht);

err_glue:
	salina_glue_unmap(sa);
	return rc;
}

static void salina_ahci_remove(struct pci_dev *pdev)
{
	struct ata_host *host = pci_get_drvdata(pdev);
	struct ahci_host_priv *hpriv;
	struct salina_ahci *sa;

	if (!host)
		return;

	hpriv = host->private_data;
	sa = hpriv->plat_data;

	ata_host_detach(host);
	pci_free_irq_vectors(pdev);
	salina_glue_unmap(sa);
}

#ifdef CONFIG_PM_SLEEP
static int salina_ahci_suspend(struct device *dev)
{
	struct pci_dev *pdev = to_pci_dev(dev);
	struct ata_host *host = pci_get_drvdata(pdev);
	struct ahci_host_priv *hpriv;
	void __iomem *mmio;
	u32 ctl;
	int rc;

	if (!host)
		return 0;

	hpriv = host->private_data;
	mmio = hpriv->mmio;

	ctl = readl(mmio + HOST_CTL);
	ctl &= ~HOST_IRQ_EN;
	writel(ctl, mmio + HOST_CTL);
	readl(mmio + HOST_CTL);

	rc = ata_host_suspend(host, PMSG_SUSPEND);
	if (rc)
		return rc;

	pci_save_state(pdev);
	pci_set_power_state(pdev, PCI_D3hot);
	return 0;
}

static int salina_ahci_resume(struct device *dev)
{
	struct pci_dev *pdev = to_pci_dev(dev);
	struct ata_host *host = pci_get_drvdata(pdev);
	struct ahci_host_priv *hpriv;
	struct salina_ahci *sa;
	int rc;

	if (!host)
		return 0;

	hpriv = host->private_data;
	sa = hpriv->plat_data;

	rc = pci_set_power_state(pdev, PCI_D0);
	if (rc)
		return rc;
	pci_restore_state(pdev);
	pci_set_master(pdev);

	rc = salina_sata_phy_init(&sa->phy);
	if (rc) {
		dev_err(dev, "PHY re-init on resume failed: %d\n", rc);
		return rc;
	}

	rc = ahci_reset_controller(host);
	if (rc)
		return rc;

	ahci_init_controller(host);
	ata_host_resume(host);

	return 0;
}
#endif

static SIMPLE_DEV_PM_OPS(salina_ahci_pm, salina_ahci_suspend, salina_ahci_resume);

static const struct pci_device_id salina_ahci_tbl[] = {
	{ PCI_DEVICE(PCI_VENDOR_ID_SONY, PCI_DEVICE_ID_AHCI_A) },
	{ PCI_DEVICE(PCI_VENDOR_ID_SONY, PCI_DEVICE_ID_AHCI_B) },
	{ }
};
MODULE_DEVICE_TABLE(pci, salina_ahci_tbl);

static struct pci_driver salina_ahci_driver = {
	.name		= KBUILD_MODNAME,
	.id_table	= salina_ahci_tbl,
	.probe		= salina_ahci_probe,
	.remove		= salina_ahci_remove,
	.driver		= {
		.pm = &salina_ahci_pm,
	},
};

module_pci_driver(salina_ahci_driver);

MODULE_AUTHOR("Armandas Kvietkus");
MODULE_DESCRIPTION("PlayStation 5 Salina SATA / BD-ROM AHCI driver");
MODULE_LICENSE("GPL");
MODULE_SOFTDEP("pre: libahci spcie");
