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

// A task waits for @cpu: one in its queue, or one stuck on a cpu taken by an RT
// task, which this cpu takes over when its slice ends (see dispatch).
static __always_inline bool tasks_waiting(u32 cpu)
{
  return dsq_queued(cpu_dsq(cpu)) || nr_rt_busy;
}

// The slice of the task running on cpu @key is over: if another task waits for
// the cpu, end it now instead of at the next tick.
static int slice_timer_fn(void* map, u32* key, struct elara_timer* st)
{
  u32 cpu = *key;
  struct dispatch_ctx* dctx = get_dispatch_ctx(cpu);
  if (!dctx)
    return 0;
  dctx->timer_end = 0;
  if (dctx->running_deadline == DEADLINE_NONE || !tasks_waiting(cpu))
    return 0;

  // The timer was set for an earlier slice, which ended sooner (see
  // arm_slice_timer()): on to the end of the current one.
  u64 now = bpf_ktime_get_ns();
  if (now < dctx->slice_end)
  {
    dctx->timer_end = dctx->slice_end;
    bpf_timer_start(&st->timer, dctx->slice_end - now, BPF_F_TIMER_CPU_PIN);
    return 0;
  }
  scx_bpf_kick_cpu(cpu, SCX_KICK_PREEMPT);
  return 0;
}

// Make sure the slice timer of @cpu fires at the end of the slice of its running
// task. Only done when a task waits for the cpu: when the slice starts with a
// non-empty queue, or when a task is queued behind the running one later.
//
// A timer still set for an earlier time (the end of an earlier slice, when tasks
// switch faster than their slices end) is left alone: it goes on to the end of
// the current slice when it fires. So it is started at most about once per
// slice, not for every switch.
static __always_inline void arm_slice_timer(u32 cpu, struct dispatch_ctx* dctx, u64 now)
{
  u64 end = dctx->slice_end;
  if (dctx->timer_end > now && dctx->timer_end <= end)
    return;
  dctx->timer_end = end;
  u32 key = cpu & (MAX_CPUS - 1);
  struct elara_timer* st = bpf_map_lookup_elem(&slice_timers, &key);
  if (!st)
    return;
  u64 left = end > now ? end - now : 0;
  // Pinned when set by the cpu itself: the timer fires there without an IPI.
  bpf_timer_start(&st->timer, left, cpu == bpf_get_smp_processor_id() ? BPF_F_TIMER_CPU_PIN : 0);
}

// A new slice of @slice ns starts on @cpu (this cpu).
static __always_inline void start_slice(u32 cpu, struct dispatch_ctx* dctx, u64 now, u64 slice)
{
  dctx->slice_end = now + slice;
  if (tasks_waiting(cpu))
    arm_slice_timer(cpu, dctx, now);
}

// An RT or deadline task has kept cpu @key - MAX_CPUS for RT_EVACUATE_DELAY_NS:
// the tasks picked for it go back through enqueue(), which places them on
// other cpus.
static int rt_timer_fn(void* map, u32* key, struct elara_timer* st)
{
  u32 cpu = *key - MAX_CPUS;
  struct dispatch_ctx* dctx = get_dispatch_ctx(cpu);
  if (!dctx || !dctx->rt_busy || cpu != bpf_get_smp_processor_id())
    return 0;
  if (dsq_queued(SCX_DSQ_LOCAL_ON | cpu))
    scx_bpf_reenqueue_local_from_anywhere();
  return 0;
}

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
    dispatch_ctx->last_progress = 0;
    dispatch_ctx->slice_end = 0;
    dispatch_ctx->timer_end = 0;
    dispatch_ctx->local_weight = 0;
    dispatch_ctx->preempt_pending = false;

    u32 key = cpu & (MAX_CPUS - 1);
    struct elara_timer* st = bpf_map_lookup_elem(&slice_timers, &key);
    if (!st)
      return -ENOMEM;
    ret = bpf_timer_init(&st->timer, &slice_timers, CLOCK_MONOTONIC);
    if (ret)
      return ret;
    ret = bpf_timer_set_callback(&st->timer, slice_timer_fn);
    if (ret)
      return ret;

    key = (cpu & (MAX_CPUS - 1)) + MAX_CPUS;
    st = bpf_map_lookup_elem(&slice_timers, &key);
    if (!st)
      return -ENOMEM;
    ret = bpf_timer_init(&st->timer, &slice_timers, CLOCK_MONOTONIC);
    if (ret)
      return ret;
    ret = bpf_timer_set_callback(&st->timer, rt_timer_fn);
    if (ret)
      return ret;

    // scx_bpf_cpu_curr() needs an RCU read section, which a sleepable callback
    // doesn't have by itself.
    bpf_rcu_read_lock();
    dispatch_ctx->rt_busy = cpu_is_online(cpu) && cpu_taken_by_rt(cpu);
    bpf_rcu_read_unlock();
    if (dispatch_ctx->rt_busy)
      __sync_fetch_and_add(&nr_rt_busy, 1);
  }

  elara_ready = true;
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
  tctx->boost_weight = 0;
  tctx->boost_depth = 0;
  tctx->boost_used = 0;
  tctx->boost_limit = 0;
  tctx->boost_credit = WAKE_BOOST_BUDGET_NS;
  tctx->boost_credit_ts = now;
  tctx->started_at = now;
  tctx->granted_slice = 0;
  tctx->resume_slice = 0;
  tctx->last_migrated_at = 0;
  tctx->upmigrate_cpu = 0;

  return 0;
}

