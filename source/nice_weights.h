// SPDX-License-Identifier: GPL-2.0
//
// Generated: weight of each nice value, 1024 at nice 0, a factor of 1.5 per
// nice step (CFS: ~1.25). Index: nice + 20.

#ifndef NICE_WEIGHTS_H
#define NICE_WEIGHTS_H

#define NICE_WEIGHT_STEP "1.5"

static const u32 nice_weights[40] = {
  3405063, 2270042, 1513361, 1008908, 672605,  // nice -20 .. -16
  448403, 298936, 199290, 132860, 88574,  // nice -15 .. -11
  59049, 39366, 26244, 17496, 11664,  // nice -10 .. -6
  7776, 5184, 3456, 2304, 1536,  // nice -5 .. -1
  1024, 683, 455, 303, 202,  // nice 0 .. 4
  135, 90, 60, 40, 27,  // nice 5 .. 9
  18, 12, 8, 5, 4,  // nice 10 .. 14
  2, 2, 1, 1, 1,  // nice 15 .. 19
};

#endif  // NICE_WEIGHTS_H
