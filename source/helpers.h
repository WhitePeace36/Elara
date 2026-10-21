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

// The queue of @cpu: one per cpu, ordered by deadline.
static __always_inline u64 cpu_dsq(u32 cpu)
{
  return DSQ_BASE + cpu;
}

static __always_inline bool is_percpu_kthread(const struct task_struct* p)
{
  return (p->flags & PF_KTHREAD) && p->nr_cpus_allowed == 1;
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

  u64 weight = p->scx.weight ? p->scx.weight : 1;
  u64 slack = SLACK_NICE_0_NS * NICE_0_WEIGHT / weight;
  if (slack < SLACK_MIN_NS)
    return SLACK_MIN_NS;
  if (slack > SLACK_MAX_NS)
    return SLACK_MAX_NS;
  return slack;
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
// cpu in weighted cpu time, limited to [-LAG_EARLY_MAX_NS, LAG_LATE_MAX_NS].
static __always_inline s64 task_lag(const struct task_struct* p, struct task_ctx* tctx)
{
  if (tctx->vtime_cpu == VTIME_CPU_NONE)
    return 0;
  s64 lag = (s64)(tctx->vtime - cpu_vtime_ref(tctx->vtime_cpu));
  // In vtime, so scaled by weight like the vtime itself: a task with a low
  // weight uses up much more vtime per slice, and keeps that debt when it moves.
  u64 weight = p->scx.weight ? p->scx.weight : 1;
  s64 late_max = (s64)(LAG_LATE_MAX_NS * NICE_0_WEIGHT / weight);
  if (lag > late_max)
    return late_max;
  if (lag < -(s64)LAG_EARLY_MAX_NS)
    return -(s64)LAG_EARLY_MAX_NS;
  return lag;
}

// Move the vtime of a task to @cpu, keeping its lag.
static __always_inline void rebase_vtime(struct task_ctx* tctx, u32 cpu, s64 lag)
{
  tctx->vtime = cpu_vtime_ref(cpu) + lag;
  tctx->vtime_cpu = cpu;
}

// Charge @used ns of cpu time to the vtime of a task.
static __always_inline void charge_vtime(const struct task_struct* p, struct task_ctx* tctx, u64 used)
{
  u64 weight = p->scx.weight ? p->scx.weight : 1;
  tctx->vtime += used * NICE_0_WEIGHT / weight;
}

// Deadline from the time a task is queued, its slack and its lag.
static __always_inline u64 make_deadline(u64 now, u64 slack, s64 lag)
{
  return now + slack + lag;
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
