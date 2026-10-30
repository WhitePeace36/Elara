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

// @cpu has tasks queued, but its queue didn't move for STUCK_NS (see there).
static __always_inline bool cpu_stuck(u32 cpu, u64 now)
{
  struct dispatch_ctx* dctx = get_dispatch_ctx(cpu);
  return dctx && dsq_queued(cpu_dsq(cpu)) && elapsed(now, dctx->last_progress) > STUCK_NS;
}

// Called before a task is put into the queue of @cpu: if it is the first one,
// the time without progress (see cpu_stuck()) counts from now. Otherwise a cpu
// that was idle for long would count as stuck as soon as a task is queued for
// it, before it even woke up.
static __always_inline void queue_on(u32 cpu, u64 now)
{
  struct dispatch_ctx* dctx = get_dispatch_ctx(cpu);
  if (dctx && !dsq_queued(cpu_dsq(cpu)))
    dctx->last_progress = now;
}

// Claim an idle cpu @p may run on among the fast cpus (@fast) or the slower ones,
// of a core whose other cpus are idle too if @whole_core. -1 if there is none.
static __always_inline s32 claim_idle_of(const struct task_struct* p, bool fast, bool whole_core)
{
  u32 nr = fast ? nr_big_cpus : nr_little_cpus;
  const struct cpumask* idle = whole_core ? scx_bpf_get_idle_smtmask() : scx_bpf_get_idle_cpumask();
  s32 found = -1;
  u32 i;

  bpf_for(i, 0, nr)
  {
    u32 c = fast ? big_cpus[i & (MAX_CPUS - 1)] : little_cpus[i & (MAX_CPUS - 1)];
    if (bpf_cpumask_test_cpu(c, idle) && bpf_cpumask_test_cpu(c, p->cpus_ptr) && scx_bpf_test_and_clear_cpu_idle(c))
    {
      found = c;
      break;
    }
  }
  scx_bpf_put_idle_cpumask(idle);
  return found;
}

// Claim an idle cpu @p may run on in @llc, of a core whose other cpus are idle
// too if @whole_core. -1 if there is none.
static __always_inline s32 claim_idle_in_llc(const struct task_struct* p, u32 llc, bool whole_core)
{
  llc &= (MAX_CPUS - 1);
  u32 start = llc_start[llc];
  u32 nr = llc_len[llc];
  const struct cpumask* idle = whole_core ? scx_bpf_get_idle_smtmask() : scx_bpf_get_idle_cpumask();
  s32 found = -1;
  u32 i;

  bpf_for(i, 0, nr)
  {
    u32 c = llc_cpus[(start + i) & (MAX_CPUS - 1)];
    if (bpf_cpumask_test_cpu(c, idle) && bpf_cpumask_test_cpu(c, p->cpus_ptr) && scx_bpf_test_and_clear_cpu_idle(c))
    {
      found = c;
      break;
    }
  }
  scx_bpf_put_idle_cpumask(idle);
  return found;
}

// Claim an idle cpu of the kind @p prefers (see wants_fast_core()), a whole idle
// core first. -1 if there is none, or no fast and slower cores.
static __always_inline s32 claim_idle_preferred(const struct task_struct* p, const struct task_ctx* tctx)
{
  if (!hybrid)
    return -1;
  bool fast = wants_fast_core(p, tctx);
  if (!fast && !wants_slow_core(p, tctx))
    return -1;
  s32 cpu = smt ? claim_idle_of(p, fast, true) : -1;
  return cpu >= 0 ? cpu : claim_idle_of(p, fast, false);
}

// @p runs only for a moment each time it wakes up (see SHORT_RUN_NS).
static __always_inline bool runs_short(const struct task_ctx* tctx)
{
  return tctx && !tctx->cpu_bound && tctx->run_avg < SHORT_RUN_NS;
}

