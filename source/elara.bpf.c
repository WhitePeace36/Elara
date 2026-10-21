// SPDX-License-Identifier: GPL-2.0
//
// Author: Timon Stipkovits <timon2201@gmail.com>
//
// This software may be used and distributed according to the terms of the
// GNU General Public License version 2.

#include <include/scx/common.bpf.h>
#include <include/bpf_experimental.h>
#include <bpf/bpf_helpers.h>
#include "defines.h"
#include "helpers.h"
#include "datatypes.h"
#include "dispatches.h"

char _license[] SEC("license") = "GPL";

UEI_DEFINE(uei);

// callbacks

s32 BPF_STRUCT_OPS_SLEEPABLE(elara_init)
{
  s32 ret;

  u32 nr_cpu_ids = scx_bpf_nr_cpu_ids();

  u32 cpu;
  bpf_for(cpu, 0, nr_cpu_ids)
  {
    ret = scx_bpf_create_dsq(cpu_dsq(cpu), -1);
    if (ret)
      return ret;
  }

  // A cpu that already runs an RT task when the scheduler starts doesn't switch
  // again until that task is done, so it is counted here.
  nr_rt_busy = 0;
  bpf_for(cpu, 0, nr_cpu_ids)
  {
    struct dispatch_ctx* dispatch_ctx = get_dispatch_ctx(cpu);
    if (!dispatch_ctx)
      return -ENOMEM;

    dispatch_ctx->running_deadline = DEADLINE_NONE;
    dispatch_ctx->running_key = DEADLINE_NONE;
    dispatch_ctx->vtime_ref = VTIME_BASE;
    dispatch_ctx->last_starve_check = 0;
    dispatch_ctx->preempt_pending = false;

    // scx_bpf_cpu_curr() needs an RCU read section, which a sleepable callback
    // doesn't have by itself.
    bpf_rcu_read_lock();
    dispatch_ctx->rt_busy = cpu_is_online(cpu) && cpu_taken_by_rt(cpu);
    bpf_rcu_read_unlock();
    if (dispatch_ctx->rt_busy)
      __sync_fetch_and_add(&nr_rt_busy, 1);
  }

  return 0;
}

s32 BPF_STRUCT_OPS_SLEEPABLE(elara_init_task, struct task_struct* p, struct scx_init_task_args* args)
{
  struct task_ctx* tctx;
  u64 now = bpf_ktime_get_ns();

  tctx = bpf_task_storage_get(&task_ctx_store, p, NULL, BPF_LOCAL_STORAGE_GET_F_CREATE);
  if (!tctx)
    return -ENOMEM;

  tctx->deadline = now;
  tctx->key = VTIME_BASE;
  tctx->vtime = VTIME_BASE;
  tctx->vtime_cpu = VTIME_CPU_NONE;
  tctx->boost_slack = 0;
  tctx->boost_used = 0;
  tctx->boost_limit = 0;
  tctx->boost_credit = WAKE_BOOST_BUDGET_NS;
  tctx->boost_credit_ts = now;
  tctx->started_at = now;
  tctx->granted_slice = 0;
  tctx->resume_slice = 0;
  tctx->last_migrated_at = 0;

  return 0;
}

void BPF_STRUCT_OPS(elara_exit_task, struct task_struct* p, struct scx_exit_task_args* args) { }

// Wake boost: a task woken by a task with a shorter slack takes over that slack
// until it sleeps again, for a limited amount of cpu time.
static __always_inline void apply_wake_boost(struct task_struct* p, struct task_ctx* tctx)
{
  if (!bpf_in_task())
    return;

  struct task_struct* waker = bpf_get_current_task_btf();
  if (!waker || waker == p || (waker->flags & PF_IDLE))
    return;
  if (!WAKE_BOOST_FROM_KTHREADS && (waker->flags & PF_KTHREAD))
    return;

  // The waker's own slack, not a boost it runs with itself: otherwise a boost
  // passes on to everything the boosted task wakes, and from there on.
  u64 waker_slack = task_slack(waker);
  if (waker_slack >= task_slack(p) || (tctx->boost_slack && waker_slack >= tctx->boost_slack))
    return;

  refill_boost_credit(tctx, bpf_ktime_get_ns());
  if (tctx->boost_credit < WAKE_BOOST_MIN_NS)
    return;

  tctx->boost_slack = waker_slack;
  tctx->boost_used = 0;
  tctx->boost_limit = tctx->boost_credit < WAKE_BOOST_BUDGET_NS ? tctx->boost_credit : WAKE_BOOST_BUDGET_NS;
}

