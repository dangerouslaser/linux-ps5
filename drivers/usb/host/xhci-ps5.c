/*
* Playstation 5 Salina XHCI driver helper
* Author: https://github.com/c0w-ar
*/

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/pci.h>
#include <linux/types.h>
#include <linux/io.h>
#include <linux/usb.h>
#include <linux/usb/hcd.h>
#include <linux/workqueue.h>
#include <asm/delay.h>
#include "xhci-ps5.h"
#include "xhci.h"
#include "xhci-trace.h"
#include "xhci-ring.h"

/* External SPCIE driver functions */
extern bool spcie_is_initialized(void);
extern u32 spcie_get_chip_id(void);
extern u32 spcie_bar2_180000_read(u32 reg);
extern void spcie_bar2_180000_write(u32 reg, u32 val);
extern u32 spcie_pervasive0_4000_read(u32 reg);

#define SALINA_XHCI_BAR_SIZE            0x140000
#define SALINA_POLL_INTERVAL_MS         20

struct salina_xhci_dev {
	void __iomem *spcie_bar2;
	void __iomem *spcie_pervasive0;
    void __iomem *xhci_bar;
    u32 revision;
};
static struct salina_xhci_dev salina_xhci;

struct salina_xhci_poller {
    struct xhci_hcd *xhci;
    struct delayed_work work;
    bool running;
};
static struct salina_xhci_poller salina_poll;

static int dev_ioremap_bar(struct pci_dev *dev, int bar_nr, resource_size_t min_size, void __iomem **bar) {
    resource_size_t start = pci_resource_start(dev, bar_nr);
    resource_size_t len = pci_resource_len(dev, bar_nr);

    if (start == 0) {
        return -EINVAL;
    }

    if (len < min_size) {
        return -ENODEV;
    }

    *bar = ioremap(start, min_size);
    if (!*bar) {
        pr_err("Could not ioremap memory\n");
        release_mem_region(start, min_size);
        return -ENOMEM;
    }

    return 0;
}

static void unmap_bar(void) {
    if(salina_xhci.xhci_bar) {
        iounmap(salina_xhci.xhci_bar);
        salina_xhci.xhci_bar = NULL;
    }
}

static int init_bar(struct pci_dev *own_pdev) {
    int ret = dev_ioremap_bar(own_pdev, 0, SALINA_XHCI_BAR_SIZE, &salina_xhci.xhci_bar);
    if (ret)
        goto unmap;
    return 0;

unmap:
    unmap_bar();
    return ret;
}

static u32 phys_readl(u64 addr) {
    void __iomem *ptr = ioremap(addr, 4);
    if (!ptr) {
        pr_err("Could not ioremap memory\n");
        return 0;
    }
    u32 val = readl(ptr);
    iounmap(ptr);
    return val;
}

static u8 usb_get_length(u8 usb_n) {
    return (u8)((phys_readl(0x9bf44) >> (usb_n*8))&0x1f);
}

inline void spcie_phy_bit_set(u32 reg, u32 bit) {
    u32 val_old = spcie_bar2_180000_read(reg);
    spcie_bar2_180000_write(reg, val_old | BIT(bit));
}

inline void spcie_phy_bit_unset(u32 reg, u32 bit) {
    u32 val_old = spcie_bar2_180000_read(reg);
    spcie_bar2_180000_write(reg, val_old & ~BIT(bit));
}

inline u32 spcie_config_read(u32 reg) {
    return spcie_pervasive0_4000_read(reg);
}

inline u32 xhci_read(u64 offset, u32 reg) {
    if (salina_xhci.xhci_bar)
        return readl(salina_xhci.xhci_bar + offset + reg);
    else
        return -1;
}

inline void xhci_write(u64 offset, u32 reg, u32 val) {
    if (salina_xhci.xhci_bar)
        writel(val, salina_xhci.xhci_bar + offset + reg);
}

inline void xhci_or(u64 offset, u32 reg, u32 val) {
    if (salina_xhci.xhci_bar) {
        u32 val_old = readl(salina_xhci.xhci_bar + offset + reg);
        writel(val_old | val, salina_xhci.xhci_bar + offset + reg);
    }
}

