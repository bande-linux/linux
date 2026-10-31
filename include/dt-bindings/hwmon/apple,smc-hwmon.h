/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
/*
 * Sensor specifiers for apple,smc-hwmon thermal sensors
 */

#ifndef _DT_BINDINGS_HWMON_APPLE_SMC_HWMON_H
#define _DT_BINDINGS_HWMON_APPLE_SMC_HWMON_H

/* An SMC key, e.g. APPLE_SMC_KEY('T', 'C', 'M', 'b') for "TCMb" */
#define APPLE_SMC_KEY(a, b, c, d) \
	(((a) << 24) | ((b) << 16) | ((c) << 8) | (d))

#endif
