// SPDX-License-Identifier: GPL-2.0 OR MIT
/*
 * ASMedia xHCI firmware loader
 * Copyright (C) The Asahi Linux Contributors
 *
 * Apple silicon Macs have ASMedia ASM2214A controllers (they report the
 * ASM2142 PCI ID) without a flash ROM. They run a minimal ROM firmware until
 * the host uploads the real firmware into their code RAM, which macOS does
 * at startup. The register sequence below was reverse-engineered from macOS
 * by the Asahi Linux project; the register names are guesses and the purpose
 * of some steps is not known.
 */

#include <linux/bitfield.h>
#include <linux/firmware.h>
#include <linux/iopoll.h>
#include <linux/module.h>
#include <linux/pci.h>
#include <linux/sizes.h>
#include <linux/unaligned.h>
#include <linux/usb.h>
#include <linux/usb/hcd.h>

#include "xhci.h"
#include "xhci-pci.h"

#define ASMEDIA_APPLE_FW_NAME		"asmedia/asm2214a-apple.bin"

/* Configuration space registers */
#define ASMT_CFG_CONTROL		0xe0
#define ASMT_CFG_CONTROL_WRITE		BIT(1)
#define ASMT_CFG_CONTROL_READ		BIT(0)

#define ASMT_CFG_SRAM_ADDR		0xe2

#define ASMT_CFG_SRAM_ACCESS		0xef
#define ASMT_CFG_SRAM_ACCESS_ENABLE	BIT(7)

#define ASMT_CFG_DATA_READ0		0xf0
#define ASMT_CFG_DATA_READ1		0xf4

#define ASMT_CFG_DATA_WRITE0		0xf8
#define ASMT_CFG_DATA_WRITE1		0xfc

/* Mailbox command that returns the running firmware version */
#define ASMT_CMD_GET_FWVER		0x8000060840ULL
/* Version reported by the ROM firmware of the Apple parts */
#define ASMT_FWVER_ROM			0x010250090816ULL

/* BAR0 registers */
#define ASMT_REG_ADDR			0x3000

#define ASMT_REG_WDATA			0x3004
#define ASMT_REG_RDATA			0x3008

#define ASMT_REG_STATUS			0x3009
#define ASMT_REG_STATUS_BUSY		BIT(7)

#define ASMT_REG_CODE_WDATA		0x3010

#define ASMT_MMIO_CPU_MISC		0x500e
#define ASMT_MMIO_CPU_MISC_CODE_RAM_WR	BIT(0)

#define ASMT_MMIO_CPU_MODE_NEXT		0x5040

#define ASMT_MMIO_CPU_MODE_RAM		BIT(0)
#define ASMT_MMIO_CPU_MODE_HALFSPEED	BIT(1)

#define ASMT_MMIO_CPU_EXEC_CTRL		0x5042
#define ASMT_MMIO_CPU_EXEC_CTRL_RESET	BIT(0)
#define ASMT_MMIO_CPU_EXEC_CTRL_HALT	BIT(1)

/* The code RAM is written in two interleaved halves of this many words */
#define ASMT_FW_HALF_WORDS		0x4000
/* The code RAM address is 16 bits and advances by 2 per 32-bit write */
#define ASMT_FW_MAX_SIZE		SZ_128K

#define ASMT_TIMEOUT_USEC		10000
#define ASMT_RESET_TIMEOUT_USEC		500000

static int asmedia_mbox_tx(struct pci_dev *pdev, u64 data)
{
	int ret, err;
	u8 op;

	ret = read_poll_timeout(pci_read_config_byte, err,
				err || !(op & ASMT_CFG_CONTROL_WRITE),
				1, ASMT_TIMEOUT_USEC, false, pdev, ASMT_CFG_CONTROL,
				&op);
	if (ret) {
		pci_err(pdev, "Timed out on mailbox tx: 0x%llx\n", data);
		return ret;
	}
	if (err)
		return pcibios_err_to_errno(err);

	pci_write_config_dword(pdev, ASMT_CFG_DATA_WRITE0, lower_32_bits(data));
	pci_write_config_dword(pdev, ASMT_CFG_DATA_WRITE1, upper_32_bits(data));
	pci_write_config_byte(pdev, ASMT_CFG_CONTROL, ASMT_CFG_CONTROL_WRITE);

	return 0;
}