// Claim an idle cpu of the llc of @cpu for @p, whose previous cpu is @prev, -1
// if there is none: @prev if it is of that llc (a task that runs only for a
// moment gains more from its warm caches than from a whole core), a whole idle
// core, @cpu, its sibling, any.
static __always_inline s32 claim_idle_near(const struct task_struct* p, u32 cpu, u32 prev)
{
  u32 llc = cpu_llc_id(cpu);
  if (cpu_llc_id(prev) == llc && bpf_cpumask_test_cpu(prev, p->cpus_ptr) && scx_bpf_test_and_clear_cpu_idle(prev))
    return prev;
  bool allowed = bpf_cpumask_test_cpu(cpu, p->cpus_ptr);
  s32 c = claim_idle_in_llc(p, llc, true);
  if (c >= 0)
    return c;
  if (allowed && scx_bpf_test_and_clear_cpu_idle(cpu))
    return cpu;
  s32 sibling = cpu_sibling[cpu & (MAX_CPUS - 1)];
  if (sibling >= 0 && bpf_cpumask_test_cpu(sibling, p->cpus_ptr) && scx_bpf_test_and_clear_cpu_idle(sibling))
    return sibling;
  return claim_idle_in_llc(p, llc, false);
}

// A cpu of @llc that @p may run on runs a task that only runs for a moment, and
// nothing waits there: it is free again in a moment.
static __always_inline bool llc_frees_soon(const struct task_struct* p, u32 llc)
{
  llc &= (MAX_CPUS - 1);
  u32 start = llc_start[llc];
  u32 nr = llc_len[llc];
  u32 i;

  bpf_for(i, 0, nr)
  {
    u32 c = llc_cpus[(start + i) & (MAX_CPUS - 1)];
    struct dispatch_ctx* dctx = get_dispatch_ctx(c);
    if (dctx && dctx->running_deadline != DEADLINE_NONE && dctx->running_short && bpf_cpumask_test_cpu(c, p->cpus_ptr) &&
        !dsq_queued(cpu_dsq(c)) && !dsq_queued(SCX_DSQ_LOCAL_ON | c))
      return true;
  }
  return false;
}

// Claim an idle cpu for @p, whose cpu is @cpu, -1 if there is none. With fast
// and slower cores those of the kind @p prefers first. With SMT a cpu of a whole
// idle core (a cpu whose sibling is busy gets only part of the core), of the llc
// of @cpu first, then any; then any idle cpu, of the llc of @cpu first: in
// another llc the task starts with a cold cache. The order of the kernel's
// default (scx_select_cpu_dfl()). Except for a task that runs only for a moment
// with several llcs: any idle cpu of its own llc first, and none of another llc
// while a cpu of its own is free again in a moment (see SHORT_RUN_NS).
static __always_inline s32 pick_idle(const struct task_struct* p, const struct task_ctx* tctx, u32 cpu)
{
  s32 c = claim_idle_preferred(p, tctx);
  if (c >= 0)
    return c;
  if (nr_llcs > 1 && runs_short(tctx))
  {
    c = claim_idle_near(p, cpu, cpu);
    if (c >= 0 || llc_frees_soon(p, cpu_llc_id(cpu)))
      return c;
  }
  if (smt)
  {
    c = claim_idle_in_llc(p, cpu_llc_id(cpu), true);
    if (c >= 0)
      return c;
    c = scx_bpf_pick_idle_cpu(p->cpus_ptr, SCX_PICK_IDLE_CORE);
    if (c >= 0)
      return c;
  }
  if (nr_llcs > 1)
  {
    c = claim_idle_in_llc(p, cpu_llc_id(cpu), false);
    if (c >= 0)
      return c;
  }
  return scx_bpf_pick_idle_cpu(p->cpus_ptr, 0);
}

// Nothing is ahead of @p on its own @cpu, but an idle cpu elsewhere is better:
// with SMT a whole idle core of the same llc when the sibling of @cpu is busy
// (another llc costs the cache, more than sharing the core), with fast and
// slower cores a fast one when @p wants one and @cpu is slower. That cpu
// (claimed), -1 if there is none.
static __always_inline s32 better_idle(const struct task_struct* p, const struct task_ctx* tctx, u32 cpu)
{
  if (p->nr_cpus_allowed == 1)
    return -1;
  if (hybrid && !cpu_is_fast(cpu) && wants_fast_core(p, tctx))
  {
    s32 c = claim_idle_preferred(p, tctx);
    if (c >= 0)
      return c;
  }
  if (smt)
  {
    const struct cpumask* idle_cores = scx_bpf_get_idle_smtmask();
    bool core_idle = bpf_cpumask_test_cpu(cpu, idle_cores);
    scx_bpf_put_idle_cpumask(idle_cores);
    if (!core_idle)
      return claim_idle_in_llc(p, cpu_llc_id(cpu), true);
  }
  return -1;
}

