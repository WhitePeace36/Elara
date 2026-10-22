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

// Slice of a task. A task that can't preempt (a task of the same urgency runs)
// waits at most this long.
//
// The kernel itself only ends a sched_ext slice at the scheduler tick: the slice
// counts down while the task runs, but the task is only switched out at the
// first tick after it reached 0. With HZ=1000 a slice of 0.5ms really lasted
// 0.5-1.5ms, with HZ=250 up to 4.5ms. So a per-cpu timer ends the slice on time
// when another task waits for the cpu (slice_timer_fn()), and a task queued on a
// cpu whose running task is already past its slice preempts it right away.
#define SLICE_NS (1000 * NS_PER_US)

#define RESUME_SLICE_MIN_NS (50 * NS_PER_US)

// ---------------------------------------------------------------------------
// Deadlines
// ---------------------------------------------------------------------------

// Every task has a weight from its nice value (nice_weights.h, NICE_0_WEIGHT at
// nice 0, a factor of 1.5 per nice step, steeper than CFS) and a slack:
// SLACK_NICE_0_NS scaled by NICE_0_WEIGHT / weight. A lower nice value gives a
// shorter slack, so the task gets on the cpu sooner.
//
// A queued task has two deadlines:
// - its key, vtime + slack: the order of its cpu's queue. The vtime is the cpu
//   time it used scaled by NICE_0_WEIGHT / weight, so tasks that keep running
//   share a cpu by their weight (this is the virtual deadline of EEVDF).
// - its deadline, queue time + slack + lag (ns, the same clock on every cpu):
//   used to compare tasks of different cpus, whose vtimes don't compare. A task
//   waiting on a cpu that can't run it keeps its deadline while the deadlines of
//   the tasks on the other cpus move on, so it gets taken over.
//
//   nice -12: ~23us   nice -5: ~0.4ms   nice 0: 3ms
//   nice   5: ~23ms   nice 10: ~170ms   nice 15 and higher: 500ms
#define NICE_0_WEIGHT 1024
#define SLACK_NICE_0_NS (3ULL * NS_PER_MS)
#define SLACK_MIN_NS (20ULL * NS_PER_US)
#define SLACK_MAX_NS (500ULL * NS_PER_MS)

// The lag of a task is its vtime minus the vtime reference of its cpu (the
// highest vtime that started running there). It is kept when a task moves to
// another cpu, and limited to -LAG_EARLY_MAX_NS (a task that slept gets at most
// this much ahead of the others) and +LAG_LATE_MAX_NS scaled by NICE_0_WEIGHT /
// weight (a task with a low weight uses up much more vtime per slice).
#define LAG_EARLY_MAX_NS SLICE_NS
#define LAG_LATE_MAX_NS (20ULL * NS_PER_MS)
#define VTIME_BASE (1ULL << 40)
#define VTIME_CPU_NONE ((u32)-1)

// Weighted shares alone can let a task wait very long: a SCHED_IDLE task next to
// a nice -20 task that keeps running gets a slice every ~50s, and the sched_ext
// watchdog stops the scheduler after 30s. So the deadline is also a promise:
// every STARVE_CHECK_INTERVAL_NS a cpu looks at its queue (at most the last
// STARVE_SCAN_DEPTH tasks, where the long waiting ones are) for a task more than
// STARVE_GRACE_NS past its deadline, and runs it first. With normal shares this
// hardly ever happens.
#define STARVE_GRACE_NS (500ULL * NS_PER_MS)
#define STARVE_CHECK_INTERVAL_NS (10ULL * NS_PER_MS)
#define STARVE_SCAN_DEPTH 32

// A woken task gets this fraction of its slack (1 / WAKE_SLACK_DIV). Without it
// a task that wakes up lands behind every task of the same nice value that keeps
// running, because those were queued earlier.
#define WAKE_SLACK_DIV 2

// A waking task preempts the running task when its deadline is earlier by more
// than this. Keeps tasks with nearly the same deadline from preempting each
// other back and forth.
#define PREEMPT_MARGIN_NS (250ULL * NS_PER_US)

// Tasks with at most this slack check all cpus of their llc for the one with the
// least work ahead of them, other tasks compare their cpu with BALANCE_SAMPLES
// random ones.
#define SCAN_WHOLE_LLC_SLACK_NS (1ULL * NS_PER_MS)

#define BALANCE_INTERVAL_NS (10ULL * NS_PER_MS)

// A task that comes back after its slice is balanced by weight: it moves to the
// cpu with the least weight of other tasks (running and queued), if that is
// lighter than its own cpu by more than half its own weight. A cpu taken by an
// RT or deadline task counts as RT_CPU_WEIGHT. Of a queue, at most
// WEIGHT_SCAN_DEPTH tasks are summed up, the rest count as nice 0.
#define RT_CPU_WEIGHT (1ULL << 32)
#define WEIGHT_SCAN_DEPTH 16
#define BALANCE_SAMPLES 2

// When a cpu takes a task from another cpu, only tasks that may run on it count
// (affinity, migration disabled). At most this many tasks of a queue are looked
// at for the first such task.
#define STEAL_SCAN_DEPTH 8

#define DSQ_BASE 1536
#define DEADLINE_NONE ((u64)-1)

#define NICE_0_PRIO 120

// ---------------------------------------------------------------------------
// Wake boost
// ---------------------------------------------------------------------------

#define WAKE_BOOST_FROM_KTHREADS 0

// CPU time a woken task keeps the slack of its waker for, as long as it doesn't
// sleep before. Covers work that needs a few slices, without letting a task that
// keeps running stay ahead of everything.
#define WAKE_BOOST_BUDGET_NS (4ULL * NS_PER_MS)

// A task can't use more than WAKE_BOOST_BUDGET_NS of boosted cpu time per
// WAKE_BOOST_WINDOW_NS, however often it is woken: the credit for it refills at
// that rate.
#define WAKE_BOOST_WINDOW_NS (20ULL * NS_PER_MS)

// No boost when less credit than this is left: it would end before the task got
// anything done.
#define WAKE_BOOST_MIN_NS (250ULL * NS_PER_US)

// ---------------------------------------------------------------------------
// Realtime
// ---------------------------------------------------------------------------

// When an RT or deadline task takes a cpu, the task it preempted (and the ones
// already picked for the cpu) stay there for this long before they are placed
// again on other cpus. Most RT tasks (irq threads, audio, the compositor) only
// run for a moment: the preempted task then just continues, instead of losing
// its place and being moved for nothing.
#define RT_EVACUATE_DELAY_NS (250ULL * NS_PER_US)

// Load a cpu taken by an RT or deadline task counts as. More than one task: a
// task of ours gives the cpu back after at most one slice, an RT task only when
// it is done.
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
#ifndef CLOCK_MONOTONIC
#define CLOCK_MONOTONIC 1
#endif
#ifndef SCHED_DEADLINE
#define SCHED_DEADLINE 6
#endif

// rt_priority of the per-cpu stopper threads (migration/N): MAX_RT_PRIO - 1,
// see sched_set_stop_task() in kernel/sched/core.c.
#define STOPPER_RT_PRIO 99

#define MAX_CPUS 512

#endif  // DEFINES_H
