// SPDX-License-Identifier: GPL-2.0
//
// Author: Timon Stipkovits <timon2201@gmail.com>
//
// This software may be used and distributed according to the terms of the
// GNU General Public License version 2.

#ifndef HELPERS_H
#define HELPERS_H
#include "datatypes.h"
#include "defines.h"
#include "nice_weights.h"

// The queue of @cpu: one per cpu, ordered by deadline.
static __always_inline u64 cpu_dsq(u32 cpu)
{
  return DSQ_BASE + cpu;
}

static __always_inline bool is_percpu_kthread(const struct task_struct* p)
{
  return (p->flags & PF_KTHREAD) && p->nr_cpus_allowed == 1;
}

// Weight of a task from its nice value (nice_weights.h). SCHED_IDLE asks to run
// only when nothing else wants the cpu: the lowest weight.
static __always_inline u64 task_weight(const struct task_struct* p)
{
  if (p->policy == SCHED_IDLE)
    return 1;
  s32 idx = (s32)p->static_prio - NICE_0_PRIO + 20;
  if (idx < 0)
    idx = 0;
  if (idx > 39)
    idx = 39;
  return nice_weights[idx];
}

// Slack of a task from its nice value (see SLACK_NICE_0_NS).
static __always_inline u64 task_slack(const struct task_struct* p)
{
  // Per-cpu kernel threads (kworker/N, ksoftirqd/N, ...) are the only ones that
  // can do the work of their cpu, so they go first, whatever their nice value.
  if (is_percpu_kthread(p))
    return SLACK_MIN_NS;

  // SCHED_IDLE asks to run only when nothing else wants the cpu.
  if (p->policy == SCHED_IDLE)
    return SLACK_MAX_NS;

  u64 slack = SLACK_NICE_0_NS * NICE_0_WEIGHT / task_weight(p);
  if (slack < SLACK_MIN_NS)
    return SLACK_MIN_NS;
  if (slack > SLACK_MAX_NS)
    return SLACK_MAX_NS;
  return slack;
}

// First task of one of our queues. scx_bpf_dsq_peek() (since 6.19) reads it
// without the lock of the queue, the iterator takes that lock (also of queues of
// other cpus) for every step. Our queues are all ordered by vtime: peek is right
// for them since 6.19 (the fix in 7.1 is for FIFO queues).
static __always_inline struct task_struct* dsq_first(u64 dsq)
{
  struct task_struct* p = NULL;
  struct bpf_iter_scx_dsq it;

  if (bpf_ksym_exists(scx_bpf_dsq_peek))
    return scx_bpf_dsq_peek(dsq);
  if (!bpf_iter_scx_dsq_new(&it, dsq, 0))
    p = bpf_iter_scx_dsq_next(&it);
  bpf_iter_scx_dsq_destroy(&it);
  return p;
}

static __always_inline u64 dsq_queued(u64 dsq)
{
  s32 n = scx_bpf_dsq_nr_queued(dsq);
  return n > 0 ? (u64)n : 0;
}

static __always_inline struct task_ctx* get_task_ctx(struct task_struct* task)
{
  return bpf_task_storage_get(&task_ctx_store, task, NULL, 0);
}

static __always_inline struct dispatch_ctx* get_dispatch_ctx(u32 cpu)
{
  u32 key = 0;
  return bpf_map_lookup_percpu_elem(&dispatch_state, &key, cpu);
}

static __always_inline u32 cpu_llc_id(u32 cpu)
{
  cpu &= (MAX_CPUS - 1);
  return cpu_to_llc[cpu];
}

static __always_inline bool cpu_is_online(u32 cpu)
{
  cpu &= (MAX_CPUS - 1);
  return cpu_online[cpu];
}

static __always_inline bool is_rt_task(const struct task_struct* p)
{
  int policy = p->policy;

  // The per-cpu stopper threads (migration/N, run for every affinity change and
  // task migration) only run for microseconds: they don't take the cpu away in
  // a way worth moving tasks for. They are the only per-cpu kernel threads with
  // the highest RT priority. Other per-cpu kernel threads with an RT policy (irq
  // threads bound to the cpu of their irq, rcuc/N, ktimers/N, ...) count as RT
  // tasks: they can run for a long time (threaded NAPI, irq storms).
  if (policy == SCHED_FIFO && p->rt_priority == STOPPER_RT_PRIO && is_percpu_kthread(p))
    return false;
  return policy == SCHED_FIFO || policy == SCHED_RR || policy == SCHED_DEADLINE;
}