// @p runs alone on @cpu (nothing queued there) at the end of its slice: an idle
// cpu that is clearly better for it, claimed, or -1. With fast and slower cores
// one of the kind it prefers when @cpu is the other kind; with SMT a cpu of a
// whole idle core (of the same kind, without those: of the same llc) when it
// has shared its core with a cpu bound task for SMT_SHARE_NS, of another llc
// after SMT_CROSS_LLC_NS.
// At most once per BALANCE_INTERVAL_NS per task.
static __always_inline s32 better_cpu_alone(const struct task_struct* p, struct task_ctx* tctx, u32 cpu, u64 now)
{
  if (p->nr_cpus_allowed == 1 || is_migration_disabled(p))
    return -1;

  // How long @p has shared its core: only a cpu bound task (or an RT task) on
  // the sibling takes half the core for long, something that runs there for a
  // moment is no reason to move. Followed also while @p may not move. A gap of
  // more than SMT_SHARE_GAP_NS (@p didn't run alone at the end of a slice for
  // that long) starts over.
  u64 shared = 0;
  if (smt)
  {
    s32 sibling = cpu_sibling[cpu & (MAX_CPUS - 1)];
    bool contended = false;
    if (sibling >= 0)
    {
      const struct cpumask* idle = scx_bpf_get_idle_cpumask();
      bool sibling_busy = !bpf_cpumask_test_cpu(sibling, idle);
      scx_bpf_put_idle_cpumask(idle);
      struct dispatch_ctx* sdctx = get_dispatch_ctx((u32)sibling);
      contended = sibling_busy && (cpu_taken_by_rt((u32)sibling) || (sdctx && sdctx->running_deadline != DEADLINE_NONE && sdctx->running_cpu_bound));
    }
    if (!contended)
      tctx->shared_since = 0;
    else
    {
      if (!tctx->shared_since || now - tctx->shared_seen > SMT_SHARE_GAP_NS)
        tctx->shared_since = now;
      tctx->shared_seen = now;
      shared = now - tctx->shared_since;
    }
  }

  if (now - tctx->last_migrated_at < BALANCE_INTERVAL_NS)
    return -1;

  bool on_fast = cpu_is_fast(cpu);
  bool want_fast = on_fast;
  if (hybrid)
  {
    bool fast = wants_fast_core(p, tctx);
    if ((fast && !on_fast) || (wants_slow_core(p, tctx) && on_fast))
    {
      s32 c = claim_idle_preferred(p, tctx);
      if (c >= 0)
        return c;
    }
    if (fast || wants_slow_core(p, tctx))
      want_fast = fast;
  }

  if (smt && shared >= SMT_SHARE_NS)
  {
    if (hybrid)
      return claim_idle_of(p, want_fast, true);
    s32 c = claim_idle_in_llc(p, cpu_llc_id(cpu), true);
    if (c < 0 && nr_llcs > 1 && shared >= SMT_CROSS_LLC_NS)
      c = scx_bpf_pick_idle_cpu(p->cpus_ptr, SCX_PICK_IDLE_CORE);
    return c;
  }
  return -1;
}

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
  // prefer it when no cpu is idle. So is a stuck one.
  else if (cpu_taken_by_rt(cpu) || cpu_stuck(cpu, bpf_ktime_get_ns()))
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
  if (curr && curr->pid)
  {
    if (curr != self)
      load += is_rt_task(curr) ? RT_CPU_WEIGHT : task_weight(curr);
  }
  else if (cpu_stuck(cpu, bpf_ktime_get_ns()))
    load += RT_CPU_WEIGHT;

  // The tasks already picked for the cpu count as nice 0, except the one the
  // own dispatch just picked (see note_local()).
  u64 nlocal = dsq_queued(SCX_DSQ_LOCAL_ON | cpu);
  if (nlocal)
  {
    struct dispatch_ctx* dctx = get_dispatch_ctx(cpu);
    u64 lw = dctx && cpu == bpf_get_smp_processor_id() ? dctx->local_weight : 0;
    load += lw ? lw + (nlocal - 1) * NICE_0_WEIGHT : nlocal * NICE_0_WEIGHT;
  }

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

// Weight load of @cpu (cpu_weight_load()) as if on a fast core: a slower core
// gets through the same work in more time, so it counts heavier.
static __always_inline u64 capacity_load(u64 load, u32 cpu)
{
  return hybrid ? load * 1024 / cpu_cap(cpu) : load;
}

