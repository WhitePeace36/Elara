// SPDX-License-Identifier: GPL-2.0
//
// Author: Timon Stipkovits <timon2201@gmail.com>
//
// This software may be used and distributed according to the terms of the
// GNU General Public License version 2.

#ifndef DISPATCHES_H
#define DISPATCHES_H

#include "defines.h"
#include "datatypes.h"
#include "helpers.h"

static __always_inline u64 task_deadline(struct task_struct* p)
{
  struct task_ctx* tctx = get_task_ctx(p);
  return tctx ? tctx->deadline : DEADLINE_NONE;
}

// Deadline of the first task in @dsq, DEADLINE_NONE if it is empty.
static __always_inline u64 dsq_head_deadline(u64 dsq)
{
  struct task_struct* p = dsq_first(dsq);
  return p ? task_deadline(p) : DEADLINE_NONE;
}

// Key of the first task in @dsq, DEADLINE_NONE if it is empty.
static __always_inline u64 dsq_head_key(u64 dsq)
{
  struct task_struct* p = dsq_first(dsq);
  return p ? p->scx.dsq_vtime : DEADLINE_NONE;
}

// ---------------------------------------------------------------------------
// Placement
// ---------------------------------------------------------------------------

// How much work would run on @cpu before a task with @deadline. @wakeup: the
// task is woken up and may preempt.
static __always_inline u64 cpu_load_ahead(u32 cpu, u64 deadline, bool wakeup)
{
  u64 load = 0;

  struct dispatch_ctx* dctx = get_dispatch_ctx(cpu);
  if (dctx && dctx->running_deadline != DEADLINE_NONE)
  {
    // The running task finishes its slice, unless the new one is woken and
    // preempts it. A task that comes back after its slice doesn't preempt:
    // counting the running task as gone would make it move to a cpu where it
    // waits just as long, and leave its own cpu to worse tasks.
    if (!wakeup || dctx->running_deadline <= deadline + PREEMPT_MARGIN_NS)
      load++;
  }
  // A cpu taken by an RT or deadline task is busy for everything. Without this
  // it looks empty (stopping cleared running_deadline) and placement would even
  // prefer it when no cpu is idle.
  else if (cpu_taken_by_rt(cpu))
    load += RT_CPU_LOAD;

  load += dsq_queued(SCX_DSQ_LOCAL_ON | cpu);

  u64 dsq = cpu_dsq(cpu);
  u64 queued = dsq_queued(dsq);
  if (queued && dsq_head_deadline(dsq) <= deadline)
    load += queued;

  return load;
}

// Weight of the tasks on @cpu other than @self: the running one (an RT or
// deadline task counts as RT_CPU_WEIGHT), the ones already picked for it and the
// queued ones.
static __always_inline u64 cpu_weight_load(u32 cpu, struct task_struct* self)
{
  u64 load = 0;
  u32 summed = 0;
  struct task_struct* q;

  struct task_struct* curr = __COMPAT_scx_bpf_cpu_curr(cpu);
  if (curr && curr != self && curr->pid)
    load += is_rt_task(curr) ? RT_CPU_WEIGHT : task_weight(curr);

  load += dsq_queued(SCX_DSQ_LOCAL_ON | cpu) * NICE_0_WEIGHT;

  u64 dsq = cpu_dsq(cpu);
  bpf_for_each(scx_dsq, q, dsq, 0)
  {
    if (q != self)
      load += task_weight(q);
    if (++summed >= WEIGHT_SCAN_DEPTH)
      break;
  }
  u64 queued = dsq_queued(dsq);
  if (queued > summed)
    load += (queued - summed) * NICE_0_WEIGHT;

  return load;
}

