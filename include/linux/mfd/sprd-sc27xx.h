/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_MFD_SPRD_SC27XX_H
#define _LINUX_MFD_SPRD_SC27XX_H

#include <linux/kconfig.h>
#include <linux/types.h>

#if IS_ENABLED(CONFIG_MITOCHODRIA_SC27XX_SPURIOUS_WAKE_RETRY)
bool sprd_sc27xx_consume_spurious_wakeup(void);
#else
static inline bool sprd_sc27xx_consume_spurious_wakeup(void)
{
	return false;
}
#endif

#endif /* _LINUX_MFD_SPRD_SC27XX_H */
