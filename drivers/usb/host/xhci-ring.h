#include "xhci.h"

dma_addr_t xhci_trb_virt_to_dma(struct xhci_segment *seg, union xhci_trb *trb);
int xhci_handle_events(struct xhci_hcd *xhci, struct xhci_interrupter *ir, bool skip_events);