// Set the key and deadline a task is queued with on @cpu: its lag moves along
// to @cpu, @slack is the slack for this time in the queue.
static __always_inline void set_queue_keys(struct task_ctx* tctx, u32 cpu, u64 now, u64 slack, s64 lag)
{
  rebase_vtime(tctx, cpu, lag);
  tctx->key = tctx->vtime + slack;
  tctx->deadline = make_deadline(now, slack, lag);
}

// Slack of a task for the time in the queue after a wakeup.
static __always_inline u64 wakeup_slack(struct task_struct* p, struct task_ctx* tctx)
{
  return effective_slack(p, tctx) / WAKE_SLACK_DIV;
}

s32 BPF_STRUCT_OPS(elara_select_cpu, struct task_struct* p, s32 prev_cpu, u64 wake_flags)
{
  bool is_idle = false;
  struct task_ctx* tctx = get_task_ctx(p);

  if (tctx && (wake_flags & SCX_WAKE_TTWU))
    apply_wake_boost(p, tctx);

  s32 cpu = scx_bpf_select_cpu_dfl(p, prev_cpu, wake_flags, &is_idle);

  // Run directly on the idle cpu, unless tasks with an earlier deadline are
  // already queued there (their kick is still on the way): the local DSQ runs
  // before them. Otherwise enqueue() queues it properly.
  if (is_idle && tctx)
  {
    u64 now = bpf_ktime_get_ns();
    u64 slack = wakeup_slack(p, tctx);
    s64 lag = task_lag(p, tctx);
    if (cpu_load_ahead(cpu, make_deadline(now, slack, lag), true) == 0)
    {
      set_queue_keys(tctx, cpu, now, slack, lag);
      scx_bpf_dsq_insert(p, SCX_DSQ_LOCAL, SLICE_NS, 0);
    }
  }

  return cpu;
}

void BPF_STRUCT_OPS(elara_enqueue, struct task_struct* p, u64 enq_flags)
{
  struct task_ctx* tctx = get_task_ctx(p);
  u32 cpu = scx_bpf_task_cpu(p);
  u64 now = bpf_ktime_get_ns();

  if (!tctx)
  {
    scx_bpf_dsq_insert_vtime(p, cpu_dsq(cpu), SLICE_NS, cpu_vtime_ref(cpu) + task_slack(p), enq_flags);
    scx_bpf_kick_cpu(cpu, SCX_KICK_IDLE);
    return;
  }

  if (enq_flags & SCX_ENQ_WAKEUP)
    apply_wake_boost(p, tctx);

  // A preempted task goes back to its own queue with the rest of its slice, its
  // key and deadline, unless its cpu is taken by an RT task now: then it is
  // placed like any other.
  if (tctx->resume_slice && cpu_taken_by_rt(cpu))
    tctx->resume_slice = 0;

  if (tctx->resume_slice)
  {
    u64 slice = tctx->resume_slice;
    tctx->resume_slice = 0;
    scx_bpf_dsq_insert_vtime(p, cpu_dsq(cpu), slice, tctx->key, enq_flags);
    return;
  }

  u64 slack = (enq_flags & SCX_ENQ_WAKEUP) ? wakeup_slack(p, tctx) : effective_slack(p, tctx);
  s64 lag = task_lag(p, tctx);
  // A re-enqueue (the cpu was taken by an RT task) keeps the deadline: the task
  // has already waited for it.
  bool reenq = enq_flags & SCX_ENQ_REENQ;
  u64 deadline = reenq ? tctx->deadline : make_deadline(now, slack, lag);

  // A woken task goes where it runs soonest, a task that comes back after its
  // slice is balanced by weight.
  u32 target = (enq_flags & SCX_ENQ_WAKEUP) ? (u32)pick_enqueue_cpu(p, tctx, deadline, slack, cpu, now, true)
                                            : (u32)pick_balance_cpu(p, tctx, slack, cpu, now);
  if (!cpu_is_online(target))
    target = cpu;

  set_queue_keys(tctx, target, now, slack, lag);
  if (reenq)
    tctx->deadline = deadline;
  scx_bpf_dsq_insert_vtime(p, cpu_dsq(target), SLICE_NS, tctx->key, enq_flags);

  struct dispatch_ctx* dctx = get_dispatch_ctx(target);
  if (!dctx || dctx->running_deadline == DEADLINE_NONE)
  {
    scx_bpf_kick_cpu(target, SCX_KICK_IDLE);
    return;
  }

  // Keys of the same cpu compare: preempt the running task when the woken one is
  // clearly earlier.
  if ((enq_flags & SCX_ENQ_WAKEUP) && tctx->key + PREEMPT_MARGIN_NS < dctx->running_key)
  {
    dctx->preempt_pending = true;
    scx_bpf_kick_cpu(target, SCX_KICK_PREEMPT);
    return;
  }

  scx_bpf_kick_cpu(target, SCX_KICK_IDLE);
}