// With several llcs: a cpu of another llc (one at random) for @p, which keeps
// running on @cpu, if that llc has clearly less weight per cpu than the own one
// (moving @p evens them out, not just turns them around): the cpu with the
// least weight there, as of the last llc_scan(). -1 if there is none.
static __always_inline s32 pick_other_llc(const struct task_struct* p, u32 cpu)
{
  u32 mine = cpu_llc_id(cpu) & (MAX_CPUS - 1);
  u32 other = bpf_get_prandom_u32() % nr_llcs;
  if (other == mine)
    other = (other + 1) % nr_llcs;
  other &= MAX_CPUS - 1;

  u64 own_sum = llc_load[mine], other_sum = llc_load[other];
  u64 own_n = llc_online[mine], other_n = llc_online[other];
  u64 w = task_weight(p);
  s32 best = llc_light_cpu[other];
  if (!own_n || !other_n || best < 0 || !bpf_cpumask_test_cpu(best, p->cpus_ptr))
    return -1;
  // Per cpu, the own llc is heavier by more than half of @p's weight spread
  // over both: after the move the difference is smaller than before.
  if (own_sum * other_n <= other_sum * own_n + w * (own_n + other_n) / 2)
    return -1;
  // Until the next scan the move counts already, so the next task doesn't
  // follow on the same numbers.
  __sync_fetch_and_add(&llc_load[mine], -w);
  __sync_fetch_and_add(&llc_load[other], w);
  return best;
}

// Cpu for a task that comes back after its slice: balanced by weight (see
// RT_CPU_WEIGHT), at most once every BALANCE_INTERVAL_NS, unless its own cpu is
// taken by an RT task.
static __always_inline s32 pick_balance_cpu(struct task_struct* p, struct task_ctx* tctx, u64 slack, u32 cpu, u64 now)
{
  if (p->nr_cpus_allowed == 1)
    return cpu;

  // The task that runs next on the cpu (the one in its local DSQ, otherwise
  // the head of its queue) only runs for a moment (its last run ended in sleep:
  // a woken task, a kworker): this one gets the cpu back right after it, in its
  // warm cache. Moving to an idle cpu instead moved cpu bound tasks dozens of
  // times a second, with SMT often next to another one.
  bool next_bound;
  if (dsq_queued(SCX_DSQ_LOCAL_ON | cpu))
  {
    struct dispatch_ctx* own = get_dispatch_ctx(cpu);
    next_bound = !own || own->local_cpu_bound;
  }
  else
  {
    struct task_struct* head = dsq_first(cpu_dsq(cpu));
    struct task_ctx* hctx = head ? get_task_ctx(head) : NULL;
    next_bound = head && (!hctx || hctx->cpu_bound);
  }
  if (!next_bound && !cpu_taken_by_rt(cpu))
    return cpu;
  if (!tctx->turns_since)
    tctx->turns_since = now;

  s32 idle = pick_idle(p, tctx, cpu);
  if (idle >= 0)
  {
    if ((u32)idle != cpu)
      tctx->last_migrated_at = now;
    return idle;
  }

  // Weight balancing (below) stays in the llc. Tasks that keep running move
  // to another llc when that one has clearly less work (see LLC_BALANCE_NS).
  if (nr_llcs > 1 && tctx->cpu_bound && now - tctx->last_migrated_at >= LLC_BALANCE_NS)
  {
    s32 far = pick_other_llc(p, cpu);
    if (far >= 0)
    {
      tctx->last_migrated_at = now;
      return far;
    }
  }

  bool scan_whole_llc = slack <= SCAN_WHOLE_LLC_SLACK_NS;
  if (!scan_whole_llc && now - tctx->last_migrated_at < BALANCE_INTERVAL_NS && !cpu_taken_by_rt(cpu))
    return cpu;

  u32 key = 0;
  struct pick_scratch* sc = bpf_map_lookup_elem(&pick_scratch_map, &key);
  if (!sc)
    return cpu;

  bool rotate = now - tctx->turns_since >= ROTATE_NS;
  u64 margin = task_weight(p) / 2;
  sc->best_load = capacity_load(cpu_weight_load(cpu, p), cpu);
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

    u64 load = capacity_load(cpu_weight_load(other, p), other);
    if (load + margin < sc->best_load || (rotate && (u32)sc->best == cpu && load <= sc->best_load + margin))
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
    s32 better = better_idle(p, tctx, cpu);
    if (better >= 0)
    {
      tctx->last_migrated_at = now;
      return better;
    }
    // Claim the cpu if it is idle, so no select_cpu() of another wakeup picks
    // it and puts its task into the local DSQ ahead of this one.
    scx_bpf_test_and_clear_cpu_idle(cpu);
    return cpu;
  }

  if (p->nr_cpus_allowed == 1)
    return cpu;

  s32 idle = pick_idle(p, tctx, cpu);
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

