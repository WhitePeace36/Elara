// SPDX-License-Identifier: GPL-2.0
//
// Author: Timon Stipkovits <timon2201@gmail.com>
//
// This software may be used and distributed according to the terms of the
// GNU General Public License version 2.

#ifndef DATATYPES_H
#define DATATYPES_H
#include "defines.h"

const volatile u32 nr_llcs = 1;
const volatile u32 cpu_to_llc[MAX_CPUS] = {};
const volatile u8 cpu_online[MAX_CPUS] = {};

// Set by the loader when the kernel has SCX_OPS_ALWAYS_ENQ_IMMED (7.1+): every
// task put into a local DSQ either runs right away or comes back through
// enqueue(), it never waits there. In particular a task preempted by an RT or
// deadline task comes back right away (see reenq_preempted()), so the delayed
// evacuation of RT-taken cpus isn't needed.
const volatile bool enq_immed = false;

struct task_ctx
{
  // deadline in ns (see SLACK_NICE_0_NS) of the task in its queue, or of its
  // current slice while it runs, and its key (order in its cpu's queue)
  u64 deadline;
  u64 key;
  // weighted cpu time, relative to the vtime reference of vtime_cpu
  u64 vtime;
  u32 vtime_cpu;
  // slack taken over from the waker by a wake boost (0: no boost)
  u64 boost_slack;
  // weight of the waker the boost came from (its cpu time is charged with it)
  u64 boost_weight;
  // cpu time used with the boost since the wakeup that boosted it, and how much
  // of it that boost may use (WAKE_BOOST_BUDGET_NS, less when the credit is low)
  u64 boost_used;
  u64 boost_limit;
  // boosted cpu time the task may still use, refilled over WAKE_BOOST_WINDOW_NS
  u64 boost_credit;
  u64 boost_credit_ts;
  u64 started_at;

  u64 granted_slice;
  u64 resume_slice;
  u64 last_migrated_at;
};

struct dispatch_ctx
{
  // deadline and key of the task running on this cpu (DEADLINE_NONE: none of ours)
  u64 running_deadline;
  u64 running_key;
  // highest vtime of a task that started running on this cpu
  u64 vtime_ref;
  // last time this cpu looked for overdue tasks in its queue
  u64 last_starve_check;
  // when the slice of the running task ends, and when the slice timer fires (0:
  // not set)
  u64 slice_end;
  u64 timer_end;
  // weight of the task dispatch just put into the local DSQ of this cpu and
  // that hasn't started yet (see note_local())
  u64 local_weight;
  // key of the woken task enqueue() put straight into the local DSQ to preempt
  // the running one, until it starts (0: none)
  u64 preempt_key;
  bool preempt_pending;
  // this cpu runs an RT or deadline task (as seen by the last sched_switch)
  bool rt_busy;
};

// Set at the end of elara_init(). The sched_switch tracepoint can run before:
// the queues don't exist yet then (scx_bpf_dsq_peek() on them stops the
// scheduler).
bool elara_ready;

// Number of cpus an RT or deadline task runs on right now. Lets dispatch skip
// looking for such cpus when there are none.
u64 nr_rt_busy;

struct pick_scratch
{
  u64 best_load;
  u64 sampled;
  s32 best;
};

struct
{
  __uint(type, BPF_MAP_TYPE_PERCPU_ARRAY);
  __uint(max_entries, 1);
  __type(key, u32);
  __type(value, struct pick_scratch);
} pick_scratch_map SEC(".maps");

// Loop state of find_earliest(). Kept in a map instead of local variables: the
// verifier tracks locals with their exact values, which multiplies the states of
// the cpu loop.
struct steal_scratch
{
  u64 best_deadline;
  s32 best_cpu;
};

struct
{
  __uint(type, BPF_MAP_TYPE_PERCPU_ARRAY);
  __uint(max_entries, 1);
  __type(key, u32);
  __type(value, struct steal_scratch);
} steal_scratch_map SEC(".maps");

struct
{
  __uint(type, BPF_MAP_TYPE_PERCPU_ARRAY);
  __uint(max_entries, 1);
  __type(key, u32);
  __type(value, struct dispatch_ctx);
} dispatch_state SEC(".maps");

// Two timers per cpu: entry cpu ends the slice of the running task on time (see
// SLICE_NS), entry MAX_CPUS + cpu moves the tasks picked for the cpu elsewhere
// when an RT task keeps it (see RT_EVACUATE_DELAY_NS). Timers can't be kept in
// per-cpu maps, so this is an array indexed by cpu.
struct elara_timer
{
  struct bpf_timer timer;
};

struct
{
  __uint(type, BPF_MAP_TYPE_ARRAY);
  __uint(max_entries, 2 * MAX_CPUS);
  __type(key, u32);
  __type(value, struct elara_timer);
} slice_timers SEC(".maps");

struct
{
  __uint(type, BPF_MAP_TYPE_TASK_STORAGE);
  __uint(map_flags, BPF_F_NO_PREALLOC);
  __type(key, int);
  __type(value, struct task_ctx);
} task_ctx_store SEC(".maps");

#endif  // DATATYPES_H