void BPF_STRUCT_OPS(elara_dispatch, s32 cpu, struct task_struct* prev)
{
  u64 prev_key = DEADLINE_NONE;
  u64 prev_deadline = DEADLINE_NONE;
  struct task_ctx* pctx = NULL;
  struct dispatch_ctx* dctx = get_dispatch_ctx(cpu);
  u64 now = bpf_ktime_get_ns();

  if (prev && (prev->scx.flags & SCX_TASK_QUEUED))
  {
    pctx = get_task_ctx(prev);
    if (pctx && dctx)
    {
      // The slice is over: a wake boost ends once its budget is used up. A
      // preemption (dispatch is called before stopping then) keeps the boost,
      // key and deadline: the task gets the rest of its slice back.
      u64 used = elapsed(now, pctx->started_at);
      charge_wake_boost(pctx, used);
      charge_vtime(prev, pctx, used);
      if (!dctx->preempt_pending && boost_spent(pctx))
        pctx->boost_slack = 0;
      pctx->granted_slice = pctx->granted_slice > used ? pctx->granted_slice - used : 0;
      pctx->started_at = now;
      if (dctx->preempt_pending)
      {
        prev_key = pctx->key;
        prev_deadline = pctx->deadline;
      }
      else
      {
        u64 slack = effective_slack(prev, pctx);
        prev_key = pctx->vtime + slack;
        prev_deadline = make_deadline(now, slack, task_lag(prev, pctx));
      }
    }
  }

  if (!dispatch_dsq_per_cpu(cpu, prev_key, prev_deadline) || !prev || !pctx || !dctx)
    return;

  // prev keeps running for a new slice, a boost left over from a preemption
  // that didn't replace it ends here once its budget is used up.
  if (pctx->boost_slack && boost_spent(pctx))
  {
    pctx->boost_slack = 0;
    u64 slack = task_slack(prev);
    prev_key = pctx->vtime + slack;
    prev_deadline = make_deadline(now, slack, task_lag(prev, pctx));
  }

  // The vtime reference follows the running task also when it keeps the cpu:
  // otherwise a task that runs alone for long builds up a lag against its own
  // cpu and carries it along as debt when it moves.
  if ((s64)(pctx->vtime - dctx->vtime_ref) > 0)
    dctx->vtime_ref = pctx->vtime;

  pctx->key = prev_key;
  pctx->deadline = prev_deadline;
  scx_bpf_task_set_slice(prev, SLICE_NS);
  pctx->granted_slice = SLICE_NS;
  dctx->running_key = prev_key;
  dctx->running_deadline = prev_deadline;
  dctx->preempt_pending = false;
}

