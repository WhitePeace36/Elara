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

// Ticks per second of the kernel (its config, read by libbpf when loading), 0
// if unknown.
extern unsigned int CONFIG_HZ __kconfig __weak;

// The scheduler tick comes later than the end of a slice (HZ below 1000, or
// unknown): a task without a slice timer can run on for up to a tick after its
// slice ended (see BRIEF_RUN_NS).
static __always_inline bool slow_tick(void)
{
  return CONFIG_HZ < NS_PER_SEC / SLICE_NS;
}

// The first task waiting in the queue of @cpu slept before (a woken task, not
// one that comes back after its slice): a task running there that only runs
// for a moment gets a slice timer for it too (see BRIEF_RUN_NS).
static __always_inline bool sleeper_waits(u32 cpu)
{
  struct task_struct* p = dsq_first(cpu_dsq(cpu));
  struct task_ctx* tctx = p ? get_task_ctx(p) : NULL;
  return tctx && !tctx->cpu_bound;
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
    if (dctx->running_brief)
    {
      if (!slow_tick())
        return 0;
      // scx_bpf_dsq_peek() needs an RCU read section, which a timer callback
      // doesn't have by itself.
      bpf_rcu_read_lock();
      bool sleeper = sleeper_waits(cpu);
      bpf_rcu_read_unlock();
      if (!sleeper)
        return 0;
    }
    dctx->timer_end = dctx->slice_end;
    bpf_timer_start(&st->timer, dctx->slice_end - now, BPF_F_TIMER_CPU_PIN);
    return 0;
  }
  scx_bpf_kick_cpu(cpu, SCX_KICK_PREEMPT);
  return 0;
}

// Make sure the slice timer of @cpu fires at the end of the slice of its running
// task. Only done when a task waits for the cpu: when the slice starts with a
// non-empty queue, or when a task is queued behind the running one later. For
// a running task that only runs for a moment (see BRIEF_RUN_NS) only when a
// task that slept waits and the tick is slow (@sleeper, see slow_tick()).
//
// A timer still set for an earlier time (the end of an earlier slice, when tasks
// switch faster than their slices end) is left alone: it goes on to the end of
// the current slice when it fires. So it is started at most about once per
// slice, not for every switch.
static __always_inline void arm_slice_timer(u32 cpu, struct dispatch_ctx* dctx, u64 now, bool sleeper)
{
  if (dctx->running_brief && !sleeper)
    return;
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
    arm_slice_timer(cpu, dctx, now, dctx->running_brief && slow_tick() && sleeper_waits(cpu));
}

// The kernel is built with PREEMPT_RT (read by libbpf when loading).
extern bool CONFIG_PREEMPT_RT __kconfig __weak;

// An RT or deadline task has kept cpu @key - MAX_CPUS for RT_EVACUATE_DELAY_NS:
// the tasks picked for it go back through enqueue(), which places them on
// other cpus, and an idle cpu is woken for the first task of its queue.
static int rt_timer_fn(void* map, u32* key, struct elara_timer* st)
{
  u32 cpu = *key - MAX_CPUS;
  struct dispatch_ctx* dctx = get_dispatch_ctx(cpu);
  if (!dctx || dctx->rt_busy != RT_BUSY_YES || cpu != bpf_get_smp_processor_id())
    return 0;
  if (dsq_queued(SCX_DSQ_LOCAL_ON | cpu))
    scx_bpf_reenqueue_local_from_anywhere();
  // scx_bpf_dsq_peek() needs an RCU read section, which a timer callback
  // doesn't have by itself.
  bpf_rcu_read_lock();
  kick_idle_for_waiting(cpu);
  bpf_rcu_read_unlock();
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
    dispatch_ctx->starve_skip = 0;
    dispatch_ctx->last_progress = 0;
    dispatch_ctx->slice_end = 0;
    dispatch_ctx->timer_end = 0;
    dispatch_ctx->local_weight = 0;
    dispatch_ctx->queued_weight = 0;
    dispatch_ctx->preempt_pending = false;
    dispatch_ctx->rt_busy = RT_BUSY_UNKNOWN;

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
  }

  // From here on the sched_switch tracepoint follows the cpus (the queues,
  // contexts and timers it uses exist now).
  elara_ready = true;

  // A cpu that already runs an RT task when the scheduler starts doesn't switch
  // again until that task is done, so it is counted here. Only after the
  // tracepoint is on, and only for cpus that didn't switch since (see
  // init_rt_busy()): looked at before, a switch in between was missed and the
  // cpu counted as taken (or not) until its next one.
  bpf_for(cpu, 0, nr_cpu_ids)
  {
    struct dispatch_ctx* dispatch_ctx = get_dispatch_ctx(cpu);
    if (!dispatch_ctx)
      return -ENOMEM;
    // scx_bpf_cpu_curr() needs an RCU read section, which a sleepable callback
    // doesn't have by itself.
    bpf_rcu_read_lock();
    bool rt = cpu_is_online(cpu) && cpu_taken_by_rt(cpu);
    bpf_rcu_read_unlock();
    init_rt_busy(dispatch_ctx, rt);
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
  tctx->cpu_bound = false;
  tctx->queued_cpu = -1;
  tctx->queued_w = 0;
  tctx->shared_since = 0;
  tctx->shared_seen = 0;
  tctx->turns_since = 0;
  tctx->burst = 0;
  // Not known to run only for a moment until it slept once: a new process (a
  // compiler started by make) would otherwise be kept in the llc of its parent.
  tctx->run_avg = SHORT_RUN_NS;
  tctx->stopped_at = 0;
  tctx->rescued = false;
  tctx->sync_wake = false;
  tctx->boost_checked = false;
  tctx->yield_pending = false;
  tctx->immed = false;

  return 0;
}

