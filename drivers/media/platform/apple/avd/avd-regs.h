/* SPDX-License-Identifier: MIT */
/* AVD coprocessor mailbox registers, from the asahi avd driver and avd-fw. */

#ifndef AVD_REGS_H_
#define AVD_REGS_H_

#include <linux/bits.h>

#define AVD_REG_RUN_CTRL	0x08
#define AVD_RUN_CTRL_RUN	BIT(0)
#define AVD_RUN_CTRL_STOP	(BIT(1) | BIT(2) | BIT(3))

#define AVD_REG_MBOX_IRQ_ENABLE	0x48
#define AVD_REG_MBOX_IRQ_CLR	0x4c
#define AVD_MBOX1_NOT_EMPTY	BIT(3)

#define AVD_REG_MBOX1_STATUS	0x5c
#define AVD_REG_MBOX1_RETRIEVE	0x64
#define AVD_MBOX_ENABLE		BIT(0)

#define AVD_REG_FLAG0_SET	0x90
#define AVD_REG_FLAG0_CLR	0x98

/* Messages avd-fw sends on mailbox 1 (avd-fw src/irq.c). */
#define AVD_MSG_UNHANDLED	BIT(16)
#define AVD_MSG_PP_DONE		BIT(12)
#define AVD_MSG_VP_DONE		BIT(8)

#endif /* AVD_REGS_H_ */