static int asmedia_mbox_rx(struct pci_dev *pdev, u64 *data)
{
	u32 low, high;
	int ret, err;
	u8 op;

	ret = read_poll_timeout(pci_read_config_byte, err,
				err || (op & ASMT_CFG_CONTROL_READ),
				1, ASMT_TIMEOUT_USEC, false, pdev, ASMT_CFG_CONTROL,
				&op);
	if (ret) {
		pci_err(pdev, "Timed out on mailbox rx\n");
		return ret;
	}
	if (err)
		return pcibios_err_to_errno(err);

	err = pci_read_config_dword(pdev, ASMT_CFG_DATA_READ0, &low);
	if (err)
		return pcibios_err_to_errno(err);
	err = pci_read_config_dword(pdev, ASMT_CFG_DATA_READ1, &high);
	if (err)
		return pcibios_err_to_errno(err);
	pci_write_config_byte(pdev, ASMT_CFG_CONTROL, ASMT_CFG_CONTROL_READ);

	*data = ((u64)high << 32) | low;
	return 0;
}

static int asmedia_get_fw_version(struct pci_dev *pdev, u64 *version)
{
	u64 cmd;
	int err;

	err = asmedia_mbox_tx(pdev, ASMT_CMD_GET_FWVER);
	if (err)
		return err;
	err = asmedia_mbox_tx(pdev, 0);
	if (err)
		return err;

	err = asmedia_mbox_rx(pdev, &cmd);
	if (err)
		return err;
	err = asmedia_mbox_rx(pdev, version);
	if (err)
		return err;

	if (cmd != ASMT_CMD_GET_FWVER) {
		pci_err(pdev, "Unexpected reply command 0x%llx\n", cmd);
		return -EIO;
	}

	return 0;
}

/* Returns 1 if RAM firmware is running, 0 if only the ROM, or -errno */
static int asmedia_check_firmware(struct pci_dev *pdev)
{
	u64 fwver;
	int ret;

	ret = asmedia_get_fw_version(pdev, &fwver);
	if (ret)
		return ret;

	pci_dbg(pdev, "Firmware version 0x%llx\n", fwver);

	return fwver != ASMT_FWVER_ROM;
}

/*
 * After a reset of the controller's CPU, the firmware sets USBCMD.HCRST until
 * it is up again. Toggling the SRAM access bit is known to unstick it.
 */
static int asmedia_wait_reset(struct pci_dev *pdev, void __iomem *regs)
{
	struct xhci_cap_regs __iomem *cap = regs;
	struct xhci_op_regs __iomem *op;
	u32 val;
	int ret;

	op = regs + FIELD_GET(HC_LENGTH, readl(&cap->hc_capbase));

	ret = readl_poll_timeout(&op->command, val, !(val & CMD_RESET),
				 1000, ASMT_RESET_TIMEOUT_USEC);
	if (!ret)
		return 0;

	pci_warn(pdev, "Reset timed out, trying to kick it\n");

	pci_write_config_byte(pdev, ASMT_CFG_SRAM_ACCESS,
			      ASMT_CFG_SRAM_ACCESS_ENABLE);
	pci_write_config_byte(pdev, ASMT_CFG_SRAM_ACCESS, 0);

	ret = readl_poll_timeout(&op->command, val, !(val & CMD_RESET),
				 1000, ASMT_RESET_TIMEOUT_USEC);
	if (ret)
		pci_err(pdev, "Reset timed out, giving up\n");

	return ret;
}

static int asmedia_read_reg(struct pci_dev *pdev, void __iomem *regs,
			    u16 addr, u8 *val)
{
	u8 status;
	int ret;

	ret = readb_poll_timeout(regs + ASMT_REG_STATUS, status,
				 !(status & ASMT_REG_STATUS_BUSY),
				 1000, ASMT_TIMEOUT_USEC);
	if (ret) {
		pci_err(pdev, "Read reg wait timed out ([%04x])\n", addr);
		return ret;
	}

	writew(addr, regs + ASMT_REG_ADDR);

	ret = readb_poll_timeout(regs + ASMT_REG_STATUS, status,
				 !(status & ASMT_REG_STATUS_BUSY),
				 1000, ASMT_TIMEOUT_USEC);
	if (ret) {
		pci_err(pdev, "Read reg addr timed out ([%04x])\n", addr);
		return ret;
	}

	*val = readb(regs + ASMT_REG_RDATA);
	return 0;
}