// @p ran until a moment ago (STEAL_HOT_NS): it gets its cpu back soon, with its
// cache still warm there.
static __always_inline bool cache_hot(struct task_struct* p)
{
  struct task_ctx* tctx = get_task_ctx(p);
  return tctx && elapsed(bpf_ktime_get_ns(), tctx->stopped_at) < STEAL_HOT_NS;
}

// A task of another cpu's queue that @cpu may take: allowed to run on it and not
// pinned to its cpu for the moment (migrate_disable()). Unless @hot_ok, not one
// that ran until a moment ago (see may_take_hot()), and from another llc (@far)
// not one that runs only for a moment (see SHORT_RUN_NS) while it is the only
// one waiting there: its own llc runs it soon.
static __always_inline bool can_take(struct task_struct* p, u32 cpu, bool hot_ok, bool far)
{
  if (is_migration_disabled(p) || !bpf_cpumask_test_cpu(cpu, p->cpus_ptr))
    return false;
  if (hot_ok)
    return true;
  return !cache_hot(p) && !(far && runs_short(get_task_ctx(p)));
}

// A task that ran until a moment ago may still be taken from @other when it
// would wait long there: behind a cpu bound task (a whole slice), an RT task or
// on a stuck cpu (those are rescues). Behind a task that only runs for a moment
// it gets its cpu back right away. Not counting the cpu bound case, an idle cpu
// left a short task (a forked process) waiting behind a cpu bound one, which then
// gave its cpu to it and moved itself.
static __always_inline bool may_take_hot(u32 other)
{
  struct dispatch_ctx* d = get_dispatch_ctx(other);
  return d && d->running_deadline != DEADLINE_NONE && d->running_cpu_bound;
}

// Deadline of the first task in @dsq that @cpu may take, DEADLINE_NONE if there
// is none among the first @depth tasks.
static __always_inline u64 dsq_first_takeable(u64 dsq, u32 cpu, u32 depth, bool hot_ok, bool far)
{
  far = far && dsq_queued(dsq) <= 1;
  struct task_struct* p = dsq_first(dsq);
  u32 looked = 0;

  if (!p)
    return DEADLINE_NONE;
  if (can_take(p, cpu, hot_ok, far))
    return task_deadline(p);

  bpf_for_each(scx_dsq, p, dsq, 0)
  {
    if (can_take(p, cpu, hot_ok, far))
      return task_deadline(p);
    if (++looked >= depth)
      break;
  }
  return DEADLINE_NONE;
}

// Move the first task in @dsq that @cpu may take (among the first @depth) to the
// local DSQ of @cpu.
static __always_inline bool take_first_takeable(u64 dsq, u32 cpu, u32 depth, bool hot_ok, bool far)
{
  far = far && dsq_queued(dsq) <= 1;
  struct task_struct* p;
  u32 looked = 0;

  bpf_for_each(scx_dsq, p, dsq, 0)
  {
    if (can_take(p, cpu, hot_ok, far))
    {
      if (!scx_bpf_dsq_move(BPF_FOR_EACH_ITER, p, SCX_DSQ_LOCAL, 0))
        return false;
      note_local(cpu, p);
      return true;
    }
    if (++looked >= depth)
      break;
  }
  return false;
}