inline void xhci_and(u64 offset, u32 reg, u32 val) {
    if (salina_xhci.xhci_bar) {
        u32 val_old = readl(salina_xhci.xhci_bar + offset + reg);
        writel(val_old & val, salina_xhci.xhci_bar + offset + reg);
    }
}

inline void xhci_bit_set(u64 offset, u32 reg, u32 bit) {
    if (salina_xhci.xhci_bar) {
        u32 val_old = readl(salina_xhci.xhci_bar + offset + reg);
        writel(val_old | BIT(bit), salina_xhci.xhci_bar + offset + reg);
    }
}

inline void xhci_bit_unset(u64 offset, u32 reg, u32 bit) {
    if (salina_xhci.xhci_bar) {
        u32 val_old = readl(salina_xhci.xhci_bar + offset + reg);
        writel(val_old & ~BIT(bit), salina_xhci.xhci_bar + offset + reg);
    }
}

inline void xhci_bit_toggle(u64 offset, u32 reg, u32 bit) {
    xhci_bit_set(offset, reg, bit);
    xhci_bit_unset(offset, reg, bit);
}

static inline u64 usb_phy_offset (u8 usb_n) {
    switch (usb_n) {
        case 0: return 0x120000;
        case 1: return 0x124000;
        case 2: return 0x128000;
        default: return 0x120000;
    }
}

static inline u32 usb_trace_trim(u8 usb_n, u32 usb_length) {
    if (usb_length < 3)
        return 8;
    else if (usb_length < 6)
        return 7;
    else if (usb_length < 8)
        return 6;
    else if (usb_length < 10)
        return 5;
    else if (usb_length < 13)
        return 4;
    else {
        switch (usb_n) {
            case 0: return 4;
            case 1: return 6;
            case 2: return 7;
            default: return 4;
        }
    }
}

static void salina_usb_phy_config_step_1(u8 usb_n) {

    u64 offset = usb_phy_offset(usb_n);
    u32 val32;
    u32 usb_efuse;
    u32 usb_length;

    val32 = spcie_config_read(0x6c);
    if ((val32 >> 0x19 & val32 >> 0x11 & 1) == 0) {
        printk("[USB%d] eFuse not blown for usb\n", usb_n);
        usb_efuse = 0x98;
    }
    else {
        usb_efuse = (spcie_config_read(0x44) >> (usb_n*5+0x10)) & 0x1f;
    }

	xhci_write(offset, 0x800, 0x480a);

    xhci_write(offset, 0x814, 0x10a01000);

    xhci_bit_unset(offset, 0x818, 23);

    xhci_bit_set(offset, 0x800, 5);

    xhci_bit_unset(offset, 0x868, 18);

    xhci_and(offset, 0x804, 0xff07ffff);
    xhci_or(offset, 0x804, usb_efuse << 0x13);

    usb_length = usb_trace_trim(usb_n, usb_get_length(usb_n));

    xhci_and(offset, 0x818, 0xffffff0f);
    xhci_or(offset, 0x818, 0xa0);

    xhci_and(offset, 0x818, 0xfffffff0);
    xhci_or(offset, 0x818, usb_length);
}

static int salina_usb_phy_config_step_2(u8 usb_n) {

    u64 offset = usb_phy_offset(usb_n);

    xhci_bit_set(offset, 0x814, 15);

    udelay(2);

    xhci_bit_set(offset, 0x110, 8);

    xhci_and(offset, 0x100, 0xff000000);
    xhci_or(offset, 0x100, 0x400);

    xhci_bit_set(offset, 0x100, 24);

    int attemps = 100;
    u32 measure;
    do {
        measure = xhci_read(offset, 0x10c);
        udelay(2);
        if (measure!=0) break;
        attemps--;
    } while(attemps!=0);

    xhci_bit_unset(offset, 0x100, 24);

    xhci_bit_unset(offset, 0x110, 8);

    if (!measure) {
        printk("Something went wrong while waiting for USB init\n");
        return -1;
    }

    u32 calibration = (u32)((0xb9000 / (u64)measure) / 1000);

    xhci_and(offset, 0x814, 0xffff8fff);
    xhci_or(offset, 0x814, calibration<<12);

    xhci_bit_unset(offset, 0x814, 15);

    return 0;
}