static int asmedia_write_reg(struct pci_dev *pdev, void __iomem *regs,
			     u16 addr, u8 data, bool verify)
{
	u8 status, val = 0;
	int ret, err;

	writew(addr, regs + ASMT_REG_ADDR);

	ret = readb_poll_timeout(regs + ASMT_REG_STATUS, status,
				 !(status & ASMT_REG_STATUS_BUSY),
				 1000, ASMT_TIMEOUT_USEC);
	if (ret) {
		pci_err(pdev, "Write reg addr timed out ([%04x] = %02x)\n",
			addr, data);
		return ret;
	}

	writeb(data, regs + ASMT_REG_WDATA);

	ret = readb_poll_timeout(regs + ASMT_REG_STATUS, status,
				 !(status & ASMT_REG_STATUS_BUSY),
				 1000, ASMT_TIMEOUT_USEC);
	if (ret) {
		pci_err(pdev, "Write reg data timed out ([%04x] = %02x)\n",
			addr, data);
		return ret;
	}

	if (!verify)
		return 0;

	ret = read_poll_timeout(asmedia_read_reg, err, err || val == data,
				0, ASMT_TIMEOUT_USEC, false, pdev, regs, addr, &val);
	if (ret) {
		pci_err(pdev, "Verify register timed out ([%04x] = %02x)\n",
			addr, data);
		return ret;
	}

	return err;
}

static int asmedia_write_code(struct pci_dev *pdev, void __iomem *regs,
			      const struct firmware *fw)
{
	size_t words = fw->size / 2;
	size_t index = 0;
	u16 addr = 0, raddr;
	int ret, err;
	u32 data;

	/*
	 * In each block of 2 * ASMT_FW_HALF_WORDS words of the image, word i
	 * goes into the low half of a 32-bit write and word
	 * i + ASMT_FW_HALF_WORDS into its high half.
	 */
	while (index < words) {
		size_t high = index | ASMT_FW_HALF_WORDS;

		data = get_unaligned_le16(&fw->data[index * 2]);
		if (high < words)
			data |= (u32)get_unaligned_le16(&fw->data[high * 2]) << 16;

		pci_write_config_word(pdev, ASMT_CFG_SRAM_ADDR, addr);

		writel(data, regs + ASMT_REG_CODE_WDATA);

		ret = read_poll_timeout(pci_read_config_word, err,
					err || raddr != addr,
					1, ASMT_TIMEOUT_USEC, false, pdev,
					ASMT_CFG_SRAM_ADDR, &raddr);
		if (!ret && err)
			ret = pcibios_err_to_errno(err);
		if (ret) {
			pci_err(pdev, "Code RAM write failed at word %zu: %d\n", index, ret);
			return ret;
		}

		if (++index & ASMT_FW_HALF_WORDS)
			index += ASMT_FW_HALF_WORDS;
		addr += 2;
	}

	return 0;
}

/*
 * On failure the controller is left with its CPU stopped and needs a power
 * cycle; the probe fails, as there is no known way back to the ROM firmware.
 */
static int asmedia_load_fw(struct pci_dev *pdev, void __iomem *regs,
			   const struct firmware *fw)
{
	int ret;

	/* Stop the CPU and open the code RAM for writing */
	ret = asmedia_write_reg(pdev, regs, ASMT_MMIO_CPU_MODE_NEXT,
				ASMT_MMIO_CPU_MODE_HALFSPEED, false);
	if (ret)
		return ret;

	ret = asmedia_write_reg(pdev, regs, ASMT_MMIO_CPU_EXEC_CTRL,
				ASMT_MMIO_CPU_EXEC_CTRL_RESET, false);
	if (ret)
		return ret;

	ret = asmedia_wait_reset(pdev, regs);
	if (ret) {
		pci_err(pdev, "Failed pre-upload reset\n");
		return ret;
	}

	ret = asmedia_write_reg(pdev, regs, ASMT_MMIO_CPU_EXEC_CTRL,
				ASMT_MMIO_CPU_EXEC_CTRL_HALT, false);
	if (ret)
		return ret;

	ret = asmedia_write_reg(pdev, regs, ASMT_MMIO_CPU_MISC,
				ASMT_MMIO_CPU_MISC_CODE_RAM_WR, true);
	if (ret)
		return ret;

	pci_write_config_byte(pdev, ASMT_CFG_SRAM_ACCESS,
			      ASMT_CFG_SRAM_ACCESS_ENABLE);
	ret = asmedia_write_code(pdev, regs, fw);
	pci_write_config_byte(pdev, ASMT_CFG_SRAM_ACCESS, 0);
	if (ret)
		return ret;

	ret = asmedia_write_reg(pdev, regs, ASMT_MMIO_CPU_MISC, 0, true);
	if (ret)
		return ret;

	/* Start the CPU from the code RAM */
	ret = asmedia_write_reg(pdev, regs, ASMT_MMIO_CPU_MODE_NEXT,
				ASMT_MMIO_CPU_MODE_RAM |
				ASMT_MMIO_CPU_MODE_HALFSPEED, false);
	if (ret)
		return ret;

	ret = asmedia_write_reg(pdev, regs, ASMT_MMIO_CPU_EXEC_CTRL, 0, false);
	if (ret)
		return ret;

	ret = asmedia_wait_reset(pdev, regs);
	if (ret)
		pci_err(pdev, "Failed post-upload reset\n");

	return ret;
}