void BPF_STRUCT_OPS(elara_exit_task, struct task_struct* p, struct scx_exit_task_args* args) { }

// Wake boost: a task woken by a task with a shorter slack takes over that slack
// until it sleeps again, for a limited amount of cpu time.
//
// The waker passes on its own slack and weight, or, while it runs with a boost
// itself that is less than WAKE_BOOST_CHAIN_MAX steps from the start of its
// chain and not used up, that boost weakened by WAKE_BOOST_CHAIN_DECAY_NUM /
// _DEN, if that is better. A chain started by a nice -5 task gets its slack
// (~0.4ms) at the first step, then ~0.6, ~0.9 and ~1.35ms (nice 0: 3ms).
static __always_inline void apply_wake_boost(struct task_struct* p, struct task_ctx* tctx)
{
  if (!bpf_in_task())
    return;

  struct task_struct* waker = bpf_get_current_task_btf();
  if (!waker || waker == p || (waker->flags & PF_IDLE))
    return;
  if (!WAKE_BOOST_FROM_KTHREADS && (waker->flags & PF_KTHREAD))
    return;

  u64 waker_slack = task_slack(waker);
  u64 waker_weight = task_weight(waker);
  u32 depth = 1;
  struct task_ctx* wctx = get_task_ctx(waker);
  if (wctx && wctx->boost_slack && wctx->boost_depth < WAKE_BOOST_CHAIN_MAX && !boost_spent(wctx))
  {
    u64 chain_slack = wctx->boost_slack * WAKE_BOOST_CHAIN_DECAY_NUM / WAKE_BOOST_CHAIN_DECAY_DEN;
    if (chain_slack < waker_slack)
    {
      waker_slack = chain_slack;
      u64 chain_weight = wctx->boost_weight * WAKE_BOOST_CHAIN_DECAY_DEN / WAKE_BOOST_CHAIN_DECAY_NUM;
      waker_weight = chain_weight > waker_weight ? chain_weight : waker_weight;
      depth = wctx->boost_depth + 1;
    }
  }

  if (waker_slack >= task_slack(p) || (tctx->boost_slack && waker_slack >= tctx->boost_slack))
    return;

  refill_boost_credit(tctx, bpf_ktime_get_ns());
  if (tctx->boost_credit < WAKE_BOOST_MIN_NS)
    return;

  tctx->boost_slack = waker_slack;
  tctx->boost_weight = waker_weight;
  tctx->boost_depth = depth;
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

  // With fast and slower cores: an idle cpu of the kind the task prefers, its
  // previous one if that is such a cpu and idle.
  s32 cpu = -1;
  if (hybrid && tctx && p->nr_cpus_allowed > 1)
  {
    bool fast = wants_fast_core(p, tctx);
    if ((fast || wants_slow_core(p, tctx)) && cpu_is_fast(prev_cpu) == fast && bpf_cpumask_test_cpu(prev_cpu, p->cpus_ptr) &&
        scx_bpf_test_and_clear_cpu_idle(prev_cpu))
      cpu = prev_cpu;
    else
      cpu = claim_idle_preferred(p, tctx);
    is_idle = cpu >= 0;
  }
  if (cpu < 0)
    cpu = scx_bpf_select_cpu_dfl(p, prev_cpu, wake_flags, &is_idle);

  // Run directly on the idle cpu, unless tasks with an earlier deadline are
  // already queued there (their kick is still on the way): the local DSQ runs
  // before them. Otherwise enqueue() queues it properly.
  if (is_idle && tctx)
  {
    u64 now = bpf_ktime_get_ns();
    u64 slack = wakeup_slack(p, tctx);
    s64 lag = task_lag(p, tctx, true);
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

  // With SMT or fast and slower cores (SCX_OPS_ENQ_LAST): dispatch didn't let
  // the task go on alone on its cpu because a better one is idle
  // (upmigrate_cpu), or it found nothing to run at all. The cpu is about to go
  // idle.
  if (enq_flags & SCX_ENQ_LAST)
  {
    u32 up = tctx ? tctx->upmigrate_cpu : 0;
    if (tctx)
      tctx->upmigrate_cpu = 0;
    if (up && cpu_is_online(up - 1) && bpf_cpumask_test_cpu(up - 1, p->cpus_ptr))
    {
      scx_bpf_dsq_insert(p, SCX_DSQ_LOCAL_ON | (up - 1), SLICE_NS, enq_flags);
      scx_bpf_kick_cpu(up - 1, SCX_KICK_IDLE);
      return;
    }
    // It can't go there any more (its affinity changed or the cpu went
    // offline): the cpu claimed for it stays idle, kicked so it counts as idle
    // again.
    if (up)
      scx_bpf_kick_cpu(up - 1, SCX_KICK_IDLE);
    scx_bpf_dsq_insert(p, SCX_DSQ_LOCAL, SLICE_NS, enq_flags);
    return;
  }

  // The kernel put the task back another way (an RT task took the cpu first):
  // the cpu claimed for it stays idle, kicked so it counts as idle again.
  if (tctx && tctx->upmigrate_cpu)
  {
    scx_bpf_kick_cpu(tctx->upmigrate_cpu - 1, SCX_KICK_IDLE);
    tctx->upmigrate_cpu = 0;
  }

  if (!tctx)
  {
    queue_on(cpu, now);
    scx_bpf_dsq_insert_vtime(p, cpu_dsq(cpu), SLICE_NS, cpu_vtime_ref(cpu) + task_slack(p), enq_flags);
    scx_bpf_kick_cpu(cpu, SCX_KICK_IDLE);
    return;
  }

  if (enq_flags & SCX_ENQ_WAKEUP)
    apply_wake_boost(p, tctx);

  // An RT or deadline task preempted it while it ran (enq_immed). It keeps its
  // key, deadline and the rest of its slice. An idle cpu runs it right away.
  // Otherwise it goes to the front of its own queue: most RT tasks are done in
  // a moment, and while the cpu stays taken the other cpus take it over as
  // soon as it is their earliest task (dispatch, step 1).
  if (reenq_preempted(p, enq_flags))
  {
    tctx->resume_slice = 0;
    u64 slice = p->scx.slice > RESUME_SLICE_MIN_NS ? p->scx.slice : RESUME_SLICE_MIN_NS;
    s32 idle = p->nr_cpus_allowed > 1 && !is_migration_disabled(p) ? pick_idle(p, tctx, cpu) : -1;
    if (idle >= 0 && cpu_is_online((u32)idle))
    {
      // running() moves the key over to the vtime of that cpu.
      scx_bpf_dsq_insert(p, SCX_DSQ_LOCAL_ON | (u32)idle, slice, enq_flags);
      scx_bpf_kick_cpu((u32)idle, SCX_KICK_IDLE);
      return;
    }
    queue_on(cpu, now);
    scx_bpf_dsq_insert_vtime(p, cpu_dsq(cpu), slice, tctx->key, enq_flags);
    kick_idle_for_waiting(cpu);
    return;
  }

  // A preempted task goes back to its own queue with the rest of its slice, its
  // key and deadline, unless its cpu is taken by an RT task now: then it is
  // placed like any other.
  if (tctx->resume_slice && cpu_taken_by_rt(cpu))
    tctx->resume_slice = 0;

  if (tctx->resume_slice)
  {
    u64 slice = tctx->resume_slice;
    tctx->resume_slice = 0;
    queue_on(cpu, now);
    scx_bpf_dsq_insert_vtime(p, cpu_dsq(cpu), slice, tctx->key, enq_flags);
    return;
  }

  u64 slack = (enq_flags & SCX_ENQ_WAKEUP) ? wakeup_slack(p, tctx) : effective_slack(p, tctx);
  s64 lag = task_lag(p, tctx, enq_flags & SCX_ENQ_WAKEUP);
  // A re-enqueue (the cpu was taken by an RT task) keeps the deadline: the task
  // has already waited for it.
  bool reenq = enq_flags & SCX_ENQ_REENQ;
  u64 deadline = reenq ? tctx->deadline : make_deadline(now, slack, lag);

  // A woken task goes where it runs soonest, so does a re-enqueued one (it was
  // about to run, see enq_immed), a task that comes back after its slice is
  // balanced by weight.
  u32 target = (enq_flags & SCX_ENQ_WAKEUP) || reenq ? (u32)pick_enqueue_cpu(p, tctx, deadline, slack, cpu, now, enq_flags & SCX_ENQ_WAKEUP)
                                                     : (u32)pick_balance_cpu(p, tctx, slack, cpu, now);
  if (!cpu_is_online(target))
    target = cpu;

  set_queue_keys(tctx, target, now, slack, lag);
  if (reenq)
    tctx->deadline = deadline;

  struct dispatch_ctx* dctx = get_dispatch_ctx(target);
  bool running_ours = dctx && dctx->running_deadline != DEADLINE_NONE;

  // Keys of the same cpu compare: a woken task preempts the running task when
  // it is clearly earlier. If it is also the earliest of the queue, it goes
  // straight to the local DSQ with SCX_ENQ_PREEMPT: the running task is
  // preempted right away. A kick (SCX_KICK_PREEMPT) only takes effect through
  // irq_work, which on the test VM cost a woken task ~1.5ms more than EEVDF.
  bool preempt = (enq_flags & SCX_ENQ_WAKEUP) && running_ours && tctx->key + PREEMPT_MARGIN_NS < dctx->running_key;
  // Another woken task is already on its way in front of the running one: an
  // earlier task takes its place (with enq_immed the kernel hands that one
  // back, otherwise it runs right after), a later one queues up behind it. The
  // slice of that one ends on time (start_slice() arms the timer).
  bool behind = preempt && dctx->preempt_key && tctx->key >= dctx->preempt_key;
  if (behind)
    preempt = false;
  if (preempt && tctx->key <= dsq_head_key(cpu_dsq(target)))
  {
    dctx->preempt_pending = true;
    dctx->preempt_key = tctx->key;
    scx_bpf_dsq_insert(p, SCX_DSQ_LOCAL_ON | target, SLICE_NS, enq_flags | SCX_ENQ_PREEMPT);
    return;
  }

  queue_on(target, now);
  scx_bpf_dsq_insert_vtime(p, cpu_dsq(target), SLICE_NS, tctx->key, enq_flags);

  if (behind)
    return;
  if (!running_ours)
  {
    scx_bpf_kick_cpu(target, SCX_KICK_IDLE);
    return;
  }

  // A task of the queue is even earlier: let dispatch pick.
  if (preempt)
  {
    dctx->preempt_pending = true;
    scx_bpf_kick_cpu(target, SCX_KICK_PREEMPT);
    return;
  }

  // The running task is past its slice (nothing waited when its slice timer
  // fired): end it now, not at the next tick. Otherwise end it on time.
  if (now >= dctx->slice_end)
  {
    scx_bpf_kick_cpu(target, SCX_KICK_PREEMPT);
    return;
  }
  arm_slice_timer(target, dctx, now);
  scx_bpf_kick_cpu(target, SCX_KICK_IDLE);
}

void BPF_STRUCT_OPS(elara_dispatch, s32 cpu, struct task_struct* prev)
{
  u64 prev_key = DEADLINE_NONE;
  u64 prev_deadline = DEADLINE_NONE;
  struct task_ctx* pctx = NULL;
  struct dispatch_ctx* dctx = get_dispatch_ctx(cpu);
  u64 now = bpf_ktime_get_ns();

  // The kernel only asks for a task when the local DSQ is empty: a task that
  // enqueue() put there to preempt (see preempt_key) is gone without having
  // started (handed back with enq_immed, dequeued). Otherwise its key would keep
  // later wakeups from preempting until the next task starts here.
  if (dctx)
    dctx->preempt_key = 0;

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
        prev_deadline = make_deadline(now, slack, task_lag(prev, pctx, false));
      }
    }
  }

  u32 res = dispatch_dsq_per_cpu(cpu, prev_key, prev_deadline);
  if (res == DSP_TOOK || !prev || !(prev->scx.flags & SCX_TASK_QUEUED))
    return;

  // Nothing else to run and prev can go on (a task without context): the kernel
  // keeps it running, with its default slice of 20ms if prev's is used up.
  if (res == DSP_NONE || !pctx || !dctx)
  {
    scx_bpf_task_set_slice(prev, SLICE_NS);
    if (dctx)
      dctx->last_progress = now;
    return;
  }

  // prev runs alone, and an idle cpu is clearly better for it (see
  // better_cpu_alone()): its slice stays used up, so the kernel hands it to
  // enqueue() (SCX_ENQ_LAST), which moves it there. The idle cpu can't take
  // over a running task by itself.
  if ((hybrid || smt) && !dsq_queued(cpu_dsq(cpu)))
  {
    s32 better = better_cpu_alone(prev, pctx, cpu, now);
    if (better >= 0)
    {
      pctx->upmigrate_cpu = (u32)better + 1;
      pctx->last_migrated_at = now;
      return;
    }
  }

  // prev keeps running for a new slice, a boost left over from a preemption
  // that didn't replace it ends here once its budget is used up.
  if (pctx->boost_slack && boost_spent(pctx))
  {
    pctx->boost_slack = 0;
    u64 slack = task_slack(prev);
    prev_key = pctx->vtime + slack;
    prev_deadline = make_deadline(now, slack, task_lag(prev, pctx, false));
  }

  // The vtime reference follows the running task also when it keeps the cpu:
  // otherwise a task that runs alone for long builds up a lag against its own
  // cpu and carries it along as debt when it moves.
  if ((s64)(pctx->vtime - dctx->vtime_ref) > 0)
    dctx->vtime_ref = pctx->vtime;

  pctx->key = prev_key;
  pctx->deadline = prev_deadline;
  dctx->last_progress = now;
  scx_bpf_task_set_slice(prev, SLICE_NS);
  pctx->granted_slice = SLICE_NS;
  start_slice(cpu, dctx, now, SLICE_NS);
  dctx->running_key = prev_key;
  dctx->running_deadline = prev_deadline;
  dctx->preempt_pending = false;
}

