#include <linux/pci.h>
#include "xhci.h"

int xhci_salina_reset(struct xhci_hcd *xhci, u64 timeout_us);
int xhci_salina_init(struct pci_dev *pdev);
void xhci_salina_exit(void);

void salina_start_xhci_poller(struct xhci_hcd *xhci);
void salina_stop_xhci_poller(void);