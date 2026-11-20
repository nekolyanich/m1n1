/* SPDX-License-Identifier: GPL-2.0-or-later */

#ifndef CPUFREQ_H
#define CPUFREQ_H

#include "types.h"

int cpufreq_init(void);
void cpufreq_fixup(void);
/* native S2R: this chip's cluster bases (the cpufreq table's), up to max; returns how many */
int cpufreq_cluster_bases(u64 *bases, int max);

#endif
