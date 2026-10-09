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
const volatile u8 cpu_online[MAX_CPUS] = {};

struct task_ctx
{
  u64 key;
  u32 key_cpu;
  u32 key_band;
  u32 boost_band;
  u64 boost_used;
  u64 started_at;

  u64 granted_slice;
  u64 resume_slice;
  u64 last_migrated_at;
  // when the task was last put into a queue (not reset by a re-enqueue)
  u64 queued_at;
};

struct dispatch_ctx
{
  u64 running_band;
  u64 band_vtime[BAND_AMOUNT];
  u64 band_head_ts[BAND_AMOUNT];
  u64 last_override_ts;
  u64 override_band;
  u64 override_left;
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
