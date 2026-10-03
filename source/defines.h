// SPDX-License-Identifier: GPL-2.0
//
// Author: Timon Stipkovits <timon2201@gmail.com>
//
// This software may be used and distributed according to the terms of the
// GNU General Public License version 2.

#ifndef DEFINES_H
#define DEFINES_H

#define NS_PER_US 1000ULL
#define NS_PER_MS (1000ULL * NS_PER_US)

#define BAND_0 0  // nice -20 .. -11
#define BAND_1 1  // nice -10 ..  -3
#define BAND_2 2  // nice  -2 ..   2
#define BAND_3 3  // nice   3 ..  10
#define BAND_4 4  // nice  11 ..  20
#define BAND_AMOUNT 5

#define BAND_0_MAX_NICE (-11)
#define BAND_1_MAX_NICE (-3)
#define BAND_2_MAX_NICE 2
#define BAND_3_MAX_NICE 10

#define NICE_0_PRIO 120

#define DSQ_BASE 1536
#define DSQ_BAND_STRIDE 512

// Bands up to this one look through the whole LLC for a cpu to run on, the
// others only sample BALANCE_SAMPLES cpus.
#define BAND_SCAN_WHOLE_LLC BAND_1

#define LAG_MAX_NS SLICE_NS
#define VTIME_BASE (1ULL << 40)
#define KEY_CPU_NONE ((u32)-1)


#define STARVE_BUDGET_BAND_1_NS (20ULL * NS_PER_MS)
#define STARVE_BUDGET_BAND_2_NS (50ULL * NS_PER_MS)
#define STARVE_BUDGET_BAND_3_NS (100ULL * NS_PER_MS)
#define STARVE_BUDGET_BAND_4_NS (200ULL * NS_PER_MS)

#define STARVE_OVERRIDE_COOLDOWN_NS (10ULL * NS_PER_MS)

#define SLICE_NS (1000 * NS_PER_US)

#define RESUME_SLICE_MIN_NS (50 * NS_PER_US)

#define BALANCE_INTERVAL_NS (10ULL * NS_PER_MS)
#define BALANCE_SAMPLES 2

#define MAX_CPUS 512

#endif  // DEFINES_H
