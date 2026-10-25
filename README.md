
# Elara

## Introduction

Scx_elara is a multipurpose scheduler which was developed with ananicy in mind.

It is an earliest deadline first scheduler with one queue per cpu: the nice value
decides both how soon a task gets on the cpu and how much cpu time it gets when it
keeps running, like with CFS/EEVDF, but with a steeper curve: each nice step is a factor
of 1.5 instead of ~1.25. So a game at nice -5 gets ~88% of a core against a nice 0 task
(CFS: 75%), and the usual levels (audio and compositor at -12, games at -5, normal tasks
at 0, background work at 5 and higher) are well apart without ever locking anything
out.

## Dependencies

```
cmake (3.20 or newer) clang lld pkgconf libbpf bpf
```

kernel compiled with flag `CONFIG_DEBUG_INFO_BTF=y`

Linux 6.18 or newer. 6.19 or newer is recommended, 6.18 loses a part of the realtime
handling, 7.1 or newer hands tasks preempted by realtime tasks straight back (see
below).

Building needs the `include/vmlinux.h` of Linux 7.3 or newer (the scx headers in
`include/` use its types). The built scheduler runs on older kernels as well, it checks
at load time what the running kernel supports.

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

Every task has a weight from its nice value (`source/nice_weights.h`, 1024 at nice 0, a
factor of 1.5 per nice step) and a slack: `SLACK_NICE_0_NS` (3ms) scaled by
1024 / weight, limited to 20us .. 500ms.

| Nice | Weight | Slack | Cpu share against a nice 0 task on one core (CFS) |
| ---- | ------ | ----- | ------------------------------------------------- |
| -12 | 132860 | ~23us | ~99% (94%) |
| -5 | 7776 | ~0.4ms | ~88% (75%) |
| 0 | 1024 | 3ms | 50% (50%) |
| 5 | 135 | ~23ms | ~12% (25%) |
| 10 | 18 | ~170ms | ~2% (9%) |
| 19 | 1 | 500ms | ~0.1% (1%) |

The factor is steeper than CFS on purpose: with the CFS table a game at nice -5 lost a
lot of its share as soon as several nice 0 tasks (video decoding, a compile) ran next to
it, and its frame times got much less even (tests in a VM with a game-like workload,
network traffic, two livestreams and a compile: frame time spread p1..p99 13-15ms
instead of 19-20ms). A factor of 2 was worse again: the slack of nice -5 gets so short
that the threads of the game keep interrupting each other.

Per-cpu kernel threads (kworker/N, ksoftirqd/N, ...) get the shortest slack (20us),
they are the only ones that can do the work of their cpu. SCHED_IDLE tasks get the
longest (500ms).

Each queued task has two deadlines:

- its key, `vtime + slack`: the order of its cpu's queue. The vtime is the cpu time
  the task used, scaled by 1024 / weight, so it advances slower for tasks with a lower
  nice value. Ordering by it makes tasks that keep running share a cpu by their weight
  (this is the virtual deadline of EEVDF).
- its deadline, `queue time + slack + lag` in ns: it is the same clock on every cpu,
  so it is used whenever tasks of different cpus are compared. The lag is the vtime of
  the task minus the vtime reference of its cpu (the highest vtime that started running
  there), limited to ±20ms (`LAG_LATE_MAX_NS`) scaled by 1024 / weight: a task with a
  low weight keeps the debt of its last slice when it moves to another cpu, and keeps
  what it is owed when it had to wait. A task that wakes up is at most 1ms
  (`LAG_EARLY_MAX_NS`, one slice) behind: sleeping doesn't earn cpu time. (Limiting
  every task to that made the slacks decide the shares instead of the weights: a nice
  5 task next to a nice 0 task got 6% instead of 12%.)

A woken task gets half its slack (`WAKE_SLACK_DIV`), otherwise it would land behind
every task of the same nice value that keeps running.

There are no bands or starvation budgets. The shares alone can still let a task wait
very long (a SCHED_IDLE task next to a nice -20 task that keeps running would get a
slice every ~50s, and the sched_ext watchdog stops the scheduler after 30s), so the
deadline is also a promise: every 10ms (`STARVE_CHECK_INTERVAL_NS`) a cpu looks at its
queue for a task more than 500ms (`STARVE_GRACE_NS`) past its deadline and runs it
first. It looks from the end of the queue (at most 32 tasks): the tasks that wait
longest have the latest keys, so with many queued tasks they would be out of reach from
the front. With normal shares this hardly ever happens.

## Preemption

Every task runs for slices of 1ms (`SLICE_NS`). A task that can't preempt the running
one (because that one is about as urgent) waits at most that long.