// Cpu for a task that comes back after its slice: balanced by weight (see
// RT_CPU_WEIGHT), at most once every BALANCE_INTERVAL_NS, unless its own cpu is
// taken by an RT task.
static __always_inline s32 pick_balance_cpu(struct task_struct* p, struct task_ctx* tctx, u64 slack, u32 cpu, u64 now)
{
  if (p->nr_cpus_allowed == 1)
    return cpu;

  s32 idle = scx_bpf_pick_idle_cpu(p->cpus_ptr, 0);
  if (idle >= 0)
  {
    if ((u32)idle != cpu)
      tctx->last_migrated_at = now;
    return idle;
  }

  bool scan_whole_llc = slack <= SCAN_WHOLE_LLC_SLACK_NS;
  if (!scan_whole_llc && now - tctx->last_migrated_at < BALANCE_INTERVAL_NS && !cpu_taken_by_rt(cpu))
    return cpu;

  u32 key = 0;
  struct pick_scratch* sc = bpf_map_lookup_elem(&pick_scratch_map, &key);
  if (!sc)
    return cpu;

  u64 margin = task_weight(p) / 2;
  sc->best_load = cpu_weight_load(cpu, p);
  sc->best = cpu;
  sc->sampled = 0;
  if (sc->best_load == 0)
    return cpu;

  u32 my_llc = cpu_llc_id(cpu);
  u32 nr_cpu_ids = scx_bpf_nr_cpu_ids();
  u32 start = bpf_get_prandom_u32() % nr_cpu_ids;
  u32 budget = scan_whole_llc ? nr_cpu_ids : BALANCE_SAMPLES;
  u32 i;

  bpf_for(i, 0, nr_cpu_ids)
  {
    u32 other = (start + i) % nr_cpu_ids;
    if (other == cpu || cpu_llc_id(other) != my_llc || !cpu_is_online(other))
      continue;
    if (!bpf_cpumask_test_cpu(other, p->cpus_ptr))
      continue;

    u64 load = cpu_weight_load(other, p);
    if (load + margin < sc->best_load)
    {
      sc->best_load = load;
      sc->best = other;
    }
    sc->sampled++;
    if (sc->sampled >= budget)
      break;
  }

  s32 best = sc->best;
  if ((u32)best != cpu)
    tctx->last_migrated_at = now;
  return best;
}

static __always_inline s32 pick_enqueue_cpu(struct task_struct* p, struct task_ctx* tctx, u64 deadline, u64 slack, u32 cpu, u64 now, bool wakeup)
{
  u64 best_load = cpu_load_ahead(cpu, deadline, wakeup);
  if (best_load == 0)
  {
    // Claim the cpu if it is idle, so no select_cpu() of another wakeup picks
    // it and puts its task into the local DSQ ahead of this one.
    scx_bpf_test_and_clear_cpu_idle(cpu);
    return cpu;
  }

  if (p->nr_cpus_allowed == 1)
    return cpu;

  s32 idle = scx_bpf_pick_idle_cpu(p->cpus_ptr, 0);
  if (idle >= 0)
  {
    if ((u32)idle != cpu)
      tctx->last_migrated_at = now;
    return idle;
  }

  // The rate limit for moving tasks doesn't apply when the own cpu is taken by
  // an RT task: the task would wait until that one is done.
  bool scan_whole_llc = slack <= SCAN_WHOLE_LLC_SLACK_NS;
  if (!scan_whole_llc && now - tctx->last_migrated_at < BALANCE_INTERVAL_NS && !cpu_taken_by_rt(cpu))
    return cpu;

  u32 key = 0;
  struct pick_scratch* sc = bpf_map_lookup_elem(&pick_scratch_map, &key);
  if (!sc)
    return cpu;

  sc->best_load = best_load;
  sc->best = cpu;
  sc->sampled = 0;

  u32 my_llc = cpu_llc_id(cpu);
  u32 nr_cpu_ids = scx_bpf_nr_cpu_ids();
  u32 start = bpf_get_prandom_u32() % nr_cpu_ids;
  u32 budget = scan_whole_llc ? nr_cpu_ids : BALANCE_SAMPLES;
  u32 i;

  bpf_for(i, 0, nr_cpu_ids)
  {
    u32 other = (start + i) % nr_cpu_ids;
    if (other == cpu || cpu_llc_id(other) != my_llc || !cpu_is_online(other))
      continue;
    if (!bpf_cpumask_test_cpu(other, p->cpus_ptr))
      continue;

    u64 load = cpu_load_ahead(other, deadline, wakeup);
    if (load < sc->best_load)
    {
      sc->best_load = load;
      sc->best = other;
      if (load == 0)
        break;
    }
    sc->sampled++;
    if (sc->sampled >= budget)
      break;
  }

  s32 best = sc->best;
  if ((u32)best != cpu)
    tctx->last_migrated_at = now;
  return best;
}

// ---------------------------------------------------------------------------
// Dispatch
// ---------------------------------------------------------------------------