// The task that wakes @p, NULL if there is none: the wakeup comes from an
// interrupt (the current task is just the one that was interrupted), from the
// idle task, or from a kernel thread unless @kthreads.
static __always_inline struct task_struct* waker_of(const struct task_struct* p, bool kthreads)
{
  if (!bpf_in_task())
    return NULL;
  struct task_struct* waker = bpf_get_current_task_btf();
  if (!waker || waker == p || (waker->flags & PF_IDLE))
    return NULL;
  if (!kthreads && (waker->flags & PF_KTHREAD))
    return NULL;
  return waker;
}

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
  if (background_policy(p))
    return;
  struct task_struct* waker = waker_of(p, WAKE_BOOST_FROM_KTHREADS);
  if (!waker)
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

// Slack of a task for the time in the queue after a wakeup (see
// background_policy()).
static __always_inline u64 wakeup_slack(struct task_struct* p, struct task_ctx* tctx)
{
  if (background_policy(p))
    return effective_slack(p, tctx);
  return effective_slack(p, tctx) / WAKE_SLACK_DIV;
}

s32 BPF_STRUCT_OPS(elara_select_cpu, struct task_struct* p, s32 prev_cpu, u64 wake_flags)
{
  bool is_idle = false;
  struct task_ctx* tctx = get_task_ctx(p);

  if (tctx && (wake_flags & SCX_WAKE_TTWU))
  {
    apply_wake_boost(p, tctx);
    tctx->boost_checked = true;
  }
  if (tctx)
    tctx->sync_wake = wake_flags & SCX_WAKE_SYNC;

  // With fast and slower cores: an idle cpu of the kind the task prefers, its
  // previous one if that is such a cpu and idle.
  s32 cpu = -1;
  // No idle cpu @p may run on: the searches below would go through their cpus
  // one by one for nothing (see pick_idle()). Only looked at when one of them
  // can run: with one llc and all cores equally fast it was one more look at
  // the idle mask for every wakeup, for nothing.
  bool any_idle = (hybrid || nr_llcs > 1) && idle_cpu_allowed(p);
  if (hybrid && tctx && p->nr_cpus_allowed > 1 && any_idle)
  {
    bool fast = wants_fast_core(p, tctx);
    if ((fast || wants_slow_core(p, tctx)) && cpu_is_fast(prev_cpu) == fast && bpf_cpumask_test_cpu(prev_cpu, p->cpus_ptr) &&
        scx_bpf_test_and_clear_cpu_idle(prev_cpu))
      cpu = prev_cpu;
    else
      cpu = claim_idle_preferred(p, tctx);
    is_idle = cpu >= 0;
  }
  // The kernel's default takes an idle cpu of another llc when the own llc has
  // none, with SMT a whole idle core of another llc even before an idle cpu of
  // the own llc. A task that runs only for a moment stays in its llc (see
  // SHORT_RUN_NS): on an idle cpu there, or queued on a cpu there that is free
  // again in a moment (enqueue() places it). Woken by a task that runs only
  // for a moment too (they take turns, a pipe, a futex), the llc of its waker:
  // otherwise once they were apart each would stay near its own previous cpu.
  // Not a wakeup from an interrupt (the current task only happened to run
  // there: a disk or network interrupt, a timer) or by a kernel thread (a
  // kworker finishing I/O is no partner).
  if (cpu < 0 && nr_llcs > 1 && p->nr_cpus_allowed > 1 && runs_short(tctx))
  {
    s32 near = prev_cpu;
    struct task_struct* waker = waker_of(p, false);
    s32 this_cpu = bpf_get_smp_processor_id();
    if (waker && bpf_cpumask_test_cpu(this_cpu, p->cpus_ptr) && runs_short(get_task_ctx(waker)))
      near = this_cpu;
    if (any_idle)
      cpu = claim_idle_near(p, (u32)near, (u32)prev_cpu);
    is_idle = cpu >= 0;
    if (!is_idle)
      cpu = llc_frees_soon(p, (u32)near);
  }
  if (cpu < 0)
    cpu = scx_bpf_select_cpu_dfl(p, prev_cpu, wake_flags, &is_idle);

  // Run directly on the idle cpu, unless tasks with an earlier deadline are
  // already queued there (their kick is still on the way): the local DSQ runs
  // before them. Otherwise enqueue() queues it properly.
  //
  // Not onto a cpu that runs a task of ours: for a sync wakeup the kernel's
  // default returns the cpu of the waker as "idle" (the waker is expected to
  // sleep right away) while the waker still runs there. Put into the local DSQ
  // from here, a task that preempts the waker by key waited behind it without
  // SCX_ENQ_PREEMPT and without a slice timer, until the waker slept or the
  // tick ended its slice (kernels before 7.1 don't hand it back), while a less
  // urgent task went to an idle cpu. enqueue() decides instead: it preempts
  // (see SYNC_PREEMPT_MIN_NS) or places the task elsewhere.
  if (is_idle)
  {
    struct dispatch_ctx* d = get_dispatch_ctx(cpu);
    if (d && d->running_deadline != DEADLINE_NONE)
      is_idle = false;
  }
  if (is_idle && tctx)
  {
    u64 now = bpf_ktime_get_ns();
    u64 slack = wakeup_slack(p, tctx);
    s64 lag = task_lag(p, tctx, true);
    if (cpu_load_ahead(cpu, slack, lag, !background_policy(p), now) == 0)
    {
      set_queue_keys(tctx, cpu, now, slack, lag);
      note_local_bound((u32)cpu, p);
      tctx->immed = true;
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
      note_local_bound(up - 1, p);
      scx_bpf_dsq_insert(p, SCX_DSQ_LOCAL_ON | (up - 1), SLICE_NS, enq_flags);
      scx_bpf_kick_cpu(up - 1, SCX_KICK_IDLE);
      return;
    }
    // It can't go there any more (its affinity changed or the cpu went
    // offline): the cpu claimed for it stays idle, kicked so it counts as idle
    // again.
    if (up)
      scx_bpf_kick_cpu(up - 1, SCX_KICK_IDLE);
    note_local_bound(cpu, p);
    scx_bpf_dsq_insert(p, SCX_DSQ_LOCAL, SLICE_NS, enq_flags);
    // The cpu is already switching to the idle task (that is what
    // SCX_ENQ_LAST says), and the kernel doesn't reschedule it for a task put
    // into its local DSQ now: the scheduler has to. The kick takes effect
    // once the cpu is idle and makes it pick the task; without it the task
    // waited there until something else woke the cpu.
    scx_bpf_kick_cpu(cpu, SCX_KICK_IDLE);
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

  // Not again for a wakeup select_cpu() already looked at. The kernel doesn't
  // ask select_cpu() for a task bound to one cpu: its boost is decided here.
  if ((enq_flags & SCX_ENQ_WAKEUP) && !tctx->boost_checked)
    apply_wake_boost(p, tctx);
  tctx->boost_checked = false;

  // An RT or deadline task preempted it while it ran (enq_immed). It keeps its
  // key, deadline and the rest of its slice. An idle cpu runs it right away.
  // Otherwise it goes to the front of its own queue: most RT tasks are done in
  // a moment, and while the cpu stays taken the other cpus take it over as
  // soon as it is their earliest task (dispatch, step 1).
  if (reenq_preempted(p, enq_flags))
  {
    tctx->resume_slice = 0;
    u64 slice = p->scx.slice > RESUME_SLICE_MIN_NS ? p->scx.slice : RESUME_SLICE_MIN_NS;
    s32 idle = p->nr_cpus_allowed > 1 && !is_migration_disabled(p) ? pick_idle(p, cpu) : -1;
    if (idle >= 0 && cpu_is_online((u32)idle))
    {
      // running() moves the key over to the vtime of that cpu.
      note_local_bound((u32)idle, p);
      scx_bpf_dsq_insert(p, SCX_DSQ_LOCAL_ON | (u32)idle, slice, enq_flags);
      scx_bpf_kick_cpu((u32)idle, SCX_KICK_IDLE);
      return;
    }
    queue_on(cpu, now);
    note_queued(p, tctx, cpu);
    scx_bpf_dsq_insert_vtime(p, cpu_dsq(cpu), slice, tctx->key, enq_flags);
    // For the task waiting at the head of the queue (this one isn't in it
    // before enqueue() returns).
    kick_idle_for_waiting(cpu);
    return;
  }

  // A woken task handed back from the local DSQ it was put into to run right
  // away, before it started (see reenq_immed()): when a second woken task was
  // put there before the cpu switched (an earlier one takes the place of the
  // one on its way, below), the kernel handed back both. As a plain
  // re-enqueue it lost its wakeup slack and preemption and waited for the end
  // of the running slice. While a task of ours runs on its cpu, it keeps its
  // key, goes into the queue of that cpu and preempts by a kick if it is
  // earlier than the running task; dispatch then runs such tasks by key. Not
  // into the local DSQ again: it could be handed back again, and after a few
  // times the kernel stops the scheduler.
  //
  // Otherwise an RT or deadline task takes the cpu, or the cpu is switching to
  // another task that got there first: nothing of ours runs there to preempt.
  // An idle cpu runs it right away (running() moves its key there), otherwise
  // it waits in the queue of its cpu, where the other cpus take it over as
  // soon as it is their earliest task while an RT task keeps the cpu
  // (dispatch, step 1). Kicked into its queue alone, it waited until the RT
  // task was done, or a slice behind the other task.
  if (tctx->immed && reenq_immed(p, enq_flags))
  {
    tctx->immed = false;
    struct dispatch_ctx* d = get_dispatch_ctx(cpu);
    bool ours = d && d->running_deadline != DEADLINE_NONE && !cpu_taken_by_rt(cpu);
    s32 idle = !ours && p->nr_cpus_allowed > 1 && !is_migration_disabled(p) ? pick_idle(p, cpu) : -1;
    if (idle >= 0 && cpu_is_online((u32)idle))
    {
      note_local_bound((u32)idle, p);
      tctx->immed = true;
      scx_bpf_dsq_insert(p, SCX_DSQ_LOCAL_ON | (u32)idle, SLICE_NS, enq_flags);
      scx_bpf_kick_cpu((u32)idle, SCX_KICK_IDLE);
      return;
    }
    queue_on(cpu, now);
    note_queued(p, tctx, cpu);
    scx_bpf_dsq_insert_vtime(p, cpu_dsq(cpu), SLICE_NS, tctx->key, enq_flags);
    if (!ours)
      return;
    if (key_before(tctx->key + PREEMPT_MARGIN_NS, d->running_key))
    {
      d->preempt_pending = true;
      scx_bpf_kick_cpu(cpu, SCX_KICK_PREEMPT);
    }
    else
      arm_slice_timer(cpu, d, now, slow_tick() && !tctx->cpu_bound);
    return;
  }
  tctx->immed = false;

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
    note_queued(p, tctx, cpu);
    scx_bpf_dsq_insert_vtime(p, cpu_dsq(cpu), slice, tctx->key, enq_flags);
    return;
  }

  u64 slack = (enq_flags & SCX_ENQ_WAKEUP) ? wakeup_slack(p, tctx) : effective_slack(p, tctx);
  s64 lag = task_lag(p, tctx, enq_flags & SCX_ENQ_WAKEUP);
  // A re-enqueue (the cpu was taken by an RT task) keeps the deadline: the task
  // has already waited for it.
  bool reenq = enq_flags & SCX_ENQ_REENQ;
  u64 kept_deadline = tctx->deadline;

  // A woken task goes where it runs soonest, so does a re-enqueued one (it was
  // about to run, see enq_immed), a task that comes back after its slice is
  // balanced by weight. Background work is placed as what it is below: woken,
  // but it doesn't preempt (like select_cpu() does). Placed as if it would,
  // its own busy cpu looked free (no work ahead) and it was queued there,
  // behind the running task, without looking for an idle cpu.
  bool may_preempt = (enq_flags & SCX_ENQ_WAKEUP) && !background_policy(p);
  // A task that yielded stays on its cpu: it gave the cpu up to wait for
  // something, often a task of the same cpu, not because there is too much
  // work there. Balanced, every yield cost a weight scan of the llc
  // (stress-ng --resched took twice as long).
  bool yielded = tctx->yield_pending;
  tctx->yield_pending = false;
  u32 target = cpu;
  if ((enq_flags & SCX_ENQ_WAKEUP) || reenq)
    target = (u32)pick_enqueue_cpu(p, lag, slack, cpu, may_preempt);
  else if (!yielded)
    target = (u32)pick_balance_cpu(p, slack, cpu);
  if (!cpu_is_online(target))
    target = cpu;
  if (target != cpu)
  {
    tctx->turns_since = 0;
    tctx->shared_since = 0;
  }

  set_queue_keys(tctx, target, now, slack, lag);
  if (reenq)
    tctx->deadline = kept_deadline;

  struct dispatch_ctx* dctx = get_dispatch_ctx(target);
  bool running_ours = dctx && dctx->running_deadline != DEADLINE_NONE;

  // Keys of the same cpu compare: a woken task preempts the running task when
  // it is clearly earlier. If it is also the earliest of the queue, it goes
  // straight to the local DSQ with SCX_ENQ_PREEMPT: the running task is
  // preempted right away. A kick (SCX_KICK_PREEMPT) only takes effect through
  // irq_work, which on the test VM cost a woken task ~1.5ms more than EEVDF.
  // Background work doesn't preempt (see background_policy()).
  bool preempt = may_preempt && running_ours && key_before(tctx->key + PREEMPT_MARGIN_NS, dctx->running_key);
  // The waker runs on @target (this cpu, called from its wakeup) and the
  // wakeup is a sync one (see SYNC_PREEMPT_MIN_NS).
  bool sync = tctx->sync_wake;
  tctx->sync_wake = false;
  if (preempt && sync && bpf_in_task() && target == bpf_get_smp_processor_id() && effective_slack(p, tctx) >= dctx->running_slack &&
      now - dctx->running_since < SYNC_PREEMPT_MIN_NS)
    preempt = false;
  // Another woken task is already on its way in front of the running one: an
  // earlier task takes its place (with enq_immed the kernel hands that one
  // back, otherwise it runs right after), a later one queues up behind it. The
  // slice of that one ends on time (start_slice() arms the timer).
  bool behind = preempt && dctx->preempt_key && !key_before(tctx->key, dctx->preempt_key);
  if (behind)
    preempt = false;
  u64 head_key = preempt ? dsq_head_key(cpu_dsq(target)) : DEADLINE_NONE;
  if (preempt && (head_key == DEADLINE_NONE || !key_before(head_key, tctx->key)))
  {
    dctx->preempt_pending = true;
    dctx->preempt_key = tctx->key;
    note_local_bound(target, p);
    tctx->immed = true;
    scx_bpf_dsq_insert(p, SCX_DSQ_LOCAL_ON | target, SLICE_NS, enq_flags | SCX_ENQ_PREEMPT);
    return;
  }

  queue_on(target, now);
  note_queued(p, tctx, target);
  scx_bpf_dsq_insert_vtime(p, cpu_dsq(target), SLICE_NS, tctx->key, enq_flags);

  if (behind)
    return;
  // Wake the cpu if it is idle. A kick costs an irq_work interrupt even when
  // it does nothing, so not for a task queued on its own cpu when that cpu
  // picks a task anyway: a woken one (the wakeup reschedules the cpu if it is
  // idle, wakeup_preempt()), or one that comes back after its slice on this
  // cpu, which is about to pick the next task (not a re-enqueue, which can
  // come from a timer on an idle cpu).
  if (!running_ours)
  {
    bool picks_anyway = target == cpu && ((enq_flags & SCX_ENQ_WAKEUP) || (target == bpf_get_smp_processor_id() && bpf_in_task() && !reenq));
    if (!picks_anyway)
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
  arm_slice_timer(target, dctx, now, slow_tick() && !tctx->cpu_bound);
  // The running task stopped in the meantime, and its cpu may have looked for
  // a task before this one was queued: wake it. Otherwise it finds this task
  // when its running task stops. A kick always costs an irq_work interrupt
  // here, even when the cpu is busy and ignores it.
  if (*(volatile u64*)&dctx->running_deadline == DEADLINE_NONE)
    scx_bpf_kick_cpu(target, SCX_KICK_IDLE);
}

void BPF_STRUCT_OPS(elara_dispatch, s32 cpu, struct task_struct* prev)
{
  u64 prev_key = DEADLINE_NONE;
  u64 prev_deadline = DEADLINE_NONE;
  bool yielded = false;
  bool preempted = false;
  u64 slice_left = 0;
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
      pctx->burst += used;
      // Its slice ended long before it was used up, and nothing preempted
      // it: it called sched_yield() (without ops.yield the kernel just
      // sets the slice to 0). Handled like a used-up slice, the yield did
      // nothing while its key was still earlier than the head of the
      // queue (its vtime barely moved): a thread spinning on
      // sched_yield() kept half of a cpu that it shared with the thread
      // it waited for (EEVDF: 2%). Wine's SwitchToThread() is such a
      // yield. The kicks that end a slice on time come at its end (see
      // slice_timer_fn()).
      //
      // Like SwitchToThread() on Windows, which the games were written
      // for: the next task of the own cpu gets one turn, the yielding task
      // is charged only the time it used and keeps its place for its real
      // work after that. Unreal Engine's game and render threads yield
      // thousands of times a second; charged the rest of the slice for each
      // yield (EEVDF), they lost to every other task of their cpu after.
      slice_left = pctx->granted_slice > used ? pctx->granted_slice - used : 0;
      // A preemption (a kick, see enqueue()) counts only while the slice
      // isn't used up: when the slice ended by itself just before the kick
      // took effect, the task kept its key and deadline from before this
      // slice and its boost, and went on with them for another slice.
      preempted = dctx->preempt_pending && slice_left > RESUME_SLICE_MIN_NS;
      if (dctx->preempt_pending && !preempted)
        dctx->preempt_pending = false;
      yielded = !dctx->preempt_pending && slice_left > RESUME_SLICE_MIN_NS;
      // Its slice is used up (a preemption or a yield isn't that).
      if (!preempted && !yielded)
        pctx->cpu_bound = true;
      charge_wake_boost(pctx, used);
      charge_vtime(prev, pctx, used);
      if (!preempted && boost_spent(pctx))
        pctx->boost_slack = 0;
      pctx->granted_slice = slice_left;
      pctx->started_at = now;
      if (preempted)
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

  u32 res = dispatch_dsq_per_cpu(cpu, dctx, now, prev_key, prev_deadline, yielded);
  // It yielded and another task takes over: it comes back through enqueue()
  // now, which leaves it on this cpu (see yield_pending there).
  if (pctx)
    pctx->yield_pending = yielded && res == DSP_TOOK;
  if (res == DSP_TOOK || !prev || !(prev->scx.flags & SCX_TASK_QUEUED))
    return;

  // Nothing else to run and prev can go on (a task without context): the kernel
  // keeps it running, with its default slice of 20ms if prev's is used up.
  if (res == DSP_NONE || !pctx || !dctx)
  {
    if (pctx)
      pctx->turns_since = 0;
    scx_bpf_task_set_slice(prev, SLICE_NS);
    if (dctx)
    {
      dctx->last_progress = now;
      if (pctx)
      {
        dctx->running_cpu_bound = pctx->cpu_bound;
        dctx->running_short = runs_short(pctx);
        dctx->running_brief = false;
      }
    }
    return;
  }

  // It runs alone again: no more taking turns.
  if (!dsq_queued(cpu_dsq(cpu)))
    pctx->turns_since = 0;

  // prev runs alone, and an idle cpu is clearly better for it (see
  // better_cpu_alone()): its slice stays used up, so the kernel hands it to
  // enqueue() (SCX_ENQ_LAST), which moves it there. The idle cpu can't take
  // over a running task by itself.
  if ((hybrid || smt) && !dsq_queued(cpu_dsq(cpu)))
  {
    s32 better = better_cpu_alone(prev, pctx, cpu, now);
    if (better >= 0)
    {
      // Its key and deadline for the next slice, like below: with the old
      // ones, from before the vtime of this slice was charged, running()
      // took less than its slack over to the other cpu (for a task of low
      // weight less than none: a key earlier than its vtime).
      pctx->key = prev_key;
      pctx->deadline = prev_deadline;
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
  if (!pctx->rescued && (s64)(pctx->vtime - dctx->vtime_ref) > 0)
    dctx->vtime_ref = pctx->vtime;

  pctx->key = prev_key;
  pctx->deadline = prev_deadline;
  dctx->running_cpu_bound = pctx->cpu_bound;
  dctx->running_short = runs_short(pctx);
  dctx->running_brief = false;
  dctx->last_progress = now;
  // Preempted but nothing earlier was there when dispatch looked: it goes on
  // with the rest of its slice (like a preempted task that goes back to its
  // queue, see resume_slice), not with a new one.
  u64 slice = preempted ? slice_left : SLICE_NS;
  scx_bpf_task_set_slice(prev, slice);
  pctx->granted_slice = slice;
  start_slice(cpu, dctx, now, slice);
  dctx->running_key = prev_key;
  dctx->running_deadline = prev_deadline;
  dctx->running_since = now;
  dctx->running_slack = effective_slack(prev, pctx);
  dctx->preempt_pending = false;
}

void BPF_STRUCT_OPS(elara_running, struct task_struct* p)
{
  u32 cpu = scx_bpf_task_cpu(p);
  struct dispatch_ctx* dispatch_ctx = get_dispatch_ctx(cpu);
  if (!dispatch_ctx)
    return;
  // A task started: the queue of this cpu moves (see cpu_stuck()).
  u64 now = bpf_ktime_get_ns();
  dispatch_ctx->last_progress = now;

  struct task_ctx* context = get_task_ctx(p);
  if (!context)
    return;
  // ops.dequeue took it out of the queue's weight when it left the queue.
  if (context->queued_cpu >= 0)
  {
    note_unqueued(context);
    __sync_fetch_and_add(&nr_unqueued_late, 1);
  }

  // A task taken over from another cpu brings its lag along, its key moves to
  // the vtime of this cpu.
  if (context->vtime_cpu != cpu)
  {
    // taken over by this cpu (see also enqueue())
    context->turns_since = 0;
    context->shared_since = 0;
    u64 slack = context->key - context->vtime;
    rebase_vtime(context, cpu, task_lag(p, context, false));
    context->key = context->vtime + slack;
  }
  if (!context->rescued && (s64)(context->vtime - dispatch_ctx->vtime_ref) > 0)
    dispatch_ctx->vtime_ref = context->vtime;

  dispatch_ctx->running_key = context->key;
  dispatch_ctx->running_deadline = context->deadline;
  dispatch_ctx->running_since = now;
  dispatch_ctx->running_slack = effective_slack(p, context);
  dispatch_ctx->running_cpu_bound = context->cpu_bound;
  dispatch_ctx->running_short = runs_short(context);
  dispatch_ctx->running_brief = !context->cpu_bound && context->run_avg < BRIEF_RUN_NS;
  dispatch_ctx->preempt_pending = false;
  context->immed = false;
  // select_cpu() put it straight into the local DSQ, enqueue() didn't see it.
  context->boost_checked = false;
  context->yield_pending = false;

  // Out of the local DSQ (see note_local()).
  dispatch_ctx->local_weight = 0;
  dispatch_ctx->preempt_key = 0;

  // Tasks the kernel queues by itself (exiting tasks and tasks with migration
  // disabled go straight to the local DSQ) come with its default slice of 20ms.
  if (p->scx.slice > SLICE_NS)
    scx_bpf_task_set_slice(p, SLICE_NS);

  context->started_at = now;
  // A cpu dispatch claimed for it that it didn't go to (the kernel put it back
  // without enqueue(), see SCX_EV_ENQ_SKIP_EXITING) stays idle: kicked so it
  // counts as idle again.
  if (context->upmigrate_cpu && context->upmigrate_cpu - 1 != cpu)
    scx_bpf_kick_cpu(context->upmigrate_cpu - 1, SCX_KICK_IDLE);
  context->upmigrate_cpu = 0;
  context->granted_slice = p->scx.slice;
  context->resume_slice = 0;
  start_slice(cpu, dispatch_ctx, context->started_at, p->scx.slice);

  // Tasks waiting behind a task that only runs for a moment get the cpu back
  // right after it (see pick_balance_cpu()); an idle cpu would take them away
  // from their warm cache.
  if (context->cpu_bound)
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

  tctx->burst += used_ns;
  if (!runnable)
  {
    tctx->run_avg = (3 * tctx->run_avg + tctx->burst) / 4;
    tctx->burst = 0;
    tctx->cpu_bound = false;
    tctx->turns_since = 0;
    tctx->shared_since = 0;
  }
  tctx->stopped_at = now;
  tctx->rescued = false;

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

// @p leaves our custody (see track_queued_weight()): moved from a queue to a
// local DSQ to run, or taken off the runqueue (a change of its properties).
void BPF_STRUCT_OPS(elara_dequeue, struct task_struct* p, u64 deq_flags)
{
  struct task_ctx* tctx = get_task_ctx(p);
  if (tctx)
    note_unqueued(tctx);
}

// Every context switch: when the next task is an RT or deadline task, the cpu
// is taken from us, from one of our tasks or from idle. If RT tasks keep the cpu
// for RT_EVACUATE_DELAY_NS, the tasks already picked for it (the preempted one
// goes back to the front of them) are placed again, and an idle cpu is woken
// for the first task waiting in its queue (rt_timer_fn()).
// (This replaces ops.cpu_release, which newer kernels deprecate.)
SEC("tp_btf/sched_switch")
int BPF_PROG(elara_sched_switch, bool preempt, struct task_struct* prev, struct task_struct* next, unsigned int prev_state)
{
  if (!elara_ready)
    return 0;

  u32 cpu = bpf_get_smp_processor_id();
  struct dispatch_ctx* dctx = get_dispatch_ctx(cpu);

  bool rt = is_rt_task(next);
  // Only when the cpu changes over to RT tasks: a switch from one RT task to
  // another started the timer again, and RT tasks taking turns (irq threads,
  // audio) kept pushing it out.
  if (!dctx || !set_rt_busy(dctx, rt) || !rt)
    return 0;

  // Most RT tasks are done in a moment (see RT_EVACUATE_DELAY_NS): woken right
  // away, an idle cpu came out of idle for every irq thread or audio period
  // to take a task that would have run a few microseconds later. With
  // PREEMPT_RT the timer runs in a low priority RT thread (BPF timers are
  // soft hrtimers), which the RT task holding the cpu may keep from running:
  // there the idle cpu is still woken right away.
  if (CONFIG_PREEMPT_RT)
    kick_idle_for_waiting(cpu);
  // With enq_immed the preempted task already came back through enqueue().
  bool local = !enq_immed && dsq_queued(SCX_DSQ_LOCAL_ON | cpu);
  if (local || (!CONFIG_PREEMPT_RT && dsq_queued(cpu_dsq(cpu))))
  {
    u32 key = (cpu & (MAX_CPUS - 1)) + MAX_CPUS;
    struct elara_timer* st = bpf_map_lookup_elem(&slice_timers, &key);
    if (st)
      bpf_timer_start(&st->timer, RT_EVACUATE_DELAY_NS, BPF_F_TIMER_CPU_PIN);
  }
  return 0;
}

SCX_OPS_DEFINE(elara_ops,
               .init = (void*)elara_init,
               .init_task = (void*)elara_init_task,
               .select_cpu = (void*)elara_select_cpu,
               .quiescent = (void*)elara_quiescent,
               .running = (void*)elara_running,
               .enqueue = (void*)elara_enqueue,
               .dequeue = (void*)elara_dequeue,
               .dispatch = (void*)elara_dispatch,
               .stopping = (void*)elara_stopping,
               .exit = (void*)elara_exit,
               .name = "scx_elara");