void BPF_STRUCT_OPS(elara_running, struct task_struct* p)
{
  struct task_ctx* context = get_task_ctx(p);
  if (!context)
    return;

  u32 cpu = scx_bpf_task_cpu(p);
  struct dispatch_ctx* dispatch_ctx = get_dispatch_ctx(cpu);
  if (!dispatch_ctx)
    return;

  // A task taken over from another cpu brings its lag along, its key moves to
  // the vtime of this cpu.
  if (context->vtime_cpu != cpu)
  {
    u64 slack = context->key - context->vtime;
    rebase_vtime(context, cpu, task_lag(p, context));
    context->key = context->vtime + slack;
  }
  if ((s64)(context->vtime - dispatch_ctx->vtime_ref) > 0)
    dispatch_ctx->vtime_ref = context->vtime;

  dispatch_ctx->running_key = context->key;
  dispatch_ctx->running_deadline = context->deadline;
  dispatch_ctx->preempt_pending = false;

  context->started_at = bpf_ktime_get_ns();
  context->granted_slice = p->scx.slice;
  context->resume_slice = 0;

  kick_idle_for_waiting(cpu);
}

void BPF_STRUCT_OPS(elara_stopping, struct task_struct* task, bool runnable)
{
  u64 now = bpf_ktime_get_ns();

  struct task_ctx* tctx = get_task_ctx(task);
  if (!tctx)
    return;

  struct dispatch_ctx* dctx = get_dispatch_ctx(scx_bpf_task_cpu(task));
  if (!dctx)
    return;

  u64 used_ns = elapsed(now, tctx->started_at);
  charge_wake_boost(tctx, used_ns);
  charge_vtime(task, tctx, used_ns);

  if (dctx->preempt_pending && runnable && task->scx.slice == 0 && tctx->granted_slice > used_ns + RESUME_SLICE_MIN_NS)
    tctx->resume_slice = tctx->granted_slice - used_ns;

  // A wake boost ends when the task sleeps or its budget is used up, a
  // preempted task keeps it for the rest of its slice.
  if (!tctx->resume_slice && (!runnable || boost_spent(tctx)))
    tctx->boost_slack = 0;

  dctx->preempt_pending = false;
  dctx->running_deadline = DEADLINE_NONE;
  dctx->running_key = DEADLINE_NONE;
}

void BPF_STRUCT_OPS(elara_exit, struct scx_exit_info* ei)
{
  UEI_RECORD(uei, ei);
}

void BPF_STRUCT_OPS(elara_quiescent, struct task_struct* p, u64 deq_flags)
{
  struct task_ctx* tctx = get_task_ctx(p);
  if (!tctx)
    return;

  tctx->resume_slice = 0;
  tctx->boost_slack = 0;
}

// Every context switch: when the next task is an RT or deadline task, the cpu
// is taken from us, from one of our tasks or from idle. The tasks already picked
// for it are placed again, and an idle cpu is woken for the first one still
// waiting in its queue.
// (This replaces ops.cpu_release, which newer kernels deprecate.)
SEC("tp_btf/sched_switch")
int BPF_PROG(elara_sched_switch, bool preempt, struct task_struct* prev, struct task_struct* next, unsigned int prev_state)
{
  u32 cpu = bpf_get_smp_processor_id();
  struct dispatch_ctx* dctx = get_dispatch_ctx(cpu);

  bool rt = is_rt_task(next);
  if (dctx && rt != dctx->rt_busy)
  {
    dctx->rt_busy = rt;
    if (rt)
      __sync_fetch_and_add(&nr_rt_busy, 1);
    else
      __sync_fetch_and_sub(&nr_rt_busy, 1);
  }
  if (!rt)
    return 0;

  evacuate_rt_cpu(cpu);
  kick_idle_for_waiting(cpu);
  return 0;
}

SCX_OPS_DEFINE(elara_ops,
               .init = (void*)elara_init,
               .init_task = (void*)elara_init_task,
               .exit_task = (void*)elara_exit_task,
               .select_cpu = (void*)elara_select_cpu,
               .quiescent = (void*)elara_quiescent,
               .running = (void*)elara_running,
               .enqueue = (void*)elara_enqueue,
               .dispatch = (void*)elara_dispatch,
               .stopping = (void*)elara_stopping,
               .exit = (void*)elara_exit,
               .name = "scx_elara");