// A task of another cpu's queue that @cpu may take: allowed to run on it and not
// pinned to its cpu for the moment (migrate_disable()).
static __always_inline bool can_take(struct task_struct* p, u32 cpu)
{
  return !p->migration_disabled && bpf_cpumask_test_cpu(cpu, p->cpus_ptr);
}

// Deadline of the first task in @dsq that @cpu may take (the queue is ordered by
// deadline, so the earliest of them), DEADLINE_NONE if there is none among the
// first STEAL_SCAN_DEPTH tasks.
static __always_inline u64 dsq_first_takeable(u64 dsq, u32 cpu)
{
  struct task_struct* p = dsq_first(dsq);
  u32 looked = 0;

  if (!p)
    return DEADLINE_NONE;
  if (can_take(p, cpu))
    return task_deadline(p);

  bpf_for_each(scx_dsq, p, dsq, 0)
  {
    if (can_take(p, cpu))
      return task_deadline(p);
    if (++looked >= STEAL_SCAN_DEPTH)
      break;
  }
  return DEADLINE_NONE;
}

// Move the first task in @dsq that @cpu may take to the local DSQ of @cpu.
static __always_inline bool take_first_takeable(u64 dsq, u32 cpu)
{
  struct task_struct* p;
  u32 looked = 0;

  bpf_for_each(scx_dsq, p, dsq, 0)
  {
    if (can_take(p, cpu))
      return scx_bpf_dsq_move(BPF_FOR_EACH_ITER, p, SCX_DSQ_LOCAL, 0);
    if (++looked >= STEAL_SCAN_DEPTH)
      break;
  }
  return false;
}

#define SCAN_RT_CPUS 0            // cpus of the own llc taken by an RT task
#define SCAN_OWN_LLC 1            // all other cpus of the own llc
#define SCAN_OTHER_LLCS 2         // cpus of other llcs
#define SCAN_RT_CPUS_OTHER_LLCS 3 // cpus of other llcs taken by an RT task

// The cpu (of the cpus selected by @mode) whose queue has the task with the
// earliest deadline that @cpu may take, -1 if there is none. Its deadline is
// stored in @deadline. The scan starts at a random cpu, so of several tasks with
// the same deadline a random one is taken.
static __always_inline s32 find_earliest(u32 cpu, u32 mode, u64* deadline)
{
  u32 key = 0;
  struct steal_scratch* sc = bpf_map_lookup_elem(&steal_scratch_map, &key);
  if (!sc)
    return -1;

  sc->best_deadline = DEADLINE_NONE;
  sc->best_cpu = -1;

  u32 my_llc = cpu_llc_id(cpu);
  u32 nr_cpu_ids = scx_bpf_nr_cpu_ids();
  u32 start = bpf_get_prandom_u32() % nr_cpu_ids;
  u32 i;

  bpf_for(i, 0, nr_cpu_ids)
  {
    u32 other = (start + i) % nr_cpu_ids;
    if (other == cpu || !cpu_is_online(other))
      continue;

    bool same_llc = cpu_llc_id(other) == my_llc;
    bool other_llcs = mode == SCAN_OTHER_LLCS || mode == SCAN_RT_CPUS_OTHER_LLCS;
    if (other_llcs ? same_llc : !same_llc)
      continue;

    if (mode == SCAN_RT_CPUS || mode == SCAN_RT_CPUS_OTHER_LLCS)
    {
      struct dispatch_ctx* other_ctx = get_dispatch_ctx(other);
      if (!other_ctx || !other_ctx->rt_busy || !cpu_taken_by_rt(other))
        continue;
    }

    u64 dsq = cpu_dsq(other);
    if (!dsq_queued(dsq))
      continue;

    u64 d = dsq_first_takeable(dsq, cpu);
    if (d < sc->best_deadline)
    {
      sc->best_deadline = d;
      sc->best_cpu = other;
    }
  }

  *deadline = sc->best_deadline;
  return sc->best_cpu;
}

