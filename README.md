
# Elara

## Introduction

Scx_elara is a multipurpose scheduler which was developed with ananicy in mind.

It is an earliest deadline first scheduler with one queue per cpu: the nice value
decides both how soon a task gets on the cpu and how much cpu time it gets when it
keeps running, like with CFS/EEVDF. So the default ananicy profiles fit, and lower nice
values for audio, the compositor and games make them both faster and bigger.

## Dependencies

```
cmake (3.20 or newer) clang lld pkgconf libbpf bpf
```

kernel compiled with flag `CONFIG_DEBUG_INFO_BTF=y`

Linux 6.18 or newer. 6.19 or newer is recommended, 6.18 loses a part of the realtime
handling (see below).

for the kernel option you can just check if `/sys/kernel/btf/vmlinux` is present.

But this kernel option should be enabled by default, but not bad to check never the less.

## Important

The nice value decides the slack (how soon a task gets on the cpu) and the weight (how
much cpu time it gets) of a task, see below. Every task gets its turn: a lower nice value
makes a task earlier and bigger, it never locks others out.

So nice values are the way to tell elara what is important. Tools like ananicy-cpp can
be used for that, for example audio and the compositor at nice -12, a game at nice -5
and background work at nice 5 or higher.

## Building it

```
./build.sh
```

`build.sh` regenerates `include/vmlinux.h` from the running kernel, and the BPF part is
rebuilt whenever that file (or any other included header) changed, so after a kernel
update just run it again.

The loader is built for any cpu of the architecture. To build it with `-march=native`
(the binary then only runs on cpus like the build machine), configure with
`-DELARA_NATIVE_ARCH=ON`.

## Installing

```
sudo ./install.sh
```

## Uninstalling

```
sudo ./uninstall.sh
```

## Explanation

### Slack, weight and deadlines

Every task has a weight from its nice value (`p->scx.weight`, 100 at nice 0, the same
table as CFS) and a slack: `SLACK_NICE_0_NS` (3ms) scaled by 100 / weight.

| Nice | Slack | Cpu share against a nice 0 task on one core |
| ---- | ----- | ------------------------------------------- |
| -20 | ~35us | ~99% |
| -12 | ~0.2ms | ~94% |
| -5 | ~1ms | ~75% |
| 0 | 3ms | 50% |
| 5 | ~9ms | ~25% |
| 19 | ~300ms | ~1% |

Per-cpu kernel threads (kworker/N, ksoftirqd/N, ...) get the shortest slack (20us),
they are the only ones that can do the work of their cpu. SCHED_IDLE tasks get the
longest (500ms).

Each queued task has two deadlines:

- its key, `vtime + slack`: the order of its cpu's queue. The vtime is the cpu time
  the task used, scaled by 100 / weight, so it advances slower for tasks with a lower
  nice value. Ordering by it makes tasks that keep running share a cpu by their weight
  (this is the virtual deadline of EEVDF).
- its deadline, `queue time + slack + lag` in ns: it is the same clock on every cpu,
  so it is used whenever tasks of different cpus are compared. The lag is the vtime of
  the task minus the vtime reference of its cpu (the highest vtime that started running
  there), limited to -1ms (`LAG_EARLY_MAX_NS`, the most a task that slept gets ahead)
  and +20ms (`LAG_LATE_MAX_NS`) scaled by 100 / weight, so a task with a low weight
  keeps the debt of its last slice when it moves to another cpu.

A woken task gets half its slack (`WAKE_SLACK_DIV`), otherwise it would land behind
every task of the same nice value that keeps running.

There are no bands or starvation budgets. The shares alone can still let a task wait
very long (a SCHED_IDLE task next to a nice -20 task that keeps running would get a
slice every ~50s, and the sched_ext watchdog stops the scheduler after 30s), so the
deadline is also a promise: every 10ms (`STARVE_CHECK_INTERVAL_NS`) a cpu looks at its
queue for a task more than 500ms (`STARVE_GRACE_NS`) past its deadline and runs it
first. With normal shares this hardly ever happens.

## Preemption

A waking task preempts the running task of its cpu when its key is earlier by more than
250us (`PREEMPT_MARGIN_NS`). The preempted task goes back into its queue with its key,
deadline and the rest of its slice.

When the slice of a task runs out, its vtime is updated. It keeps running for another
slice if its new key is not later than the key of the first queued task.

## Wake boost

When a task wakes a task with a longer slack, the woken task takes over the slack of the
waker until it goes to sleep again, for at most 4ms of cpu time (`WAKE_BOOST_BUDGET_NS`).
That way work a task waits for (a helper thread, wineserver, a kworker that submits its
gpu job, ...) runs right away instead of behind everything in between.