static void salina_usb_phy_config_step_3(u8 usb_n) {
    u32 offset = usb_phy_offset(usb_n);
    xhci_bit_set(offset, 0x800, 0);
    xhci_bit_unset(offset, 0x808, 17);
    xhci_bit_set(offset, 0x880, 0);
}

static void salina_usb_phy_config_step_4(void) {
    xhci_and(0, 0xc100, 0xfffffff0);
    xhci_or(0, 0xc100, 0xe);
    xhci_write(0, 0xc12c, 0x7fc0c010);
    xhci_write(0, 0xc200, 0x1548);
    if (salina_xhci.revision == 0x110000) {
        xhci_write(0, 0xc204, 0x1548);
        xhci_write(0, 0xc208, 0x1548);
    }
}

static int xhci_salina_phy_init(void) {

    spcie_phy_bit_set(0x24, 1);
    spcie_phy_bit_set(0x24, 0);
    spcie_phy_bit_unset(0x24, 1);

    salina_usb_phy_config_step_1(0);
    if (salina_xhci.revision == 0x110000) {
        salina_usb_phy_config_step_1(1);
        salina_usb_phy_config_step_1(2);
    }

    spcie_phy_bit_unset(0x24, 0);

    udelay(2000);

    if(salina_usb_phy_config_step_2(0)) return -1;
    if (salina_xhci.revision == 0x110000) {
        if(salina_usb_phy_config_step_2(1)) return -1;
        if(salina_usb_phy_config_step_2(2)) return -1;
    }

    salina_usb_phy_config_step_3(0);
    if (salina_xhci.revision == 0x110000) {
        salina_usb_phy_config_step_3(1);
        salina_usb_phy_config_step_3(2);
    }

    xhci_bit_unset(0x100000, 0x4, 0);

    salina_usb_phy_config_step_4();

    return 0;
}

int xhci_salina_init(struct pci_dev *pdev) {

    int ret;

	if (!spcie_is_initialized()) {
		dev_warn(&pdev->dev, "spcie not initialized, deferring\n");
		return -EPROBE_DEFER;
	}

    salina_xhci.revision = spcie_get_chip_id();
    if (salina_xhci.revision!=0x110000 && salina_xhci.revision!=0x120000) {
        dev_warn(&pdev->dev, "PS5 Salina revision unknown %08x\n", salina_xhci.revision);
        return ENODEV;
    }

    ret = init_bar(pdev);
    if (ret) {
        pr_err("PS5 Salina could not map BAR\n");
        return ret;
    }

    xhci_salina_phy_init();

    dev_info(&pdev->dev, "PS5 Salina xHCI PHY initialized\n");
    return 0;
}