// The cpu runs a task of a higher sched class (RT, deadline) right now. sched_ext
// tasks don't run there until it is done, even when nothing of ours is running.
static __always_inline bool cpu_taken_by_rt(u32 cpu)
{
  struct task_struct* curr = __COMPAT_scx_bpf_cpu_curr(cpu);
  return curr && is_rt_task(curr);
}

static __always_inline u64 elapsed(u64 now, u64 last)
{
  return now > last ? now - last : 0;
}

static __always_inline void charge_wake_boost(struct task_ctx* tctx, u64 used)
{
  if (!tctx->boost_slack)
    return;
  tctx->boost_used += used;
  tctx->boost_credit = tctx->boost_credit > used ? tctx->boost_credit - used : 0;
}

// The boost has used up what it was granted.
static __always_inline bool boost_spent(const struct task_ctx* tctx)
{
  return tctx->boost_used >= tctx->boost_limit;
}

// Refill the boost credit for the time since the last refill, at a rate of
// WAKE_BOOST_BUDGET_NS per WAKE_BOOST_WINDOW_NS, up to WAKE_BOOST_BUDGET_NS.
static __always_inline void refill_boost_credit(struct task_ctx* tctx, u64 now)
{
  u64 since = elapsed(now, tctx->boost_credit_ts);
  u64 add = since >= WAKE_BOOST_WINDOW_NS ? WAKE_BOOST_BUDGET_NS : since * WAKE_BOOST_BUDGET_NS / WAKE_BOOST_WINDOW_NS;
  u64 credit = tctx->boost_credit + add;
  tctx->boost_credit = credit < WAKE_BOOST_BUDGET_NS ? credit : WAKE_BOOST_BUDGET_NS;
  tctx->boost_credit_ts = now;
}

static __always_inline u64 cpu_vtime_ref(u32 cpu)
{
  struct dispatch_ctx* dctx = get_dispatch_ctx(cpu);
  return dctx ? dctx->vtime_ref : VTIME_BASE;
}

// How far the task is ahead of (positive) or behind (negative) the tasks of its
// cpu in weighted cpu time. Limited to LAG_LATE_MAX_NS either way, scaled by
// weight like the vtime itself: a task with a low weight uses up much more vtime
// per slice, keeps that debt when it moves, and is just as far behind when it
// had to wait.
//
// A task that wakes up (@woken) is at most LAG_EARLY_MAX_NS behind: it slept,
// sleeping doesn't earn cpu time. A task that stayed runnable keeps all of it:
// it is behind because it waited for the cpu, and that is what makes the shares
// follow the weights. Clamping it too would let the slack decide instead: a nice
// 5 task next to a nice 0 task got a slice every ~20ms (the difference of their
// slacks) instead of every ~7.6ms (the ratio of their weights).
static __always_inline s64 task_lag(const struct task_struct* p, struct task_ctx* tctx, bool woken)
{
  if (tctx->vtime_cpu == VTIME_CPU_NONE)
    return 0;
  s64 lag = (s64)(tctx->vtime - cpu_vtime_ref(tctx->vtime_cpu));
  s64 lag_max = (s64)(LAG_LATE_MAX_NS * NICE_0_WEIGHT / task_weight(p));
  s64 early_max = woken ? (s64)LAG_EARLY_MAX_NS : lag_max;
  if (lag > lag_max)
    return lag_max;
  if (lag < -early_max)
    return -early_max;
  return lag;
}

// Move the vtime of a task to @cpu, keeping its lag.
static __always_inline void rebase_vtime(struct task_ctx* tctx, u32 cpu, s64 lag)
{
  tctx->vtime = cpu_vtime_ref(cpu) + lag;
  tctx->vtime_cpu = cpu;
}

