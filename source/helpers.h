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

static __always_inline u64 task_band(const struct task_struct* p)
{
  if (is_percpu_kthread(p))
    return BAND_0;

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
