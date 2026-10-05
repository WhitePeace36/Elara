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
    ret = scx_bpf_create_dsq(band_dsq(BAND_0, cpu), -1);
    if (ret)
      return ret;
    ret = scx_bpf_create_dsq(band_dsq(BAND_1, cpu), -1);
    if (ret)
      return ret;
    ret = scx_bpf_create_dsq(band_dsq(BAND_2, cpu), -1);
    if (ret)
      return ret;
    ret = scx_bpf_create_dsq(band_dsq(BAND_3, cpu), -1);
    if (ret)
      return ret;
    ret = scx_bpf_create_dsq(band_dsq(BAND_4, cpu), -1);
    if (ret)
      return ret;
  }

  bpf_for(cpu, 0, nr_cpu_ids)
  {
    struct dispatch_ctx* dispatch_ctx = get_dispatch_ctx(cpu);
    if (!dispatch_ctx)
      return -ENOMEM;

    u64 now = bpf_ktime_get_ns();
    dispatch_ctx->running_band = BAND_AMOUNT;

    u32 band;
    bpf_for(band, 0, BAND_AMOUNT)
    {
      if (band < BAND_AMOUNT)
      {
        dispatch_ctx->band_vtime[band] = VTIME_BASE;
        dispatch_ctx->band_head_ts[band] = now;
      }
    }

    dispatch_ctx->last_override_ts = now;
    dispatch_ctx->last_rt_evacuate_ts = 0;
    dispatch_ctx->preempt_pending = false;
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

  tctx->key = VTIME_BASE;
  tctx->key_cpu = KEY_CPU_NONE;
  tctx->key_band = BAND_2;
  tctx->boost_band = BAND_AMOUNT;
  tctx->started_at = now;
  tctx->granted_slice = 0;
  tctx->resume_slice = 0;
  tctx->last_migrated_at = 0;

  return 0;
}

void BPF_STRUCT_OPS(elara_exit_task, struct task_struct* p, struct scx_exit_task_args* args) { }

static __always_inline void apply_wake_boost(struct task_struct* p, struct task_ctx* tctx)
{
  if (!bpf_in_task())
    return;

  struct task_struct* waker = bpf_get_current_task_btf();
  if (!waker || waker == p || (waker->flags & PF_IDLE))
    return;
  if (!WAKE_BOOST_FROM_KTHREADS && (waker->flags & PF_KTHREAD))
    return;

  u64 waker_band = effective_band(waker, get_task_ctx(waker));
  if (waker_band < task_band(p) && waker_band < tctx->boost_band)
    tctx->boost_band = waker_band;
}

s32 BPF_STRUCT_OPS(elara_select_cpu, struct task_struct* p, s32 prev_cpu, u64 wake_flags)
{
  bool is_idle = false;
  struct task_ctx* tctx = get_task_ctx(p);

  if (tctx && (wake_flags & SCX_WAKE_TTWU))
    apply_wake_boost(p, tctx);

  s32 cpu = scx_bpf_select_cpu_dfl(p, prev_cpu, wake_flags, &is_idle);

  // Run directly on the idle cpu, unless tasks of the same or a better band are
  // already queued there (their kick is still on the way): the local DSQ runs
  // before them. Otherwise enqueue() queues it properly.
  u64 band = effective_band(p, tctx);
  if (is_idle && cpu_load_ahead(cpu, band) == 0)
  {
    if (tctx)
      set_task_key(tctx, task_key(get_dispatch_ctx(cpu), band, task_lag(tctx)), cpu, band);
    scx_bpf_dsq_insert(p, SCX_DSQ_LOCAL, SLICE_NS, 0);
  }

  return cpu;
}