void BPF_STRUCT_OPS(elara_running, struct task_struct* p)
{
  u32 cpu = scx_bpf_task_cpu(p);
  struct dispatch_ctx* dispatch_ctx = get_dispatch_ctx(cpu);
  if (!dispatch_ctx)
    return;
  // A task started: the queue of this cpu moves (see cpu_stuck()).
  dispatch_ctx->last_progress = bpf_ktime_get_ns();

  struct task_ctx* context = get_task_ctx(p);
  if (!context)
    return;

  // A task taken over from another cpu brings its lag along, its key moves to
  // the vtime of this cpu.
  if (context->vtime_cpu != cpu)
  {
    u64 slack = context->key - context->vtime;
    rebase_vtime(context, cpu, task_lag(p, context, false));
    context->key = context->vtime + slack;
  }
  if ((s64)(context->vtime - dispatch_ctx->vtime_ref) > 0)
    dispatch_ctx->vtime_ref = context->vtime;

  dispatch_ctx->running_key = context->key;
  dispatch_ctx->running_deadline = context->deadline;
  dispatch_ctx->preempt_pending = false;

  // Out of the local DSQ (see note_local()).
  dispatch_ctx->local_weight = 0;
  dispatch_ctx->preempt_key = 0;

  // Tasks the kernel queues by itself (exiting tasks and tasks with migration
  // disabled go straight to the local DSQ) come with its default slice of 20ms.
  if (p->scx.slice > SLICE_NS)
    scx_bpf_task_set_slice(p, SLICE_NS);

  context->started_at = bpf_ktime_get_ns();
  // A cpu dispatch claimed for it that it didn't go to (the kernel put it back
  // without enqueue(), see SCX_EV_ENQ_SKIP_EXITING) stays idle: kicked so it
  // counts as idle again.
  if (context->upmigrate_cpu && context->upmigrate_cpu - 1 != cpu)
    scx_bpf_kick_cpu(context->upmigrate_cpu - 1, SCX_KICK_IDLE);
  context->upmigrate_cpu = 0;
  context->granted_slice = p->scx.slice;
  context->resume_slice = 0;
  start_slice(cpu, dispatch_ctx, context->started_at, p->scx.slice);

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
// is taken from us, from one of our tasks or from idle. If it keeps the cpu for
// RT_EVACUATE_DELAY_NS, the tasks already picked for it (the preempted one goes
// back to the front of them) are placed again. An idle cpu is woken right away
// for the first task waiting in its queue.
// (This replaces ops.cpu_release, which newer kernels deprecate.)
SEC("tp_btf/sched_switch")
int BPF_PROG(elara_sched_switch, bool preempt, struct task_struct* prev, struct task_struct* next, unsigned int prev_state)
{
  if (!elara_ready)
    return 0;

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

  // With enq_immed the preempted task already came back through enqueue().
  if (!enq_immed && dsq_queued(SCX_DSQ_LOCAL_ON | cpu))
  {
    u32 key = (cpu & (MAX_CPUS - 1)) + MAX_CPUS;
    struct elara_timer* st = bpf_map_lookup_elem(&slice_timers, &key);
    if (st)
      bpf_timer_start(&st->timer, RT_EVACUATE_DELAY_NS, BPF_F_TIMER_CPU_PIN);
  }
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
