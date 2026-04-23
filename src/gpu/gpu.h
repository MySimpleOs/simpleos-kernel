#pragma once

/* High-level GPU probe: walks the PCI bus, identifies every display-class
 * device, and routes to a vendor-specific driver when available. For now
 * the only driver that actually touches the hardware is VirtIO-GPU
 * (Faz 11.1+). Intel and Nvidia devices are logged; drivers land in
 * ROADMAP §1. */
void gpu_init(void);
