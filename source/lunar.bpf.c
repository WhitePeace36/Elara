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

s32 BPF_STRUCT_OPS_SLEEPABLE(lunar_init)
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
    dispatch_ctx->running_key = VTIME_BASE;

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
    dispatch_ctx->preempt_pending = false;
  }

  return 0;
}

s32 BPF_STRUCT_OPS_SLEEPABLE(lunar_init_task, struct task_struct* p, struct scx_init_task_args* args)
{
  struct task_ctx* tctx;
  u64 now = bpf_ktime_get_ns();

  tctx = bpf_task_storage_get(&task_ctx_store, p, NULL, BPF_LOCAL_STORAGE_GET_F_CREATE);
  if (!tctx)
    return -ENOMEM;

  // No cpu yet: task_lag() is 0, so a new task starts exactly at the band
  // reference, without lead or debt.
  tctx->key = VTIME_BASE;
  tctx->key_cpu = KEY_CPU_NONE;
  tctx->started_at = now;
  tctx->granted_slice = 0;
  tctx->resume_slice = 0;
  tctx->last_migrated_at = 0;

  return 0;
}

void BPF_STRUCT_OPS(lunar_exit_task, struct task_struct* p, struct scx_exit_task_args* args) { }

s32 BPF_STRUCT_OPS(lunar_select_cpu, struct task_struct* p, s32 prev_cpu, u64 wake_flags)
{
  bool is_idle = false;
  s32 cpu = scx_bpf_select_cpu_dfl(p, prev_cpu, wake_flags, &is_idle);

  if (is_idle)
  {
    struct task_ctx* tctx = get_task_ctx(p);
    if (tctx)
    {
      tctx->key = task_key(get_dispatch_ctx(cpu), task_band(p), task_lag(tctx, task_band(p)));
      tctx->key_cpu = cpu;
    }
    scx_bpf_dsq_insert(p, SCX_DSQ_LOCAL, SLICE_NS, 0);
  }

  return cpu;
}

void BPF_STRUCT_OPS(lunar_enqueue, struct task_struct* p, u64 enq_flags)
{
  struct task_ctx* tctx = get_task_ctx(p);
  u64 band = task_band(p);
  u32 cpu = scx_bpf_task_cpu(p);
  u64 now = bpf_ktime_get_ns();

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

  u64 key = task_key(dctx, band, tctx ? task_lag(tctx, band) : 0);
  if (tctx)
  {
    tctx->key = key;
    tctx->key_cpu = target;
  }

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

  // No preemption: still kick the cpu if it is idle or about to go idle.
  // Without that a task inserted while the target is between stopping and
  // dispatch could be missed and wait until something else wakes that cpu.
  if (!(enq_flags & SCX_ENQ_WAKEUP) || band > running_band)
  {
    scx_bpf_kick_cpu(target, SCX_KICK_IDLE);
    return;
  }

  bool preempt = band < running_band;
  if (!preempt)
  {
    u64 running_vtime = dctx->running_key + elapsed(now, dctx->running_since);
    preempt = (s64)(running_vtime - key) > (s64)SAME_BAND_PREEMPT_GRAN_NS;
  }

  if (preempt)
  {
    dctx->preempt_pending = true;
    scx_bpf_kick_cpu(target, SCX_KICK_PREEMPT);
  }
  else
  {
    scx_bpf_kick_cpu(target, SCX_KICK_IDLE);
  }
}

void BPF_STRUCT_OPS(lunar_dispatch, s32 cpu, struct task_struct* prev)
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
      prev_band = task_band(prev);
      u64 used = elapsed(now, pctx->started_at);
      pctx->granted_slice = pctx->granted_slice > used ? pctx->granted_slice - used : 0;
      pctx->key += used;
      prev_key = task_key(dctx, prev_band, task_lag(pctx, prev_band));
      pctx->key = prev_key;
      pctx->key_cpu = cpu;
      pctx->started_at = now;
    }
  }

  if (!dispatch_dsq_per_cpu(cpu, prev_band, prev_key) || !prev || !pctx || !dctx)
    return;

  scx_bpf_task_set_slice(prev, SLICE_NS);
  pctx->granted_slice = SLICE_NS;
  advance_band_reference(dctx, prev_band, prev_key);
  dctx->running_band = prev_band;
  dctx->running_key = prev_key;
  dctx->running_since = now;
  dctx->preempt_pending = false;
}

void BPF_STRUCT_OPS(lunar_running, struct task_struct* p)
{
  struct task_ctx* context = get_task_ctx(p);
  if (!context)
    return;

  // The cpu of the task, not the executing one: renice or setaffinity from
  // another cpu calls running/stopping remotely.
  u32 cpu = scx_bpf_task_cpu(p);
  struct dispatch_ctx* dispatch_ctx = get_dispatch_ctx(cpu);
  if (!dispatch_ctx)
    return;

  u64 band = task_band(p);

  if (context->key_cpu != cpu)
  {
    context->key = task_key(dispatch_ctx, band, task_lag(context, band));
    context->key_cpu = cpu;
  }

  advance_band_reference(dispatch_ctx, band, context->key);
  dispatch_ctx->running_band = band;
  dispatch_ctx->running_key = context->key;
  dispatch_ctx->preempt_pending = false;

  context->started_at = bpf_ktime_get_ns();
  dispatch_ctx->running_since = context->started_at;
  context->granted_slice = p->scx.slice;
  context->resume_slice = 0;

  kick_idle_for_waiting(cpu);
}

void BPF_STRUCT_OPS(lunar_stopping, struct task_struct* task, bool runnable)
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

  // Only a preemption by our kick (it sets the slice to 0). An RT task taking
  // the cpu leaves the slice and the kernel puts the task back directly.
  if (dctx->preempt_pending && runnable && task->scx.slice == 0 && tctx->granted_slice > used_ns + RESUME_SLICE_MIN_NS)
    tctx->resume_slice = tctx->granted_slice - used_ns;

  dctx->preempt_pending = false;
  dctx->running_band = BAND_AMOUNT;
}

void BPF_STRUCT_OPS(lunar_exit, struct scx_exit_info* ei)
{
  UEI_RECORD(uei, ei);
}

void BPF_STRUCT_OPS(lunar_quiescent, struct task_struct* p, u64 deq_flags)
{
  struct task_ctx* tctx = get_task_ctx(p);
  if (!tctx)
    return;

  tctx->resume_slice = 0;
}

SCX_OPS_DEFINE(lunar_ops,
               .init = (void*)lunar_init,
               .init_task = (void*)lunar_init_task,
               .exit_task = (void*)lunar_exit_task,
               .select_cpu = (void*)lunar_select_cpu,
               .quiescent = (void*)lunar_quiescent,
               .running = (void*)lunar_running,
               .enqueue = (void*)lunar_enqueue,
               .dispatch = (void*)lunar_dispatch,
               .stopping = (void*)lunar_stopping,
               .exit = (void*)lunar_exit,
               .name = "scx_lunar");
