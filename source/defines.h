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

#define BAND_SCAN_WHOLE_LLC BAND_1

#define LAG_MAX_NS SLICE_NS
#define VTIME_BASE (1ULL << 40)
#define KEY_CPU_NONE ((u32)-1)

#define WAKE_BOOST_FROM_KTHREADS 0

// CPU time a woken task keeps the band of its waker for, as long as it doesn't
// sleep before. Covers work that needs a few slices, without letting a task that
// keeps running stay in the better band.
#define WAKE_BOOST_BUDGET_NS (4ULL * NS_PER_MS)

// Load a cpu taken by an RT or deadline task counts as. More than one task: a
// task of our bands gives the cpu back after at most one slice, an RT task only
// when it is done.
#define RT_CPU_LOAD 2

// Scheduling policies (include/uapi/linux/sched.h), not part of vmlinux.h
#ifndef SCHED_FIFO
#define SCHED_FIFO 1
#endif
#ifndef SCHED_RR
#define SCHED_RR 2
#endif
#ifndef SCHED_IDLE
#define SCHED_IDLE 5
#endif
#ifndef SCHED_DEADLINE
#define SCHED_DEADLINE 6
#endif


#define STARVE_BUDGET_BAND_1_NS (20ULL * NS_PER_MS)
#define STARVE_BUDGET_BAND_2_NS (50ULL * NS_PER_MS)
#define STARVE_BUDGET_BAND_3_NS (100ULL * NS_PER_MS)
#define STARVE_BUDGET_BAND_4_NS (200ULL * NS_PER_MS)

#define STARVE_OVERRIDE_COOLDOWN_NS (10ULL * NS_PER_MS)

// CPU time a starved band gets per override: its tasks run one after another
// until it is used up (or the band is empty). One task per override isn't
// enough: tasks that sleep right away again would use it up in microseconds.
#define STARVE_OVERRIDE_BUDGET_NS SLICE_NS

#define SLICE_NS (1000 * NS_PER_US)

#define RESUME_SLICE_MIN_NS (50 * NS_PER_US)

#define BALANCE_INTERVAL_NS (10ULL * NS_PER_MS)
#define BALANCE_SAMPLES 2

#define MAX_CPUS 512

#endif  // DEFINES_H
