/* SPDX-License-Identifier: GPL-2.0-or-later */

#ifndef __WDT_H__
#define __WDT_H__

#include "types.h"

void wdt_disable(void);
void wdt_reboot(void);
void wdt_freeze(void);
void wdt_thaw(void);
void wdt_pet(void);
void wdt_drop(void);
u64 wdt_secondary_addr(void);

#endif
