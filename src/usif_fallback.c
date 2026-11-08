// SPDX-License-Identifier: GPL-2.0-only
/*
 * Lantiq VR9 USIF UART: bring-up fallback for a stock FRITZ!Box 7362 SL.
 *
 * OpenWrt's vr9.dtsi describes the USIF (usif@da00000, compatible
 * "lantiq,usif") but keeps it disabled, the 7362 SL board file does not
 * enable it, and the stock kernel has no device tree overlays. So on a
 * stock image nothing would bind the driver. On that one machine only, this
 * file creates the platform device itself, with the registers and
 * interrupts of the disabled node, and registers pinctrl mappings for the
 * two USIF UART pins (GPIO 11 RX, GPIO 12 TX). No other pin is touched.
 *
 * The device tree path is the real one: a board that enables the usif node
 * (with pinctrl for "usif uart_rx"/"usif uart_tx") makes this fallback step
 * aside. Modelled on the bring-up fallback of kmod-lantiq-ssc-pcm.
 */
#include <linux/device.h>
#include <linux/ioport.h>
#include <linux/kernel.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/of_irq.h>
#include <linux/pinctrl/machine.h>
#include <linux/pinctrl/pinctrl-state.h>
#include <linux/platform_device.h>

#include "usif_regs.h"
#include "usif_uart.h"

#define FB_MACHINE_COMPAT	"avm,fritz7362sl"
#define USIF_COMPAT		"lantiq,usif"
#define USIF_PHYS		0x1da00000
#define USIF_NR_IRQS		4	/* TX, RX, error, status */
#define XWAY_PINMUX_DEV		"1e100b10.pinmux"

/*
 * pinctrl-xway pad settings, packed as (parameter << 16) | argument
 * (drivers/pinctrl/pinctrl-lantiq.h): parameter 1 is open drain, 4 is the
 * pad direction (1 = output). The pin mux alone leaves the direction as it
 * was.
 */
#define XWAY_OPEN_DRAIN(on)	((1UL << 16) | (on))
#define XWAY_OUTPUT(on)		((4UL << 16) | (on))

static unsigned long usif_pad_in[] = { XWAY_OUTPUT(0) };
/*
 * TX pad: push-pull output (open drain argument 0 sets the OD bit), as the
 * stock board presumably drives it ([CR 7362SL/A-0004] section 5). Open
 * drain also worked at 230400 Bd thanks to the board's pull-up.
 */
static unsigned long usif_pad_tx[] = { XWAY_OPEN_DRAIN(0), XWAY_OUTPUT(1) };

static const struct pinctrl_map usif_fallback_maps[] = {
	/* "init", before probe: both pads inputs while the USIF is set up */
	PIN_MAP_CONFIGS_PIN(USIF_DEV_NAME, PINCTRL_STATE_INIT,
			    XWAY_PINMUX_DEV, "io11", usif_pad_in),
	PIN_MAP_CONFIGS_PIN(USIF_DEV_NAME, PINCTRL_STATE_INIT,
			    XWAY_PINMUX_DEV, "io12", usif_pad_in),
	/* "default", after a successful probe: the USIF drives the pads */
	PIN_MAP_MUX_GROUP(USIF_DEV_NAME, PINCTRL_STATE_DEFAULT,
			  XWAY_PINMUX_DEV, "usif uart_rx", "usif"),
	PIN_MAP_MUX_GROUP(USIF_DEV_NAME, PINCTRL_STATE_DEFAULT,
			  XWAY_PINMUX_DEV, "usif uart_tx", "usif"),
	PIN_MAP_CONFIGS_PIN(USIF_DEV_NAME, PINCTRL_STATE_DEFAULT,
			    XWAY_PINMUX_DEV, "io11", usif_pad_in),
	PIN_MAP_CONFIGS_PIN(USIF_DEV_NAME, PINCTRL_STATE_DEFAULT,
			    XWAY_PINMUX_DEV, "io12", usif_pad_tx),
	/* "sleep", selected by the driver on removal: both pads inputs */
	PIN_MAP_CONFIGS_PIN(USIF_DEV_NAME, PINCTRL_STATE_SLEEP,
			    XWAY_PINMUX_DEV, "io11", usif_pad_in),
	PIN_MAP_CONFIGS_PIN(USIF_DEV_NAME, PINCTRL_STATE_SLEEP,
			    XWAY_PINMUX_DEV, "io12", usif_pad_in),
};