int xhci_salina_reset(struct xhci_hcd *xhci, u64 timeout_us) {

	u32 state;
	int ret;

    if (salina_xhci.revision == 0x110000) {
        xhci_bit_unset(0x100000, 0x0, 2);
    }

	state = readl(&xhci->op_regs->status);

	if (state == ~(u32)0) {
		if (!(xhci->xhc_state & XHCI_STATE_DYING))
			xhci_warn(xhci, "Host not accessible, reset failed.\n");
		return -ENODEV;
	}

	if ((state & STS_HALT) == 0) {
		xhci_warn(xhci, "Host controller not halted, aborting reset.\n");
		return 0;
	}

	xhci_info(xhci, "// Reset the HC on custom Salina Reset");
	writel(CMD_RESET, &xhci->op_regs->command);

    udelay(1000);

    ret = xhci_handshake(&xhci->op_regs->command, CMD_RESET, 0, timeout_us);
	if (ret)
		return ret;

    ret = xhci_handshake(&xhci->op_regs->status, STS_CNR, 0, timeout_us);
	if (ret)
		return ret;

    if (salina_xhci.revision == 0x110000) {
        xhci_bit_toggle(0x100000, 0x0, 1);
        xhci_bit_toggle(0x100000, 0x0, 17);
        xhci_bit_toggle(0x100000, 0x0, 25);
        udelay(10000);
        udelay(10000);
        udelay(10000);
    }

    xhci_bit_unset(0x100000, 0x0, 1);
    udelay(10000);
    udelay(10000);

    if (salina_xhci.revision == 0x110000) {
        xhci_bit_set(0x100000, 0x0, 2);
        udelay(10000);
        udelay(10000);
    }

    salina_usb_phy_config_step_4();

    // Reset software state
    xhci->usb2_rhub.bus_state.port_c_suspend = 0;
	xhci->usb2_rhub.bus_state.suspended_ports = 0;
	xhci->usb2_rhub.bus_state.resuming_ports = 0;
	xhci->usb3_rhub.bus_state.port_c_suspend = 0;
	xhci->usb3_rhub.bus_state.suspended_ports = 0;
	xhci->usb3_rhub.bus_state.resuming_ports = 0;

    return 0;
}

void xhci_salina_exit(void)
{
    unmap_bar();
}

static bool ring_has_event_pending(struct xhci_ring *ring)
{
	struct xhci_segment *seg;
	u32 idx;
	u32 ccs;

	if (!ring || !ring->deq_seg || !ring->dequeue)
		return false;

	seg = ring->deq_seg;
	idx = ring->dequeue - ring->deq_seg->trbs;
	ccs = ring->cycle_state;

	if (idx >= TRBS_PER_SEGMENT)
		return false;

	u32 trb_cycle = le32_to_cpu(seg->trbs[idx].generic.field[3]) & TRB_CYCLE;
	return trb_cycle == ccs;
}

static void salina_xhci_poll_fn(struct work_struct *w)
{
    struct salina_xhci_poller *p =
        container_of(w, struct salina_xhci_poller, work.work);
    struct xhci_hcd *xhci = p->xhci;
    struct xhci_ring *er;
	unsigned long flags;

    if (!p->running || !xhci)
        return;

    er = xhci->interrupters[0]->event_ring;
    if (!er)
        goto resched;

    if (!spin_trylock_irqsave(&xhci->lock, flags)) {
        printk(KERN_INFO "Salina poller: lock busy, rescheduling\n");
        goto resched;
    }

    u32 status = readl(&xhci->op_regs->status);
    if (status == ~(u32)0) {
        xhci_hc_died(xhci);
        goto unlock;
    }

    if (status & STS_HCE) {
        xhci_warn(xhci, "WARNING: Host Controller Error\n");
        xhci_halt(xhci);
        goto unlock;
    }

    if (status & STS_FATAL) {
        xhci_warn(xhci, "WARNING: Host System Error\n");
        xhci_halt(xhci);
        goto unlock;
    }

    if (ring_has_event_pending(er)) {
        xhci_handle_events(xhci, xhci->interrupters[0], false);
    }

unlock:
    spin_unlock_irqrestore(&xhci->lock, flags);

resched:
    schedule_delayed_work(&p->work,
        msecs_to_jiffies(SALINA_POLL_INTERVAL_MS));
}

void salina_start_xhci_poller(struct xhci_hcd *xhci)
{
    if (salina_poll.running)
		return;

    memset(&salina_poll, 0, sizeof(salina_poll));
    salina_poll.xhci = xhci;
    salina_poll.running = true;

    INIT_DELAYED_WORK(&salina_poll.work, salina_xhci_poll_fn);

    schedule_delayed_work(&salina_poll.work,
                  msecs_to_jiffies(SALINA_POLL_INTERVAL_MS));

    xhci_info(xhci, "Salina: software IRQ poller enabled\n");
}

void salina_stop_xhci_poller(void)
{
    if (!salina_poll.running)
		return;
    cancel_delayed_work_sync(&salina_poll.work);
    salina_poll.running = false;
    salina_poll.xhci = NULL;
}
