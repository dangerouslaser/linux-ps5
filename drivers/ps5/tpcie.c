// SPDX-License-Identifier: GPL-2.0-only

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/console.h>
#include <linux/module.h>
#include <linux/msi.h>
#include <linux/pci.h>
#include <linux/ps5.h>
#include <linux/serial_core.h>
#include <linux/tty_flip.h>

#define PCI_DEVICE_ID_TPCIE	0x90ec
#define TPCIE_SUBFUNC_UART	6
#define NUM_IRQS		8

#define UART_OFFSET		0x1010100

struct tpcie_dev;

struct tpcie_uart_dev {
	struct pci_dev *pdev;
	struct tpcie_dev *tdev;
	void __iomem *uart_base;
	struct tty_driver *tty_drv;
	struct tty_port tty_port;
};

struct tpcie_dev {
	struct pci_dev *pdev;
	void __iomem *bar4;
	struct tpcie_uart_dev *uart_dev;
};

static struct tpcie_uart_dev *uart_dev;

static bool tpcie_uart_rx_ready(void)
{
	return (readl(uart_dev->uart_base + 0xc) & 0x10) != 0;
}

static bool tpcie_uart_tx_ready(void)
{
	return (readl(uart_dev->uart_base + 0xc) & 0x800) == 0;
}

static void tpcie_uart_putc(char c)
{
	unsigned timeout = 0xffff;
	while (!tpcie_uart_tx_ready() && --timeout)
		cpu_relax();
	writel(c, uart_dev->uart_base + 0x4);
}

static void tpcie_uart_write(struct console *con, const char *s, unsigned int n)
{
	int i;
	for (i = 0; i < n; ++i) {
		if (s[i] == '\n')
			tpcie_uart_putc('\r');
		tpcie_uart_putc(s[i]);
	}
}

static ssize_t tpcie_uart_tty_write(struct tty_struct *tty, const u8 *buf, size_t count)
{
	int i;
	for (i = 0; i < count; i++) {
		tpcie_uart_putc(buf[i]);
	}
	tty_port_tty_wakeup(&uart_dev->tty_port);
	return count;
}

static unsigned int tpcie_uart_tty_write_room(struct tty_struct *tty)
{
	return 2048;
}

static int tpcie_uart_tty_open(struct tty_struct *tty, struct file *file)
{
	return tty_port_open(&uart_dev->tty_port, tty, file);
}

static void tpcie_uart_tty_close(struct tty_struct *tty, struct file *file)
{
	tty_port_close(&uart_dev->tty_port, tty, file);
}

static void tpcie_uart_tty_hangup(struct tty_struct *tty)
{
	tty_port_hangup(&uart_dev->tty_port);
}

static struct tty_operations tpcie_uart_tty_ops = {
	.open = tpcie_uart_tty_open,
	.close = tpcie_uart_tty_close,
	.write = tpcie_uart_tty_write,
	.write_room = tpcie_uart_tty_write_room,
	.hangup = tpcie_uart_tty_hangup,
};

static const struct tty_port_operations tpcie_uart_port_ops = {};

static struct tty_driver *tpcie_uart_device(struct console *c, int *index)
{
	*index = 0;
	return uart_dev->tty_drv;
}

static struct console tpcie_uart_console = {
	.name = "ttyTitania",
	.device = tpcie_uart_device,
	.write = tpcie_uart_write,
	.flags = CON_PRINTBUFFER,
	.index = -1,
};

static irqreturn_t uart_interrupt(int irq, void *dev)
{
	u8 ch;
	while (tpcie_uart_rx_ready()) {
		ch = readl(uart_dev->uart_base + 0x0);
		tty_insert_flip_char(&uart_dev->tty_port, ch, TTY_NORMAL);
	}
	tty_flip_buffer_push(&uart_dev->tty_port);
	return IRQ_HANDLED;
}

static void tcpie_config_msi(struct tpcie_dev *tdev, int subfunc, struct msi_msg *msg)
{
	writel(0xffffffff, tdev->bar4 + 0x120214);
	writel(0, tdev->bar4 + 0x12020c);

	writel(msg->address_lo, tdev->bar4 + 0x120204);
	writel(msg->address_hi, tdev->bar4 + 0x120208);

	writel(msg->data & ~0x1f, tdev->bar4 + 0x120210);
	if (subfunc == TPCIE_SUBFUNC_UART) {
		writel(msg->data & 0x1f, tdev->bar4 + 0x120230);
	}
	writel(msg->data & 0x1f, tdev->bar4 + 0x120210 + subfunc * 4);

	writel(0x33, tdev->bar4 + 0x120200);
}