// Run a task of the own queue that is more than STARVE_GRACE_NS past its
// deadline (see there), checked at most every STARVE_CHECK_INTERVAL_NS.
static __always_inline bool take_overdue(struct dispatch_ctx* dctx, u64 dsq, u64 now)
{
  if (!dctx || now - dctx->last_starve_check < STARVE_CHECK_INTERVAL_NS)
    return false;
  dctx->last_starve_check = now;

  struct task_struct* p;
  u32 looked = 0;

  // From the end of the queue: the tasks that wait longest are the ones with the
  // latest keys (low weight, large lag). Scanning from the front, they are out
  // of reach as soon as more than STARVE_SCAN_DEPTH tasks are queued.
  bpf_for_each(scx_dsq, p, dsq, SCX_DSQ_ITER_REV)
  {
    u64 deadline = task_deadline(p);
    if (deadline != DEADLINE_NONE && deadline + STARVE_GRACE_NS < now)
      return scx_bpf_dsq_move(BPF_FOR_EACH_ITER, p, SCX_DSQ_LOCAL, 0);
    if (++looked >= STARVE_SCAN_DEPTH)
      break;
  }
  return false;
}

// Pick the next task for @cpu. @prev_key and @prev_deadline are what prev would
// run with for another slice (DEADLINE_NONE: no prev that could continue).
// Returns true when prev keeps running.
//
// 1. A task waiting on a cpu of the llc that is taken by an RT task, if its
//    deadline is earlier than the own next task (or there is none). That cpu
//    can't run it until the RT task is done, so it loses nothing by moving.
//    Then the same for cpus of other llcs: a task that may only run on its RT
//    cpu and cpus of another llc would otherwise wait until the RT task is done
//    (and get the scheduler stopped by the watchdog on kernels without the
//    sched_ext deadline server).
// 2. A task of the own queue far past its deadline (take_overdue()).
// 3. The own next task (prev or the head of the own queue, by key).
// 4. The task with the earliest deadline on the other cpus of the llc.
// 5. The task with the earliest deadline on cpus of other llcs.
//
// Busy cpus don't take tasks from each other, so tasks stay on their cpu (and
// its caches). Tasks that keep running are balanced when they are queued again
// after their slice (pick_enqueue_cpu()).
static __always_inline bool dispatch_dsq_per_cpu(u32 cpu, u64 prev_key, u64 prev_deadline)
{
  u64 own_dsq = cpu_dsq(cpu);
  u64 own_key = dsq_head_key(own_dsq);
  u64 deadline;
  s32 other;

  if (nr_rt_busy)
  {
    u64 own_deadline = dsq_head_deadline(own_dsq);
    u64 local_best = own_deadline < prev_deadline ? own_deadline : prev_deadline;
    other = find_earliest(cpu, SCAN_RT_CPUS, &deadline);
    if (other >= 0 && deadline < local_best && take_first_takeable(cpu_dsq((u32)other), cpu))
      return false;
    if (nr_llcs > 1)
    {
      other = find_earliest(cpu, SCAN_RT_CPUS_OTHER_LLCS, &deadline);
      if (other >= 0 && deadline < local_best && take_first_takeable(cpu_dsq((u32)other), cpu))
        return false;
    }
  }

  if (take_overdue(get_dispatch_ctx(cpu), own_dsq, bpf_ktime_get_ns()))
    return false;

  if (prev_key != DEADLINE_NONE && prev_key <= own_key)
    return true;
  if (own_key != DEADLINE_NONE && scx_bpf_dsq_move_to_local(own_dsq, 0))
    return false;
  if (prev_key != DEADLINE_NONE)
    return true;

  other = find_earliest(cpu, SCAN_OWN_LLC, &deadline);
  if (other >= 0 && take_first_takeable(cpu_dsq((u32)other), cpu))
    return false;

  if (nr_llcs > 1)
  {
    other = find_earliest(cpu, SCAN_OTHER_LLCS, &deadline);
    if (other >= 0 && take_first_takeable(cpu_dsq((u32)other), cpu))
      return false;
  }

  return false;
}

// Wake an idle cpu for the first task waiting in the queue of @cpu, if it may run
// elsewhere (not pinned to the cpu, also not for the moment by migrate_disable()).
static __always_inline void kick_idle_for_waiting(u32 cpu)
{
  struct task_struct* p = dsq_first(cpu_dsq(cpu));

  if (p && p->nr_cpus_allowed > 1 && !p->migration_disabled)
  {
    s32 idle = scx_bpf_pick_idle_cpu(p->cpus_ptr, 0);
    if (idle >= 0)
      scx_bpf_kick_cpu(idle, SCX_KICK_IDLE);
  }
}

#endif  // DISPATCHES_H
