// SPDX-License-Identifier: MIT
/*
 * AVD coprocessor control, from the asahi avd driver (traces of the macOS
 * AppleAVD driver).
 */

#include <linux/io.h>
#include <linux/iopoll.h>

#include "avd.h"
#include "avd-regs.h"

#define AVD_VP_INSN_FIFO_IOVA		0x150
#define AVD_VP_INSN_FIFO_MASK		0x18c
#define AVD_VP_INSN_FIFO_CACH		0x1c8
#define AVD_VP_INSN_FIFO_XFER		0x204
#define AVD_VP_CTRL_CM3_IRQ_MASK	0x0fc
#define AVD_PP_CTRL_CM3_IRQ_MASK	0x120

#define AVD_CM3_UNK			BIT(0)
#define AVD_CM3_ERROR			BIT(1)
#define AVD_CM3_DONE			BIT(2)

#define AVD_VP_CM3_MASK	(AVD_CM3_UNK | AVD_CM3_ERROR | AVD_CM3_DONE)
#define AVD_PP_CM3_MASK	(AVD_CM3_UNK | AVD_CM3_DONE)

int avd_boot(struct avd_dev *avd)
{
	u32 val;

	memcpy_toio(avd->code, avd->fw->data, avd->fw->size);

	writel_relaxed(AVD_MBOX_ENABLE, avd->mbox + AVD_REG_MBOX1_STATUS);
	writel_relaxed(AVD_MBOX1_NOT_EMPTY, avd->mbox + AVD_REG_MBOX_IRQ_ENABLE);
	writel(AVD_RUN_CTRL_RUN, avd->mbox + AVD_REG_RUN_CTRL);

	/* avd-fw writes 1 to flag 0 once its tunables are applied (src/avd.c). */
	return readl_poll_timeout(avd->mbox + AVD_REG_FLAG0_SET, val, val == 1,
				  10, 10000);
}

/* The read posts the mailbox IRQ mask before the caller powers down. */
void avd_shutdown(struct avd_dev *avd)
{
	writel_relaxed(AVD_RUN_CTRL_STOP, avd->mbox + AVD_REG_RUN_CTRL);
	writel_relaxed(1, avd->mbox + AVD_REG_FLAG0_CLR);
	writel(0, avd->mbox + AVD_REG_MBOX_IRQ_ENABLE);
	readl(avd->mbox + AVD_REG_MBOX_IRQ_ENABLE);
}

void avd_configure_stream(struct avd_dev *avd, dma_addr_t addr, u32 vp_slot)
{
	void __iomem *ctrl = avd->ctrl;

	writel_relaxed(addr >> 8, ctrl + AVD_VP_INSN_FIFO_IOVA);
	writel_relaxed(0, ctrl + AVD_VP_INSN_FIFO_MASK);
	writel_relaxed(0, ctrl + AVD_VP_INSN_FIFO_CACH);
	writel_relaxed(0, ctrl + AVD_VP_INSN_FIFO_XFER);

	writel_relaxed(readl_relaxed(ctrl + AVD_VP_CTRL_CM3_IRQ_MASK + vp_slot * 4) |
		       AVD_VP_CM3_MASK,
		       ctrl + AVD_VP_CTRL_CM3_IRQ_MASK + vp_slot * 4);
	writel_relaxed(readl_relaxed(ctrl + AVD_PP_CTRL_CM3_IRQ_MASK) |
		       AVD_PP_CM3_MASK, ctrl + AVD_PP_CTRL_CM3_IRQ_MASK);
}