// Charge @used ns of cpu time to the vtime of a task. While a wake boost runs,
// with the weight of the waker: the woken task does the waker's work, and
// charged with its own weight a light task (a nice 0 audio client woken by a
// nice -12 audio server) used up its turn in the first slice and then waited
// behind everything of higher weight anyway. The boost credit limits this to
// two fifths of a core.
static __always_inline void charge_vtime(const struct task_struct* p, struct task_ctx* tctx, u64 used)
{
  u64 w = task_weight(p);
  if (tctx->boost_slack && tctx->boost_weight > w)
    w = tctx->boost_weight;
  tctx->vtime += used * NICE_0_WEIGHT / w;
}

// Deadline from the time a task is queued, its slack and its lag. The lag goes
// in at most LAG_LATE_MAX_NS: it is scaled by weight (up to ~20s at nice 19),
// and the deadline is the promise of take_overdue(), which has to hold well
// within the 30s of the sched_ext watchdog.
static __always_inline u64 make_deadline(u64 now, u64 slack, s64 lag)
{
  if (lag > (s64)LAG_LATE_MAX_NS)
    lag = LAG_LATE_MAX_NS;
  // Behind just as far at most: a deadline that is already long past would make
  // take_overdue() run the task right away.
  if (lag < -(s64)LAG_LATE_MAX_NS)
    lag = -(s64)LAG_LATE_MAX_NS;
  return now + slack + lag;
}

// Dispatch of @cpu (this cpu) put @p into its local DSQ. The DSQ can't be looked
// into, but right after a slice the task that runs next is already in it, and
// balancing the task that comes back after its slice (pick_balance_cpu()) needs
// to know how heavy it is: counting it as nice 0 kept a light task next to a
// heavy one while another cpu had only light tasks.
static __always_inline void note_local(u32 cpu, const struct task_struct* p)
{
  struct dispatch_ctx* dctx = get_dispatch_ctx(cpu);
  if (dctx)
    dctx->local_weight = task_weight(p);
}

// @p comes back through enqueue() because an RT or deadline task preempted it
// while it ran (SCX_ENQ_IMMED, see enq_immed). Kernels before 7.1 don't do
// that: there the preempted task waits at the head of the local DSQ.
static __always_inline bool reenq_preempted(const struct task_struct* p, u64 enq_flags)
{
  if (!(enq_flags & SCX_ENQ_REENQ) || !bpf_core_enum_value_exists(enum scx_ent_flags, SCX_TASK_REENQ_PREEMPTED))
    return false;
  u32 mask = bpf_core_enum_value(enum scx_ent_flags, SCX_TASK_REENQ_REASON_MASK);
  return (p->scx.flags & mask) == bpf_core_enum_value(enum scx_ent_flags, SCX_TASK_REENQ_PREEMPTED);
}

// Relative speed of @cpu, 1024 for the fastest cores.
static __always_inline u32 cpu_cap(u32 cpu)
{
  cpu &= (MAX_CPUS - 1);
  u32 cap = cpu_capacity[cpu];
  return cap ? cap : 1024;
}

// @cpu is one of the fast cpus (see big_cpus).
static __always_inline bool cpu_is_fast(u32 cpu)
{
  cpu &= (MAX_CPUS - 1);
  return cpu_fast[cpu];
}

// With fast and slower cores: tasks more important than nice 0 (a lower nice
// value, or a wake boost from such a task) go to the fast cores first, tasks
// less important than nice 0 to the slower ones. Nice 0 takes any.
static __always_inline bool wants_fast_core(const struct task_struct* p, const struct task_ctx* tctx)
{
  return task_weight(p) > NICE_0_WEIGHT || (tctx && tctx->boost_slack && tctx->boost_weight > NICE_0_WEIGHT);
}

static __always_inline bool wants_slow_core(const struct task_struct* p, const struct task_ctx* tctx)
{
  return task_weight(p) < NICE_0_WEIGHT && !(tctx && tctx->boost_slack);
}

// Slack of a task including a wake boost.
static __always_inline u64 effective_slack(const struct task_struct* p, struct task_ctx* tctx)
{
  u64 slack = task_slack(p);
  if (tctx && tctx->boost_slack && tctx->boost_slack < slack)
    return tctx->boost_slack;
  return slack;
}

#endif  // HELPERS_H
