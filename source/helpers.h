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

static __always_inline u64 sanitize_band(u64 band)
{
  return band < BAND_AMOUNT ? band : BAND_AMOUNT - 1;
}

static __always_inline u64 band_dsq(u64 band, u32 cpu)
{
  return DSQ_BASE + sanitize_band(band) * DSQ_BAND_STRIDE + cpu;
}

static __always_inline bool is_percpu_kthread(const struct task_struct* p)
{
  return (p->flags & PF_KTHREAD) && p->nr_cpus_allowed == 1;
}

static __always_inline bool comm_starts_with(const struct task_struct* p, const char* prefix, int len)
{
  for (int i = 0; i < len; i++)
  {
    if (p->comm[i] != prefix[i])
      return false;
  }
  return true;
}

// Kernel threads that barely use cpu, but others wait for them directly, so
// they always go into band 0:
// - kswapd/kcompactd (one per NUMA node): when they fall behind, every task
//   that allocates memory has to reclaim or compact by itself, which stalls it
//   much longer than the thread would.
// - kthreadd: starts every kernel thread, also new kworkers a workqueue needs.
// - rcu_exp_*: expedited RCU grace periods, synchronize_rcu_expedited() blocks
//   until they are done.
// - oom_reaper: frees the memory of an OOM killed task.
// The length includes the terminating 0 where the whole name has to match.
static __always_inline bool is_urgent_kthread(const struct task_struct* p)
{
  if (!(p->flags & PF_KTHREAD))
    return false;
  return comm_starts_with(p, "kswapd", 6) || comm_starts_with(p, "kcompactd", 9) || comm_starts_with(p, "kthreadd", 9) ||
         comm_starts_with(p, "rcu_exp_", 8) || comm_starts_with(p, "oom_reaper", 11);
}

// Kernel threads doing work that tasks wait for, but which can also be heavy,
// so they go into band 1 at least and share it fairly with the tasks there:
// - unbound kworkers (kworker/u*): gpu job submission (amdgpu gfx_*), I/O
//   completion (btrfs-endio), events_unbound, ... Per-cpu kworkers are already
//   in band 0.
// - jbd2 (ext4 journal) and btrfs-transaction: fsync waits for them.
static __always_inline bool is_service_kthread(const struct task_struct* p)
{
  if (!(p->flags & PF_KTHREAD))
    return false;
  if (p->flags & PF_WQ_WORKER)
    return true;
  return comm_starts_with(p, "jbd2/", 5) || comm_starts_with(p, "btrfs-transacti", 15);
}

static __always_inline u64 nice_band(const struct task_struct* p)
{
  int nice = p->static_prio - NICE_0_PRIO;

  if (nice <= BAND_0_MAX_NICE)
    return BAND_0;
  if (nice <= BAND_1_MAX_NICE)
    return BAND_1;
  if (nice <= BAND_2_MAX_NICE)
    return BAND_2;
  if (nice <= BAND_3_MAX_NICE)
    return BAND_3;
  return BAND_4;
}

static __always_inline u64 task_band(const struct task_struct* p)
{
  if (is_percpu_kthread(p) || is_urgent_kthread(p))
    return BAND_0;

  u64 band = nice_band(p);
  if (band > BAND_1 && is_service_kthread(p))
    return BAND_1;
  return band;
}

static __always_inline u64 dsq_queued(u64 dsq)
{
  s32 n = scx_bpf_dsq_nr_queued(dsq);
  return n > 0 ? (u64)n : 0;
}

static __always_inline void stamp_band_head_ts(struct dispatch_ctx* dctx, u64 band, u64 now)
{
  if (band < BAND_AMOUNT)
    dctx->band_head_ts[band] = now;
}

static __always_inline u64 band_reference(struct dispatch_ctx* dctx, u64 band)
{
  if (band < BAND_AMOUNT)
    return dctx->band_vtime[band];
  return VTIME_BASE;
}

static __always_inline void advance_band_reference(struct dispatch_ctx* dctx, u64 band, u64 key)
{
  if (band < BAND_AMOUNT && (s64)(key - dctx->band_vtime[band]) > 0)
    dctx->band_vtime[band] = key;
}

static __always_inline s64 clamp_lag(s64 lag)
{
  if (lag > (s64)LAG_MAX_NS)
    return LAG_MAX_NS;
  if (lag < -(s64)LAG_MAX_NS)
    return -(s64)LAG_MAX_NS;
  return lag;
}

static __always_inline u64 task_key(struct dispatch_ctx* dctx, u64 band, s64 lag)
{
  u64 reference = dctx ? band_reference(dctx, band) : VTIME_BASE;
  return reference + lag;
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

static __always_inline u64 elapsed(u64 now, u64 last)
{
  return now > last ? now - last : 0;
}

static __always_inline s64 task_lag(struct task_ctx* tctx, u64 band)
{
  if (tctx->key_cpu == KEY_CPU_NONE)
    return 0;
  struct dispatch_ctx* from = get_dispatch_ctx(tctx->key_cpu);
  if (!from)
    return 0;
  return clamp_lag((s64)(tctx->key - band_reference(from, band)));
}

#endif  // HELPERS_H
