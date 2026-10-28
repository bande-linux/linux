// SPDX-License-Identifier: GPL-2.0-only OR MIT

#include <linux/console.h>
#include <linux/delay.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/mod_devicetable.h>
#include <linux/platform_device.h>
#include <linux/serial_core.h>

#include <asm/fixmap.h>

#include "hvc_console.h"

/* Data FIFO layout, from the m1n1 dockchannel UART and Asahi's dockchannel driver */
#define DATA_TX8	0x4
#define DATA_TX_FREE	0x14
#define DATA_RX8	0x1c
#define DATA_RX_COUNT	0x2c

#define TX_WAIT_US	10000

static void __iomem *dc_base;
static bool dc_stalled;

static size_t dc_tx(void __iomem *base, const u8 *buf, size_t count)
{
	size_t sent = 0;
	unsigned int waited = 0;

	while (sent < count) {
		u32 free = readl_relaxed(base + DATA_TX_FREE);

		if (!free) {
			if (dc_stalled || waited >= TX_WAIT_US) {
				dc_stalled = true;
				break;
			}
			udelay(1);
			waited++;
			continue;
		}

		dc_stalled = false;
		for (; free && sent < count; free--)
			writel_relaxed(buf[sent++], base + DATA_TX8);
	}

	return sent;
}

static ssize_t dc_put_chars(u32 vtermno, const u8 *buf, size_t count)
{
	return dc_tx(dc_base, buf, count);
}

static ssize_t dc_get_chars(u32 vtermno, u8 *buf, size_t count)
{
	size_t i;

	for (i = 0; i < count; i++) {
		if (!readl_relaxed(dc_base + DATA_RX_COUNT))
			break;
		buf[i] = readl_relaxed(dc_base + DATA_RX8) >> 8;
	}

	return i;
}

static const struct hv_ops dc_ops = {
	.get_chars = dc_get_chars,
	.put_chars = dc_put_chars,
};

static int dc_probe(struct platform_device *pdev)
{
	struct hvc_struct *hp;

	dc_base = devm_platform_ioremap_resource_byname(pdev, "data");
	if (IS_ERR(dc_base))
		return PTR_ERR(dc_base);

	hvc_instantiate(0, 0, &dc_ops);

	hp = hvc_alloc(0, 0, &dc_ops, 256);
	if (IS_ERR(hp))
		return PTR_ERR(hp);

	platform_set_drvdata(pdev, hp);
	return 0;
}

static const struct of_device_id dc_of_match[] = {
	{ .compatible = "apple,dockchannel-uart" },
	{}
};

static struct platform_driver dc_driver = {
	.driver = {
		.name = "hvc-dockchannel",
		.of_match_table = dc_of_match,
		.suppress_bind_attrs = true,
	},
	.probe = dc_probe,
};
builtin_platform_driver(dc_driver);

static void dc_early_write(struct console *con, const char *s, unsigned int n)
{
	struct earlycon_device *dev = con->data;
	const u8 cr = '\r';

	for (; n; n--, s++) {
		if (*s == '\n')
			dc_tx(dev->port.membase, &cr, 1);
		dc_tx(dev->port.membase, (const u8 *)s, 1);
	}
}

static int __init dc_early_setup(struct earlycon_device *device, const char *opt)
{
	if (!device->port.membase)
		return -ENODEV;

	/* Apple SoC MMIO outside PCIe faults on posted writes */
	__set_fixmap(FIX_EARLYCON_MEM_BASE, device->port.mapbase,
		     __pgprot(PROT_DEVICE_nGnRnE));

	device->con->write = dc_early_write;
	return 0;
}
EARLYCON_DECLARE(dockchannel, dc_early_setup);