void BPF_STRUCT_OPS(elara_enqueue, struct task_struct* p, u64 enq_flags)
{
  struct task_ctx* tctx = get_task_ctx(p);
  u32 cpu = scx_bpf_task_cpu(p);
  u64 now = bpf_ktime_get_ns();

  if (tctx && (enq_flags & SCX_ENQ_WAKEUP))
    apply_wake_boost(p, tctx);

  u64 band = effective_band(p, tctx);

  // A preempted task goes back to its own queue with the rest of its slice,
  // unless its cpu is taken by an RT task now: then it is placed like any other.
  if (tctx && tctx->resume_slice && cpu_taken_by_rt(cpu))
    tctx->resume_slice = 0;

  if (tctx && tctx->resume_slice)
  {
    u64 slice = tctx->resume_slice;
    u64 own_dsq = band_dsq(band, cpu);
    struct dispatch_ctx* own = get_dispatch_ctx(cpu);
    tctx->resume_slice = 0;
    if (own && band != BAND_0 && dsq_queued(own_dsq) == 0)
      stamp_band_head_ts(own, band, now);
    scx_bpf_dsq_insert_vtime(p, own_dsq, slice, tctx->key, enq_flags);
    return;
  }

  u32 target = tctx ? (u32)pick_enqueue_cpu(p, tctx, band, cpu, now) : cpu;
  if (!cpu_is_online(target))
    target = cpu;
  u64 dsq = band_dsq(band, target);
  struct dispatch_ctx* dctx = get_dispatch_ctx(target);

  u64 key = task_key(dctx, band, tctx ? task_lag(tctx) : 0);
  if (tctx)
    set_task_key(tctx, key, target, band);

  if (dctx && band != BAND_0 && dsq_queued(dsq) == 0)
    stamp_band_head_ts(dctx, band, now);

  scx_bpf_dsq_insert_vtime(p, dsq, SLICE_NS, key, enq_flags);

  if (!dctx)
  {
    scx_bpf_kick_cpu(target, SCX_KICK_IDLE);
    return;
  }

  u64 running_band = dctx->running_band;
  if (running_band >= BAND_AMOUNT)
  {
    scx_bpf_kick_cpu(target, SCX_KICK_IDLE);
    return;
  }

  if ((enq_flags & SCX_ENQ_WAKEUP) && band < running_band)
  {
    dctx->preempt_pending = true;
    scx_bpf_kick_cpu(target, SCX_KICK_PREEMPT);
    return;
  }

  scx_bpf_kick_cpu(target, SCX_KICK_IDLE);
}

void BPF_STRUCT_OPS(elara_dispatch, s32 cpu, struct task_struct* prev)
{
  u64 prev_band = BAND_AMOUNT;
  u64 prev_key = (u64)-1;
  struct task_ctx* pctx = NULL;
  struct dispatch_ctx* dctx = get_dispatch_ctx(cpu);
  u64 now = bpf_ktime_get_ns();

  if (prev && (prev->scx.flags & SCX_TASK_QUEUED))
  {
    pctx = get_task_ctx(prev);
    if (pctx && dctx)
    {
      // The slice is over and a wake boost ends with it. A preemption (dispatch
      // is called before stopping then) keeps the boost: the task gets the
      // rest of its slice back in the boosted band.
      if (!dctx->preempt_pending)
        pctx->boost_band = BAND_AMOUNT;
      prev_band = effective_band(prev, pctx);
      u64 used = elapsed(now, pctx->started_at);
      pctx->granted_slice = pctx->granted_slice > used ? pctx->granted_slice - used : 0;
      pctx->key += used;
      prev_key = task_key(dctx, prev_band, task_lag(pctx));
      set_task_key(pctx, prev_key, cpu, prev_band);
      pctx->started_at = now;
    }
  }

  if (!dispatch_dsq_per_cpu(cpu, prev_band, prev_key) || !prev || !pctx || !dctx)
    return;

  // prev keeps running for a new slice, a boost left over from a preemption
  // that didn't replace it ends here.
  if (pctx->boost_band != BAND_AMOUNT)
  {
    pctx->boost_band = BAND_AMOUNT;
    prev_band = task_band(prev);
    prev_key = task_key(dctx, prev_band, task_lag(pctx));
    set_task_key(pctx, prev_key, cpu, prev_band);
  }

  scx_bpf_task_set_slice(prev, SLICE_NS);
  pctx->granted_slice = SLICE_NS;
  advance_band_reference(dctx, prev_band, prev_key);
  dctx->running_band = prev_band;
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

  u64 band = effective_band(p, context);

  if (context->key_cpu != cpu || context->key_band != band)
    set_task_key(context, task_key(dispatch_ctx, band, task_lag(context)), cpu, band);

  advance_band_reference(dispatch_ctx, band, context->key);
  dispatch_ctx->running_band = band;
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
  tctx->key += used_ns;

  if (dctx->preempt_pending && runnable && task->scx.slice == 0 && tctx->granted_slice > used_ns + RESUME_SLICE_MIN_NS)
    tctx->resume_slice = tctx->granted_slice - used_ns;

  if (!tctx->resume_slice)
    tctx->boost_band = BAND_AMOUNT;

  dctx->preempt_pending = false;
  dctx->running_band = BAND_AMOUNT;
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
  tctx->boost_band = BAND_AMOUNT;
}

// Every context switch: when the next task is an RT or deadline task, the cpu
// is taken from us, from one of our tasks or from idle. The tasks waiting on it
// are placed again, and an idle cpu is woken for the first one still waiting.
// (This replaces ops.cpu_release, which newer kernels deprecate.)
SEC("tp_btf/sched_switch")
int BPF_PROG(elara_sched_switch, bool preempt, struct task_struct* prev, struct task_struct* next, unsigned int prev_state)
{
  int policy = next->policy;
  if (policy != SCHED_FIFO && policy != SCHED_RR && policy != SCHED_DEADLINE)
    return 0;

  u32 cpu = bpf_get_smp_processor_id();
  evacuate_rt_cpu(cpu, bpf_ktime_get_ns());
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