static int asmedia_upload_fw(struct pci_dev *pdev, const struct firmware *fw)
{
	void __iomem *regs;
	int ret;

	regs = pci_iomap(pdev, 0, 0);
	if (!regs)
		return -ENOMEM;

	ret = asmedia_load_fw(pdev, regs, fw);

	pci_iounmap(pdev, regs);
	return ret;
}

/*
 * Upload the firmware if the controller only runs its ROM firmware. Without a
 * usable firmware file the controller is left untouched and probed with the
 * ROM firmware, as xhci-pci would.
 */
static int __asmedia_check_request_fw(struct pci_dev *pdev)
{
	const struct firmware *fw;
	int ret;

	ret = asmedia_check_firmware(pdev);
	if (ret < 0)
		return ret;
	if (ret == 1)
		return 0;

	ret = firmware_request_nowarn(&fw, ASMEDIA_APPLE_FW_NAME, &pdev->dev);
	if (ret) {
		pci_warn(pdev, "Cannot load %s (%d), using the ROM firmware\n",
			 ASMEDIA_APPLE_FW_NAME, ret);
		return 0;
	}

	/* The image has no header; it is uploaded in words to a 16-bit address space */
	if (!fw->size || fw->size % 2 || fw->size > ASMT_FW_MAX_SIZE) {
		pci_warn(pdev, "Invalid firmware size %zu, using the ROM firmware\n",
			 fw->size);
		ret = 0;
		goto out_release;
	}

	ret = asmedia_upload_fw(pdev, fw);
	if (ret) {
		pci_err(pdev, "Firmware upload failed: %d\n", ret);
		goto out_release;
	}

	ret = asmedia_check_firmware(pdev);
	if (ret == 0) {
		pci_err(pdev, "ROM firmware still running after the upload\n");
		ret = -EIO;
	} else if (ret == 1) {
		pci_info(pdev, "Loaded firmware %s\n", ASMEDIA_APPLE_FW_NAME);
		ret = 0;
	}

out_release:
	release_firmware(fw);
	return ret;
}

/* Enabled for the MMIO access of the upload, as xhci-pci would have it later */
static int asmedia_check_request_fw(struct pci_dev *pdev)
{
	int ret;

	ret = pci_enable_device_mem(pdev);
	if (ret)
		return ret;

	ret = __asmedia_check_request_fw(pdev);

	pci_disable_device(pdev);
	return ret;
}

static int xhci_pci_asmedia_probe(struct pci_dev *pdev,
				  const struct pci_device_id *id)
{
	int ret;

	/* Only Apple's parts are known to need this, xhci-pci handles the rest */
	if (!xhci_pci_is_apple_asmedia(pdev))
		return -ENODEV;

	ret = asmedia_check_request_fw(pdev);
	if (ret)
		return ret;

	return xhci_pci_common_probe(pdev, id);
}

static const struct pci_device_id pci_ids[] = {
	{ PCI_DEVICE(PCI_VENDOR_ID_ASMEDIA, PCI_DEVICE_ID_ASMEDIA_2142_XHCI) },
	{ /* end: all zeroes */ }
};
MODULE_DEVICE_TABLE(pci, pci_ids);

static struct pci_driver xhci_asmedia_pci_driver = {
	.name =		"xhci-pci-asmedia",
	.id_table =	pci_ids,

	.probe =	xhci_pci_asmedia_probe,
	.remove =	xhci_pci_remove,

	.shutdown =	usb_hcd_pci_shutdown,
	.driver = {
		.pm = pm_ptr(&usb_hcd_pci_pm_ops),
	},
};
module_pci_driver(xhci_asmedia_pci_driver);

MODULE_DESCRIPTION("ASMedia xHCI PCI Host Controller Driver with firmware loading");
MODULE_FIRMWARE(ASMEDIA_APPLE_FW_NAME);
MODULE_IMPORT_NS("xhci");
MODULE_LICENSE("Dual MIT/GPL");