The kernel itself only ends a sched_ext slice at the scheduler tick: the slice counts
down while the task runs, but the task is only switched out at the first tick after it
reached 0. With HZ=1000 a slice of 1ms really lasted 1-2ms, with HZ=300 up to
more than 4ms. So elara ends slices on time itself: when a slice starts while other tasks
wait for the core, or when a task is queued behind the running one later, a per-core
timer (`slice_timers`) is set to the end of the slice and preempts the running task
then. A task queued behind a running task that is already past its slice preempts it
right away. When nothing waits, no timer is set and the task just keeps running.

While an RT task runs on some core, the timer is set on the other cores too: tasks
stuck in the queue of the RT core are only taken over when another core's slice ends.
Tasks the kernel queues by itself, without asking elara (exiting tasks, tasks with
migration disabled), come with the kernel's default slice of 20ms; it is cut down to
`SLICE_NS` when they start running.

A waking task preempts the running task of its cpu when its key is earlier by more than
250us (`PREEMPT_MARGIN_NS`). If it is also the earliest of the queue, it goes straight
into the local queue of the cpu with `SCX_ENQ_PREEMPT`, so the running task is preempted
right away. A kick (`SCX_KICK_PREEMPT`) only takes effect through irq_work, which made a
woken task wait about 1ms longer. The preempted task goes back into its queue with its
key, deadline and the rest of its slice. When several tasks wake for the same cpu before
it switched, only an earlier one replaces the task already on its way in (on 7.1+ the
kernel hands the replaced one back to elara), a later one queues up behind it.

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

While boosted, the cpu time of the task is charged to its vtime with the weight of the
waker, not its own. Otherwise a light task (a nice 0 audio client woken by a nice -12
audio server) would use up its turn within its first slice and then wait behind every
task of a higher weight anyway: under a nice -5 load on every core such a client doing
1.5ms of work every 5ms missed its deadline 1390 of 1600 times, now 333 (EEVDF: 604).

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
tasks (running and queued; a core taken by a realtime task counts as 2^32, heavier than
any task, `RT_CPU_WEIGHT`), if that one is lighter than its own core by more than half its own
weight. Counting tasks instead of weight would leave two nice -20 tasks sharing one core
while a nice 0 task has another core to itself. The task that runs next on the own
core is already in its local queue at that point, which elara can't look into: its
weight is remembered when dispatch picks it (counting it as nice 0 kept a light task
next to a heavy one while another core had only light tasks).

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
which also keeps count of the cores taken by one (`nr_rt_busy`). An idle core is woken
right away for the first task waiting in the queue of that core.

On Linux 7.1 and newer elara sets `SCX_OPS_ALWAYS_ENQ_IMMED`: a task put into the local
queue of a core (the one or two tasks the kernel runs next there) either runs right
away or the kernel hands it back to elara, it never waits there. So a task preempted by
a realtime task comes back at once, with its key, its deadline and the rest of its
slice. If an allowed core is idle it continues there right away. Otherwise it goes to
the front of the queue of its core: most realtime tasks (irq threads, audio, the
compositor) are done in a moment and it just continues, and while the core stays taken
the other cores take it over as soon as it is their earliest task (see Dispatch). A
task the kernel can't run right away for another reason (two wakeups picked the same
idle core, a realtime task woke up on the core it was picked for) comes back the same
way, keeps its deadline and is placed again.

Older kernels leave a preempted task at the head of the local queue, out of reach of
the other cores. There, if the realtime task keeps the core for 250us
(`RT_EVACUATE_DELAY_NS`), the tasks already picked to run next on it (the task it
preempted is the first of them) go back through placement. A preempted task that would
resume on a core taken by a realtime task goes through placement too.

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
   next task (or it has none). Then the same for cores of other llcs, so a task that may
   only run on its realtime core and on cores of another llc isn't stuck there.
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

A cpu going offline stops running tasks a while before the kernel stops the
scheduler, and in between the kernel waits for an RCU grace period. Tasks still
queued on that cpu would wait for good: with the RCU kthread among them, the whole
system stalled for 20s until the kernel's RCU stall detection ejected elara. So a
cpu whose queue doesn't move (tasks queued, but none started or kept running for
4 slices, `STUCK_NS`, counted from when its queue got its first task) counts as stuck: the other cpus take its tasks like those of a
cpu taken by a realtime task, and placement treats it as busy. Each cpu looks for
stuck cpus every 4ms. This also covers a cpu held by long interrupt work.

## Testing

There where 2 design goals for this scheduler.

1. That music keeps playing normally when executing the cachyos benchmarker https://github.com/CachyOS/cachyos-benchmarker
2. To keep frametimes as smooth as possible with as little frametime spikes as possible. 

As far as i have tested. Both modes do accomplish these tasks very well.

The only problem is i couldn't test the functionality with different llcs as i don't have such an cpu by hand.
The next thing is, that i mostly developed this scheduler with SMT disabled. As i found that SMT off works the best for this ryzen 5800x3d. But you can test both. Your mileage may vary.