static int tpcie_uart_init(struct tpcie_dev *tdev)
{
	struct tty_driver *drv;
	struct msi_msg msg;
	int vector, ret;

	uart_dev = devm_kzalloc(&tdev->pdev->dev, sizeof(*uart_dev), GFP_KERNEL);
	if (!uart_dev)
		return -ENOMEM;

	uart_dev->pdev = tdev->pdev;
	uart_dev->tdev = tdev;
	tdev->uart_dev = uart_dev;
	uart_dev->uart_base = tdev->bar4 + UART_OFFSET;

	drv = tty_alloc_driver(1, TTY_DRIVER_REAL_RAW);
	if (IS_ERR(drv))
		return PTR_ERR(drv);

	drv->driver_name = "tpcie_uart";
	drv->name = "ttyTitania";
	drv->type = TTY_DRIVER_TYPE_SERIAL;
	drv->major = 0;
	drv->minor_start = 0;
	drv->num = 1;
	drv->subtype = SERIAL_TYPE_NORMAL;
	drv->flags = TTY_DRIVER_RESET_TERMIOS | TTY_DRIVER_REAL_RAW;
	drv->init_termios = tty_std_termios;
	tty_set_operations(drv, &tpcie_uart_tty_ops);
	uart_dev->tty_drv = drv;

	tty_port_init(&uart_dev->tty_port);
	uart_dev->tty_port.ops = &tpcie_uart_port_ops;
	tty_port_link_device(&uart_dev->tty_port, drv, 0);

	ret = tty_register_driver(drv);
	if (ret) {
		tty_driver_kref_put(drv);
		return ret;
	}

	register_console(&tpcie_uart_console);

	/* Request IRQ for uart subfunc */
	vector = pci_irq_vector(tdev->pdev, TPCIE_SUBFUNC_UART);
	ret = devm_request_irq(&tdev->pdev->dev, vector, uart_interrupt, 0, "uart", uart_dev);
	if (ret)
		goto err_unreg_tty;

	/* Special MSI configuration */
	ret = irq_chip_compose_msi_msg(irq_get_irq_data(vector), &msg);
	if (ret)
		goto err_unreg_tty;

	tcpie_config_msi(tdev, TPCIE_SUBFUNC_UART, &msg);

	/* Enable IRQs */
	writel(0x10, uart_dev->uart_base + 0x8);

	return 0;

err_unreg_tty:
	unregister_console(&tpcie_uart_console);
	tty_unregister_driver(drv);
	tty_driver_kref_put(drv);
	return ret;
}

static void tpcie_uart_remove(struct tpcie_dev *tdev)
{
	unregister_console(&tpcie_uart_console);

	if (uart_dev->tty_drv) {
		tty_unregister_driver(uart_dev->tty_drv);
		tty_driver_kref_put(uart_dev->tty_drv);
	}

	tty_port_destroy(&uart_dev->tty_port);
}

static void tpcie_uart_shutdown(struct tpcie_dev *tdev)
{
	/* Disable IRQs */
	writel(0, uart_dev->uart_base + 0x8);
}

static int tpcie_probe(struct pci_dev *pdev, const struct pci_device_id *id)
{
	struct tpcie_dev *tdev;
	int ret;

	ret = pcim_enable_device(pdev);
	if (ret)
		return ret;

	tdev = devm_kzalloc(&pdev->dev, sizeof(*tdev), GFP_KERNEL);
	if (!tdev)
		return -ENOMEM;

	tdev->pdev = pdev;
	tdev->bar4 = pcim_iomap(pdev, 4, 0);
	if (!tdev->bar4)
		return -ENOMEM;

	pci_set_master(pdev);

	ret = pci_alloc_irq_vectors(pdev, NUM_IRQS, NUM_IRQS, PCI_IRQ_MSI);
	if (ret < 0)
		return ret;

	writel(0xf, tdev->bar4 + 0x1142000 + 0x100);
	writel(0xf, tdev->bar4 + 0x1142000 + 0x104);

	ret = tpcie_uart_init(tdev);
	if (ret) {
		pci_free_irq_vectors(pdev);
		return ret;
	}

	pci_set_drvdata(pdev, tdev);
	return 0;
}

static void tpcie_remove(struct pci_dev *pdev)
{
	struct tpcie_dev *tdev = pci_get_drvdata(pdev);
	if (tdev) {
		tpcie_uart_remove(tdev);
		pci_free_irq_vectors(pdev);
	}
}

static void tpcie_shutdown(struct pci_dev *pdev)
{
	struct tpcie_dev *tdev = pci_get_drvdata(pdev);
	if (tdev) {
		tpcie_uart_shutdown(tdev);
	}
}

static const struct pci_device_id tpcie_ids[] = {
	{ PCI_DEVICE(PCI_VENDOR_ID_SONY, PCI_DEVICE_ID_TPCIE) },
	{ 0, }
};
MODULE_DEVICE_TABLE(pci, tpcie_ids);

static struct pci_driver tpcie_driver = {
	.name		= "tpcie",
	.id_table	= tpcie_ids,
	.probe		= tpcie_probe,
	.remove		= tpcie_remove,
	.shutdown	= tpcie_shutdown,
};

module_pci_driver(tpcie_driver);

MODULE_AUTHOR("Andy Nguyen");
MODULE_DESCRIPTION("PlayStation 5 Titania PCI Express glue driver");
MODULE_LICENSE("GPL");