The boosted cpu time of a task is also limited over time: each task has a credit of at
most 4ms that refills at 4ms per 20ms (`WAKE_BOOST_WINDOW_NS`), so at most a fifth of a
core. A boost gets at most what is left of the credit, and with less than 250us left
(`WAKE_BOOST_MIN_NS`) there is no boost.

The waker's own slack counts, not a slack it runs with because it was boosted itself.
Otherwise a boost would pass on to everything the boosted task wakes, and from there on.

Only wakeups from normal task context count. A wakeup from an interrupt runs on top of
whatever task was interrupted, and that task is not the waker. Wakeups by kernel threads
don't boost either: per-cpu kworkers and ksoftirqd wake ordinary processes for every
finished disk read and network packet. This can be changed with
`WAKE_BOOST_FROM_KTHREADS` in `source/defines.h`.

## Placement and balancing

When a task wakes up and an idle core is found, it runs there directly. Otherwise it
goes where it runs soonest: the core with the least work ahead of it (the running task,
unless the woken task preempts it, and queued tasks with an earlier deadline).

A task that keeps running and doesn't keep the cpu after its slice is balanced by
weight instead, like CFS does: it moves to the core with the least weight of other
tasks (running and queued; a core taken by a realtime task counts as 20000,
`RT_CPU_WEIGHT`), if that one is lighter than its own core by more than half its own
weight. Counting tasks instead of weight would leave two nice -20 tasks sharing one core
while a nice 0 task has another core to itself.

For both:

- an idle core is always preferred
- tasks with a slack of at most 1ms (`SCAN_WHOLE_LLC_SLACK_NS`, about nice -5 and
  lower) check all cores of the same llc
- other tasks compare their core with 2 random cores of the same llc and move at most
  once every 10ms

Busy cores don't take tasks from each other in dispatch, so tasks stay on their core and
its caches.

Offline cores (for example the second threads with SMT off) are never used.

### Realtime and deadline tasks

Tasks with SCHED_FIFO, SCHED_RR or SCHED_DEADLINE (kwin, irq threads, ...) run above
all of elara's tasks. A core that runs such a task counts as busy, with a load of 2
tasks (`RT_CPU_LOAD`).

Every switch to a realtime or deadline task is seen by a `sched_switch` tracepoint,
which also keeps count of the cores taken by one (`nr_rt_busy`). The tasks already
picked to run next on that core then go back through placement, and an idle core is
woken for the first task waiting in its queue. A preempted task that would resume on a
core taken by a realtime task goes through placement too.

Tasks waiting in the queue of such a core are taken over by the other cores of its llc
(see Dispatch): that core can't run them until the realtime task is done, so they lose
nothing by moving. Their deadlines stay while the deadlines on the other cores move on,
so a task stuck behind a realtime task is taken over at the latest once it is the
earliest one around.

The per-cpu stopper threads (migration/N, which run for every affinity change and task
migration) don't count as realtime tasks taking the core: they only run for
microseconds. Other per-cpu kernel threads with a realtime policy (irq threads bound to
the core of their irq, rcuc/N, ...) do count, they can run for a long time (threaded
NAPI, irq storms).

## Dispatch

When a core needs its next task:

1. If a core runs a realtime task (`nr_rt_busy`): the earliest task waiting on a core
   of its llc taken by a realtime task, if its deadline is earlier than the core's own
   next task (or it has none).
2. A task of its own queue more than 500ms past its deadline (see above).
3. Its own next task: the running task for another slice or the first one of its queue,
   by key.
4. With nothing of its own: the task with the earliest deadline on the other cores of
   its llc. The scan starts at a random core, so of tasks with the same deadline a
   random one is taken.
5. Then the task with the earliest deadline on cores of other llcs.

From another core only tasks count that may run on this one (affinity, not
migration-disabled), and of those the first in that core's queue (at most
`STEAL_SCAN_DEPTH` tasks are looked at).

## CPU hotplug

When cpus go online or offline (for example when toggling SMT) the kernel stops
the scheduler, and elara restarts itself with the new topology.

## Testing

There where 2 design goals for this scheduler.

1. That music keeps playing normally when executing the cachyos benchmarker https://github.com/CachyOS/cachyos-benchmarker
2. To keep frametimes as smooth as possible with as little frametime spikes as possible. 

As far as i have tested. Both modes do accomplish these tasks very well.

The only problem is i couldn't test the functionality with different llcs as i don't have such an cpu by hand.
The next thing is, that i mostly developed this scheduler with SMT disabled. As i found that SMT off works the best for this ryzen 5800x3d. But you can test both. Your mileage may vary.
