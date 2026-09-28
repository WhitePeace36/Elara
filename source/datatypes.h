// SPDX-License-Identifier: GPL-2.0
//
// Author: Timon Stipkovits <timon2201@gmail.com>
//
// This software may be used and distributed according to the terms of the
// GNU General Public License version 2.

#ifndef DATATYPES_H
#define DATATYPES_H
#include "defines.h"

const volatile u32 nr_llcs = 1;
const volatile u32 cpu_to_llc[MAX_CPUS] = {};
// Set by userspace from the topology. Offline cpus have queues too, but nobody
// serves them, so no task may ever be placed there.
const volatile u8 cpu_online[MAX_CPUS] = {};

struct task_ctx
{
  // vtime of the task on the band timeline of @key_cpu: the key it was queued
  // with plus the cpu time it used since then
  u64 key;
  u32 key_cpu;
  u64 started_at;

  u64 granted_slice;
  u64 resume_slice;
  u64 last_migrated_at;
};

struct dispatch_ctx
{
  // band and key of the task running on this cpu (BAND_AMOUNT: none)
  u64 running_band;
  u64 running_key;
  u64 running_since;
  // vtime reference per band: the highest key started on this cpu
  u64 band_vtime[BAND_AMOUNT];
  u64 band_head_ts[BAND_AMOUNT];
  u64 last_override_ts;
  bool preempt_pending;
};

struct pick_scratch
{
  u64 best_load;
  u64 sampled;
  s32 best;
};

struct
{
  __uint(type, BPF_MAP_TYPE_PERCPU_ARRAY);
  __uint(max_entries, 1);
  __type(key, u32);
  __type(value, struct pick_scratch);

} pick_scratch_map SEC(".maps");

struct
{
  __uint(type, BPF_MAP_TYPE_PERCPU_ARRAY);
  __uint(max_entries, 1);
  __type(key, u32);
  __type(value, struct dispatch_ctx);
} dispatch_state SEC(".maps");

struct
{
  __uint(type, BPF_MAP_TYPE_TASK_STORAGE);
  __uint(map_flags, BPF_F_NO_PREALLOC);
  __type(key, int);
  __type(value, struct task_ctx);
} task_ctx_store SEC(".maps");

#endif  // DATATYPES_H