#define SCAN_RT_CPUS 0            // cpus of the own llc taken by an RT task
#define SCAN_OWN_LLC 1            // all other cpus of the own llc
#define SCAN_OTHER_LLCS 2         // cpus of other llcs
#define SCAN_RT_CPUS_OTHER_LLCS 3 // cpus of other llcs taken by an RT task
#define SCAN_STUCK_CPUS 4         // stuck cpus of all llcs (see STUCK_NS)

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
  u64 now = mode == SCAN_STUCK_CPUS ? bpf_ktime_get_ns() : 0;
  u32 i;

  bpf_for(i, 0, nr_cpu_ids)
  {
    u32 other = (start + i) % nr_cpu_ids;
    if (other == cpu || !cpu_is_online(other))
      continue;

    if (mode == SCAN_STUCK_CPUS)
    {
      if (!stuck_cpu[other & (MAX_CPUS - 1)] || !cpu_stuck(other, now))
        continue;
    }
    else
    {
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
    }

    u64 dsq = cpu_dsq(other);
    if (!dsq_queued(dsq))
      continue;

    bool rescue = mode == SCAN_STUCK_CPUS || mode == SCAN_RT_CPUS || mode == SCAN_RT_CPUS_OTHER_LLCS;
    u64 d = dsq_first_takeable(dsq, cpu, rescue ? RESCUE_SCAN_DEPTH : STEAL_SCAN_DEPTH, rescue || may_take_hot(other), mode == SCAN_OTHER_LLCS);
    if (d < sc->best_deadline)
    {
      sc->best_deadline = d;
      sc->best_cpu = other;
    }
  }

  *deadline = sc->best_deadline;
  return sc->best_cpu;
}

// Rough weight of the tasks on @cpu other than @self: the running one and the
// queued ones as nice 0 (cpu_weight_load() sums them up exactly, too costly for
// all cpus of two llcs). A cpu taken by an RT task or stuck counts as
// RT_CPU_WEIGHT.
static __always_inline u64 cpu_rough_load(u32 cpu, struct task_struct* self)
{
  u64 load = 0;
  struct task_struct* curr = __COMPAT_scx_bpf_cpu_curr(cpu);
  if (curr && curr->pid)
  {
    if (curr != self)
      load += is_rt_task(curr) ? RT_CPU_WEIGHT : task_weight(curr);
  }
  else if (cpu_stuck(cpu, bpf_ktime_get_ns()))
    load += RT_CPU_WEIGHT;
  u64 queued = dsq_queued(cpu_dsq(cpu)) + dsq_queued(SCX_DSQ_LOCAL_ON | cpu);
  return load + queued * NICE_0_WEIGHT;
}

// With several llcs, add up the rough weight per llc (see cpu_rough_load()) for
// pick_other_llc(), with the scan for stuck cpus: once per interval for the
// whole system.
static __always_inline void llc_scan(void)
{
  u32 nr_cpu_ids = scx_bpf_nr_cpu_ids();
  u32 i;
  bpf_for(i, 0, nr_llcs)
  {
    u32 l = i & (MAX_CPUS - 1);
    llc_load_acc[l] = 0;
    llc_online_acc[l] = 0;
    llc_light_acc[l] = -1;
    llc_light_load_acc[l] = (u64)-1;
  }
  bpf_for(i, 0, nr_cpu_ids)
  {
    u32 c = i & (MAX_CPUS - 1);
    if (!cpu_is_online(c))
      continue;
    u32 l = cpu_llc_id(c) & (MAX_CPUS - 1);
    u64 load = capacity_load(cpu_rough_load(c, NULL), c);
    llc_load_acc[l] += load;
    llc_online_acc[l]++;
    if (load < llc_light_load_acc[l])
    {
      llc_light_load_acc[l] = load;
      llc_light_acc[l] = c;
    }
  }
  bpf_for(i, 0, nr_llcs)
  {
    u32 l = i & (MAX_CPUS - 1);
    llc_load[l] = llc_load_acc[l];
    llc_online[l] = llc_online_acc[l];
    llc_light_cpu[l] = llc_light_acc[l];
  }
}