static struct platform_device *usif_fallback_pdev;
static bool usif_fallback_mapped;

/* registers and interrupts of the (disabled) usif node */
static int usif_fallback_resources(struct device_node *np,
				   struct resource *res)
{
	unsigned int i;
	int ret;

	ret = of_address_to_resource(np, 0, &res[0]);
	if (ret)
		return ret;
	if (res[0].start != USIF_PHYS || resource_size(&res[0]) < USIF_MAP_SIZE)
		return -ENODEV;
	/*
	 * vr9.dtsi gives the node 16 MiB, up to 0x1E9FFFFF. Registering the
	 * device inserts its resource into the iomem tree, which would adopt
	 * the FPI drivers' busy regions inside that range (probe then fails
	 * with -EBUSY, and removing the device drops them from the tree).
	 * The USIF itself needs 1 MiB.
	 */
	res[0].end = res[0].start + USIF_MAP_SIZE - 1;

	for (i = 0; i < USIF_NR_IRQS; i++) {
		unsigned int irq = irq_of_parse_and_map(np, i);

		if (!irq)
			return -ENXIO;
		res[1 + i] = (struct resource)DEFINE_RES_IRQ(irq);
	}
	return 0;
}

int usif_fallback_init(void)
{
	struct resource res[1 + USIF_NR_IRQS] = { };
	struct platform_device *pdev;
	struct device_node *np;
	int ret;

	np = of_find_compatible_node(NULL, NULL, USIF_COMPAT);
	if (!np)
		return 0;
	if (of_device_is_available(np) ||
	    !of_machine_is_compatible(FB_MACHINE_COMPAT)) {
		of_node_put(np);
		return 0;	/* the device tree path applies, or not our box */
	}
	ret = usif_fallback_resources(np, res);
	of_node_put(np);
	if (ret) {
		pr_err(USIF_DRV_NAME ": usif node unusable: %d\n", ret);
		return ret;
	}

	ret = pinctrl_register_mappings(usif_fallback_maps,
					ARRAY_SIZE(usif_fallback_maps));
	if (ret)
		return ret;
	usif_fallback_mapped = true;

	pdev = platform_device_alloc(USIF_DEV_NAME, PLATFORM_DEVID_NONE);
	if (!pdev) {
		ret = -ENOMEM;
		goto err_maps;
	}
	ret = platform_device_add_resources(pdev, res, ARRAY_SIZE(res));
	if (!ret)
		ret = device_set_driver_override(&pdev->dev, USIF_DRV_NAME);
	if (!ret)
		ret = platform_device_add(pdev);
	if (ret) {
		platform_device_put(pdev);
		goto err_maps;
	}

	/* probe ran synchronously; make a failed probe fail the module load */
	if (!platform_get_drvdata(pdev)) {
		platform_device_unregister(pdev);
		ret = -ENODEV;
		goto err_maps;
	}
	usif_fallback_pdev = pdev;
	pr_info(USIF_DRV_NAME ": fallback device for " FB_MACHINE_COMPAT
		" registered\n");
	return 0;

err_maps:
	pinctrl_unregister_mappings(usif_fallback_maps);
	usif_fallback_mapped = false;
	return ret;
}

void usif_fallback_exit(void)
{
	if (usif_fallback_pdev) {
		platform_device_unregister(usif_fallback_pdev);
		usif_fallback_pdev = NULL;
	}
	if (usif_fallback_mapped) {
		pinctrl_unregister_mappings(usif_fallback_maps);
		usif_fallback_mapped = false;
	}
}
