/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Lantiq VR9 USIF UART: definitions shared between the driver and the
 * bring-up fallback.
 */
#ifndef USIF_UART_H
#define USIF_UART_H

/* platform driver name */
#define USIF_DRV_NAME		"lantiq-usif-uart"

/*
 * Device name of the VR9 USIF, as the kernel names it from the device tree
 * (usif@da00000 below the FPI bus at 0x10000000). The SoC's clock lookup
 * keys the USIF gate on this name, so the fallback uses it too.
 */
#define USIF_DEV_NAME		"1da00000.usif"

int usif_fallback_init(void);
void usif_fallback_exit(void);

#endif /* USIF_UART_H */