// Every STUCK_CHECK_INTERVAL_NS one cpu (the first to get here) looks for stuck
// cpus (see STUCK_NS) and marks them in stuck_cpu, nr_stuck_cpus tells the others
// whether to look there: one scan of all cpus per interval for the whole system,
// not one per cpu.
static __always_inline void scan_stuck(u64 now)
{
  u64 last = stuck_scan_at;
  if (elapsed(now, last) < STUCK_CHECK_INTERVAL_NS)
    return;
  if (__sync_val_compare_and_swap(&stuck_scan_at, last, now) != last)
    return;

  // Counted in the global, not a local: the verifier would track a local
  // counter's exact value and go through the loop once per value.
  u32 nr_cpu_ids = scx_bpf_nr_cpu_ids();
  u32 i;
  nr_stuck_cpus = 0;
  bpf_for(i, 0, nr_cpu_ids)
  {
    u32 c = i & (MAX_CPUS - 1);
    bool stuck = cpu_is_online(c) && cpu_stuck(c, now);
    stuck_cpu[c] = stuck;
    if (stuck)
      nr_stuck_cpus++;
  }
  if (nr_llcs > 1)
    llc_scan();
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
    {
      if (!scx_bpf_dsq_move(BPF_FOR_EACH_ITER, p, SCX_DSQ_LOCAL, 0))
        return false;
      note_local(bpf_get_smp_processor_id(), p);
      return true;
    }
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
//
// Returns DSP_TOOK when a task was put into the local DSQ, DSP_PREV when prev
// keeps running, DSP_NONE when there is nothing to run.
#define DSP_TOOK 0
#define DSP_PREV 1
#define DSP_NONE 2
static __always_inline u32 dispatch_dsq_per_cpu(u32 cpu, u64 prev_key, u64 prev_deadline)
{
  u64 own_dsq = cpu_dsq(cpu);
  u64 own_key = dsq_head_key(own_dsq);
  u64 deadline;
  s32 other;

  u64 own_deadline = dsq_head_deadline(own_dsq);
  u64 local_best = own_deadline < prev_deadline ? own_deadline : prev_deadline;

  struct dispatch_ctx* dctx = get_dispatch_ctx(cpu);
  u64 now = bpf_ktime_get_ns();
  scan_stuck(now);
  if (nr_stuck_cpus)
  {
    other = find_earliest(cpu, SCAN_STUCK_CPUS, &deadline);
    if (other >= 0 && deadline < local_best && take_first_takeable(cpu_dsq((u32)other), cpu, RESCUE_SCAN_DEPTH, true, false))
      return DSP_TOOK;
  }

  if (nr_rt_busy)
  {
    other = find_earliest(cpu, SCAN_RT_CPUS, &deadline);
    if (other >= 0 && deadline < local_best && take_first_takeable(cpu_dsq((u32)other), cpu, RESCUE_SCAN_DEPTH, true, false))
      return DSP_TOOK;
    if (nr_llcs > 1)
    {
      other = find_earliest(cpu, SCAN_RT_CPUS_OTHER_LLCS, &deadline);
      if (other >= 0 && deadline < local_best && take_first_takeable(cpu_dsq((u32)other), cpu, RESCUE_SCAN_DEPTH, true, false))
        return DSP_TOOK;
    }
  }

  if (take_overdue(dctx, own_dsq, now))
    return DSP_TOOK;

  if (prev_key != DEADLINE_NONE && prev_key <= own_key)
    return DSP_PREV;
  if (own_key != DEADLINE_NONE)
  {
    struct task_struct* head = dsq_first(own_dsq);
    u64 w = head ? task_weight(head) : NICE_0_WEIGHT;
    struct task_ctx* hctx = head ? get_task_ctx(head) : NULL;
    bool head_cpu_bound = !hctx || hctx->cpu_bound;
    if (scx_bpf_dsq_move_to_local(own_dsq, 0))
    {
      if (dctx)
      {
        dctx->local_weight = w;
        dctx->local_cpu_bound = head_cpu_bound;
      }
      return DSP_TOOK;
    }
  }
  if (prev_key != DEADLINE_NONE)
    return DSP_PREV;

  other = find_earliest(cpu, SCAN_OWN_LLC, &deadline);
  if (other >= 0 && take_first_takeable(cpu_dsq((u32)other), cpu, STEAL_SCAN_DEPTH, may_take_hot((u32)other), false))
    return DSP_TOOK;

  if (nr_llcs > 1)
  {
    other = find_earliest(cpu, SCAN_OTHER_LLCS, &deadline);
    if (other >= 0 && take_first_takeable(cpu_dsq((u32)other), cpu, STEAL_SCAN_DEPTH, may_take_hot((u32)other), true))
      return DSP_TOOK;
  }

  return DSP_NONE;
}

// Wake an idle cpu for the first task waiting in the queue of @cpu, if it may run
// elsewhere (not pinned to the cpu, also not for the moment by migrate_disable()).
static __always_inline void kick_idle_for_waiting(u32 cpu)
{
  struct task_struct* p = dsq_first(cpu_dsq(cpu));

  if (p && p->nr_cpus_allowed > 1 && !is_migration_disabled(p))
  {
    s32 idle = pick_idle(p, get_task_ctx(p), cpu);
    if (idle >= 0)
      scx_bpf_kick_cpu(idle, SCX_KICK_IDLE);
  }
}

#endif  // DISPATCHES_H
