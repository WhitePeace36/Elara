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

static __always_inline u64 cpu_load_ahead(u32 cpu, u64 band)
{
  u64 load = 0;

  struct dispatch_ctx* dctx = get_dispatch_ctx(cpu);
  if (dctx && dctx->running_band <= band)
    load++;
  // A cpu taken by an RT or deadline task is busy for every band. Without this
  // it looks empty (stopping cleared running_band) and placement would even
  // prefer it when no cpu is idle.
  else if (cpu_taken_by_rt(cpu))
    load += RT_CPU_LOAD;

  load += dsq_queued(SCX_DSQ_LOCAL_ON | cpu);

  load += dsq_queued(band_dsq(BAND_0, cpu));
  if (band >= BAND_1)
    load += dsq_queued(band_dsq(BAND_1, cpu));
  if (band >= BAND_2)
    load += dsq_queued(band_dsq(BAND_2, cpu));
  if (band >= BAND_3)
    load += dsq_queued(band_dsq(BAND_3, cpu));
  if (band >= BAND_4)
    load += dsq_queued(band_dsq(BAND_4, cpu));

  return load;
}

static __always_inline s32 pick_enqueue_cpu(struct task_struct* p, struct task_ctx* tctx, u64 band, u32 cpu, u64 now)
{
  u64 best_load = cpu_load_ahead(cpu, band);
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
  bool scan_whole_llc = band <= BAND_SCAN_WHOLE_LLC;
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

    u64 load = cpu_load_ahead(other, band);
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

static __always_inline bool try_acquire_task_from_other_cpu(u64 band, u32 cpu, bool sameLLC, u64 now)
{
  u32 my_llc = cpu_llc_id(cpu);
  u32 nr_cpu_ids = scx_bpf_nr_cpu_ids();
  u32 start = bpf_get_prandom_u32() % nr_cpu_ids;
  u32 i;

  bpf_for(i, 0, nr_cpu_ids)
  {
    u32 other = (start + i) % nr_cpu_ids;
    if (other == cpu || !cpu_is_online(other))
      continue;
    if (sameLLC && cpu_llc_id(other) != my_llc)
      continue;
    if (!sameLLC && cpu_llc_id(other) == my_llc)
      continue;

    u64 dsq = band_dsq(band, other);

    if (dsq_queued(dsq) && scx_bpf_dsq_move_to_local(dsq, 0))
    {
      struct dispatch_ctx* victim = get_dispatch_ctx(other);
      if (victim)
        stamp_band_head_ts(victim, band, now);
      return true;
    }
  }
  return false;
}

// How long the first task of @dsq has been waiting, 0 if it is empty.
static __always_inline u64 dsq_head_wait(u64 dsq, u64 now)
{
  struct task_struct* p;
  u64 wait = 0;

  bpf_for_each(scx_dsq, p, dsq, 0)
  {
    struct task_ctx* tctx = get_task_ctx(p);
    if (tctx)
      wait = elapsed(now, tctx->queued_at);
    break;
  }
  return wait;
}

// Take a task of @band from a cpu of the LLC that is taken by an RT or deadline
// task right now: that cpu can't run it until the RT task is done. Only a task
// that has waited longer than the first task of @band here (and at least
// RT_STEAL_MIN_WAIT_NS) is taken, the one that waited longest.
static __always_inline bool try_acquire_from_rt_cpu(u64 band, u32 cpu, u64 now)
{
  u32 my_llc = cpu_llc_id(cpu);
  u32 nr_cpu_ids = scx_bpf_nr_cpu_ids();
  u32 i;

  // The own head is only looked at once there is a candidate at all.
  bool have_own = false;
  u64 best_wait = 0;
  s32 best = -1;

  bpf_for(i, 0, nr_cpu_ids)
  {
    if (i == cpu || !cpu_is_online(i) || cpu_llc_id(i) != my_llc)
      continue;

    u64 dsq = band_dsq(band, i);
    if (!dsq_queued(dsq) || !cpu_taken_by_rt(i))
      continue;

    if (!have_own)
    {
      u64 own_wait = dsq_head_wait(band_dsq(band, cpu), now);
      best_wait = own_wait > RT_STEAL_MIN_WAIT_NS ? own_wait : RT_STEAL_MIN_WAIT_NS;
      have_own = true;
    }

    u64 wait = dsq_head_wait(dsq, now);
    if (wait > best_wait)
    {
      best_wait = wait;
      best = i;
    }
  }

  if (best < 0 || !scx_bpf_dsq_move_to_local(band_dsq(band, (u32)best), 0))
    return false;

  struct dispatch_ctx* victim = get_dispatch_ctx((u32)best);
  if (victim)
    stamp_band_head_ts(victim, band, now);
  return true;
}

// An RT or deadline task took @cpu: the tasks already picked for it (its local
// DSQ) go back through enqueue(), which sees the cpu as busy and places them on
// another one. Tasks waiting in its band queues stay: other cpus take them over
// in dispatch (try_acquire_from_rt_cpu) when they can run them.
static __always_inline void evacuate_rt_cpu(u32 cpu)
{
  if (dsq_queued(SCX_DSQ_LOCAL_ON | cpu))
    scx_bpf_reenqueue_local_from_anywhere();
}

static __always_inline s64 band_overrun(struct dispatch_ctx* dctx, u64 band, u32 cpu, u64 now, u64 budget)
{
  if (band >= BAND_AMOUNT || !dsq_queued(band_dsq(band, cpu)))
    return 0;
  return (s64)(now - dctx->band_head_ts[band]) - (s64)budget;
}

static __always_inline u64 most_starved_band(struct dispatch_ctx* dctx, u32 cpu, u64 now)
{
  if (now - dctx->last_override_ts < STARVE_OVERRIDE_COOLDOWN_NS)
    return BAND_AMOUNT;

  u64 worst_band = BAND_AMOUNT;
  s64 worst_overrun = 0;
  s64 overrun;

  overrun = band_overrun(dctx, BAND_1, cpu, now, STARVE_BUDGET_BAND_1_NS);
  if (overrun > worst_overrun)
  {
    worst_overrun = overrun;
    worst_band = BAND_1;
  }

  overrun = band_overrun(dctx, BAND_2, cpu, now, STARVE_BUDGET_BAND_2_NS);
  if (overrun > worst_overrun)
  {
    worst_overrun = overrun;
    worst_band = BAND_2;
  }

  overrun = band_overrun(dctx, BAND_3, cpu, now, STARVE_BUDGET_BAND_3_NS);
  if (overrun > worst_overrun)
  {
    worst_overrun = overrun;
    worst_band = BAND_3;
  }

  overrun = band_overrun(dctx, BAND_4, cpu, now, STARVE_BUDGET_BAND_4_NS);
  if (overrun > worst_overrun)
  {
    worst_overrun = overrun;
    worst_band = BAND_4;
  }

  return worst_band;
}

static __always_inline bool take_from_local_band(struct dispatch_ctx* dctx, u64 band, u32 cpu, u64 now)
{
  u64 dsq = band_dsq(band, cpu);
  if (!dsq_queued(dsq) || !scx_bpf_dsq_move_to_local(dsq, 0))
    return false;

  if (dctx)
    stamp_band_head_ts(dctx, band, now);
  return true;
}

static __always_inline u64 dsq_head_key(u64 dsq)
{
  struct task_struct* p;
  u64 key = (u64)-1;

  bpf_for_each(scx_dsq, p, dsq, 0)
  {
    key = p->scx.dsq_vtime;
    break;
  }
  return key;
}

static __always_inline void kick_idle_for_waiting(u32 cpu)
{
  struct task_struct* p;
  u32 band;

  bpf_for(band, 0, BAND_AMOUNT)
  {
    bpf_for_each(scx_dsq, p, band_dsq(band, cpu), 0)
    {
      if (p->nr_cpus_allowed > 1)
      {
        s32 idle = scx_bpf_pick_idle_cpu(p->cpus_ptr, 0);
        if (idle >= 0)
          scx_bpf_kick_cpu(idle, SCX_KICK_IDLE);
        return;
      }
      break;
    }
  }
}

#define SERVE_CONTINUE 0
#define SERVE_KEEP_PREV 1
#define SERVE_TOOK_TASK 2

static __always_inline int serve_local_band(struct dispatch_ctx* dctx, u64 band, u32 cpu, u64 now, u64 prev_band, u64 prev_key)
{
  if (prev_band < band)
    return SERVE_KEEP_PREV;

  u64 dsq = band_dsq(band, cpu);
  if (prev_band == band && (s64)(prev_key - dsq_head_key(dsq)) <= 0)
    return SERVE_KEEP_PREV;

  if (take_from_local_band(dctx, band, cpu, now))
    return SERVE_TOOK_TASK;

  if (prev_band == band)
    return SERVE_KEEP_PREV;
  return SERVE_CONTINUE;
}

static __always_inline bool dispatch_dsq_per_cpu(u32 cpu, u64 prev_band, u64 prev_key)
{
  struct dispatch_ctx* dctx = get_dispatch_ctx(cpu);
  u64 now = bpf_ktime_get_ns();
  int ret;

  // A starved band goes first, but not on a dispatch caused by a preemption:
  // the task that preempted must run now, the starved band gets the next one.
  // It keeps going first until it used its override budget.
  if (dctx && !dctx->preempt_pending)
  {
    if (dctx->override_left)
    {
      if (take_from_local_band(dctx, dctx->override_band, cpu, now))
        return false;
      dctx->override_left = 0;
    }

    u64 starved = most_starved_band(dctx, cpu, now);
    if (starved != BAND_AMOUNT && take_from_local_band(dctx, starved, cpu, now))
    {
      dctx->last_override_ts = now;
      dctx->override_band = starved;
      dctx->override_left = STARVE_OVERRIDE_BUDGET_NS;
      return false;
    }
  }

  // Bands are strict across cpus too: a task of a better band than anything
  // this cpu could run next (prev or its own queues) and waiting on another cpu
  // of the LLC is taken over first.
  // (Written out per band: band numbers that come from a loop variable can
  // lose their range in the verifier and make array accesses fail.)
  u64 local_best = prev_band < BAND_AMOUNT ? prev_band : BAND_AMOUNT;
  if (local_best > BAND_0 && dsq_queued(band_dsq(BAND_0, cpu)))
    local_best = BAND_0;
  else if (local_best > BAND_1 && dsq_queued(band_dsq(BAND_1, cpu)))
    local_best = BAND_1;
  else if (local_best > BAND_2 && dsq_queued(band_dsq(BAND_2, cpu)))
    local_best = BAND_2;
  else if (local_best > BAND_3 && dsq_queued(band_dsq(BAND_3, cpu)))
    local_best = BAND_3;
  else if (local_best > BAND_4 && dsq_queued(band_dsq(BAND_4, cpu)))
    local_best = BAND_4;

  if (local_best > BAND_0 && try_acquire_task_from_other_cpu(BAND_0, cpu, true, now))
    return false;
  if (local_best > BAND_1 && try_acquire_task_from_other_cpu(BAND_1, cpu, true, now))
    return false;
  if (local_best > BAND_2 && try_acquire_task_from_other_cpu(BAND_2, cpu, true, now))
    return false;
  if (local_best > BAND_3 && try_acquire_task_from_other_cpu(BAND_3, cpu, true, now))
    return false;

  // Tasks of the same band as the best this cpu could run wait on a cpu taken
  // by an RT task: they can't run there, so they are taken over too.
  if (local_best == BAND_0 && try_acquire_from_rt_cpu(BAND_0, cpu, now))
    return false;
  if (local_best == BAND_1 && try_acquire_from_rt_cpu(BAND_1, cpu, now))
    return false;
  if (local_best == BAND_2 && try_acquire_from_rt_cpu(BAND_2, cpu, now))
    return false;
  if (local_best == BAND_3 && try_acquire_from_rt_cpu(BAND_3, cpu, now))
    return false;
  if (local_best == BAND_4 && try_acquire_from_rt_cpu(BAND_4, cpu, now))
    return false;

  if (!(prev_band == BAND_0 && (s64)(prev_key - dsq_head_key(band_dsq(BAND_0, cpu))) <= 0) && take_from_local_band(dctx, BAND_0, cpu, now))
    return false;

  ret = serve_local_band(dctx, BAND_0, cpu, now, prev_band, prev_key);
  if (ret != SERVE_CONTINUE)
    return ret == SERVE_KEEP_PREV;
  ret = serve_local_band(dctx, BAND_1, cpu, now, prev_band, prev_key);
  if (ret != SERVE_CONTINUE)
    return ret == SERVE_KEEP_PREV;
  ret = serve_local_band(dctx, BAND_2, cpu, now, prev_band, prev_key);
  if (ret != SERVE_CONTINUE)
    return ret == SERVE_KEEP_PREV;
  ret = serve_local_band(dctx, BAND_3, cpu, now, prev_band, prev_key);
  if (ret != SERVE_CONTINUE)
    return ret == SERVE_KEEP_PREV;
  ret = serve_local_band(dctx, BAND_4, cpu, now, prev_band, prev_key);
  if (ret != SERVE_CONTINUE)
    return ret == SERVE_KEEP_PREV;

  if (try_acquire_task_from_other_cpu(BAND_0, cpu, true, now))
    return false;
  if (try_acquire_task_from_other_cpu(BAND_1, cpu, true, now))
    return false;
  if (try_acquire_task_from_other_cpu(BAND_2, cpu, true, now))
    return false;
  if (try_acquire_task_from_other_cpu(BAND_3, cpu, true, now))
    return false;
  if (try_acquire_task_from_other_cpu(BAND_4, cpu, true, now))
    return false;

  if (nr_llcs > 1)
  {
    if (try_acquire_task_from_other_cpu(BAND_0, cpu, false, now))
      return false;
    if (try_acquire_task_from_other_cpu(BAND_1, cpu, false, now))
      return false;
    if (try_acquire_task_from_other_cpu(BAND_2, cpu, false, now))
      return false;
    if (try_acquire_task_from_other_cpu(BAND_3, cpu, false, now))
      return false;
    if (try_acquire_task_from_other_cpu(BAND_4, cpu, false, now))
      return false;
  }

  return false;
}

#endif  // DISPATCHES_H
