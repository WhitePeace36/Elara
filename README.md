
# Elara

## Introduction

Scx_elara is a multipurpose scheduler which was inspired partly by the windows prio system and was developed with ananicy in mind.

But you will have to adjust the default ananicy profiles because they are set with nice being cpu time and not with it being prio.
So preconfigured stuff will not fit great with this scheduler.

## Dependencies

```
cmake clang pkgconf libbpf bpf
```

kernel compiled with flag `CONFIG_DEBUG_INFO_BTF=y`

Linux 6.18 or newer. 6.19 or newer is recommended, 6.18 loses a part of the realtime
handling (see below).

for the kernel option you can just check if `/sys/kernel/btf/vmlinux` is present.

But this kernel option should be enabled by default, but not bad to check never the less.

## Important

The nice value decides the band of a task (see below). It does not change the slice or
the share of cpu time, it only decides which band is served first.

So nice values are the way to tell elara what is important. Tools like ananicy-cpp can
be used for that, for example to put audio into band 0, a game into band 1 and
background work into band 3 or 4.

Audio (pipewire, pipewire-pulse, wireplumber) should run with realtime priority or at
least with nice -11 or lower, so it lands in band 0 and nothing else can delay it.


## Building it

```
./build.sh
```

## Installing

```
sudo ./install.sh
```

## Uninstalling

```
sudo ./uninstall.sh
```

## Explanation

### Bands

Every cpu has one queue per band. The band is chosen by the nice value:

| Band | Nice |
|---|---|
| 0 | -20 to -11 |
| 1 | -10 to -3 |
| 2 | -2 to 2 |
| 3 | 3 to 10 |
| 4 | 11 to 20 |


| Band | Example of tasks to put here |
|---|---|
| 0 | xwayland, kwin, pipewire, ... |
| 1 | game or other programm which you mainly intend to use when open |
| 2 | most default applications  |
| 3, 4 | compiling or other stuff which you want to run in the background but you don't want to interrupt your work |

Bands are served strictly in order. Most tasks run with nice 0, so they are in band 2.
Band 0 holds high priority kernel threads (kworker/*H, ...) and everything that is
reniced to -11 or lower.

Kernel threads that are bound to one cpu (ksoftirqd/N, kworker/N:x, rcuc/N, ...) always
go into band 0, whatever their nice value. Only they can do the work of their cpu, so
they should never wait behind anything. Unbound kernel threads (kworker/u*) and user
tasks pinned to one cpu are handled by their nice value like everything else.

Tasks with the policy SCHED_IDLE (`chrt -i`, `CPUSchedulingPolicy=idle`) always go into
band 4, whatever their nice value: they asked to only run when nothing else wants the
cpu.

Every task has a slice of 1ms. Nice values don't change the slice.

### vtime inside a band

Inside a band the queue is ordered by vtime. Every task collects the cpu time it used,
and the task that used the least runs first.

Every cpu keeps a reference per band: the highest vtime that started running in that
band on this cpu. When a task is queued, it gets

key = reference + lag

lag is the lead (negative) or debt (positive) the task has against the reference,
limited to +-1 slice. While a task sleeps the reference keeps moving, so sleeping pays
off debt, but a task can never collect more than one slice of lead. That way a task
which slept for a long time can't come back and hog the cpu (no lag bombs), and a task
that runs rarely and shortly is always near the front of its band.

A new task starts exactly at the reference, without lead or debt.

When a task is moved to another cpu it keeps its lag against the reference of the new
cpu.

## Preemption

A waking task of a higher band preempts a running task of a lower band. The preempted
task goes back into its queue with its vtime and the rest of its slice.

Inside the same band there is no preemption. A waking task waits until the slice of the
running task ends (at most 1ms) and is then served by its vtime key, so a task that used
little cpu still goes first.

When the slice of a task runs out, its vtime is updated. It keeps running for another
slice if its band is better than every queued band on its cpu, or if in the same band
its key is not later than the one of the first queued task. Otherwise it goes back into
its queue.

## Wake boost

When a task wakes a task of a lower band, the woken task runs in the band of the waker
until it goes to sleep again, for at most 4ms of cpu time (`WAKE_BOOST_BUDGET_NS`).
After that it is back in its own band. That way work a task waits for (a helper thread,
wineserver, a kworker that submits its gpu job, ...) runs right away instead of behind
everything in between. A task that keeps running gets no advantage beyond the budget: it
is only boosted again after it has slept and is woken again.

The budget is several slices on purpose. With a boost of only one slice, a woken task
that needs a bit more than that falls back to its own band with its work unfinished.
Behind a busy better band it then only runs on the starvation override (one slice per
50ms in band 2), its next requests pile up while it waits, so it never sleeps and is
never woken (and boosted) again.

Only wakeups from normal task context count. A wakeup from an interrupt runs on top of
whatever task was interrupted, and that task is not the waker. Wakeups by kernel threads
don't boost either: per-cpu kworkers and ksoftirqd are in band 0 and wake ordinary
processes for every finished disk read and network packet. This can be changed with
`WAKE_BOOST_FROM_KTHREADS` in `source/defines.h`.

## Placement and balancing

When a task wakes up and an idle core is found, it runs there directly.
Otherwise the task goes to the queue of the core with the least work ahead of it:

- an idle core is always preferred
- tasks of band 0 and 1 check all cores of the same llc
- tasks of band 2, 3 and 4 compare their core with 2 random cores of the same llc
  and move at most once every 10ms

Idle cores only look for work when they are woken up. So when a core starts running a
task while other tasks still wait in its queues (for example the task it just
preempted), it wakes an idle core that is allowed to run the first waiting task, and
that core takes it over.

### Realtime and deadline tasks

Tasks with SCHED_FIFO, SCHED_RR or SCHED_DEADLINE (kwin, irq threads, ...) run above
all bands, outside of elara. A core that runs such a task counts as busy for every
band, with a load of 2 tasks (`RT_CPU_LOAD`): a task of our bands gives the core back
after at most one slice, a realtime task only when it is done. The 10ms limit for
moving tasks doesn't apply to tasks whose own core is taken by a realtime task.

Every switch to a realtime or deadline task is seen by a `sched_switch` tracepoint.
The tasks already picked to run next on that core then go back through placement,
which moves them to a core that is free for them (kernel 6.19+), and an idle core is
woken for the first task waiting in its queues. A preempted task that would resume on
a core taken by a realtime task goes through placement too instead of going back to its
old place.

The tasks in the band queues of that core are not placed again: realtime tasks often
run only for microseconds, and moving the queues around on every one of them breaks
the starvation tracking (a task that keeps being moved never counts as starved).

Per-cpu kernel threads with a realtime policy (migration/N, which runs for every
affinity change and task migration) don't count as realtime tasks taking the core:
they only run for microseconds.

Tasks that still wait on such a core are taken over by the other cores: before a core
runs its own best band, it first takes a task of that same band waiting on a core of
its llc that is running a realtime task.

## Dispatch

Each core first runs a starved band if there is one (not on a dispatch caused by a
preemption: the task that preempted runs first). Then it looks at the best band it could
run itself (its running task or its own queues). If a task of a better band waits on
another core of the same llc, it takes that one over, so the bands are strict across
cores too. Otherwise it runs its own band 0, then its own bands 1, 2, 3 and 4. After
that it steals from another core of the same llc and then from cores of other llcs,
band by band. From which core the core starts stealing is randomized for better load
distribution.

## Starvation

If the head of a band has not been served for longer than its budget
(band 1 20ms, band 2 50ms, band 3 100ms, band 4 200ms), it gets one slice (1ms,
`STARVE_OVERRIDE_BUDGET_NS`) of cpu time ahead of the higher bands, at most once every
10ms per core: its tasks run one after another until that time is used up or the band
is empty. One task per override isn't enough, tasks that sleep again right away (like
the rcu kthread) would use it up in microseconds, and a cpu bound task of the band
behind them (kcompactd, an unbound kworker) would never get to run. Inside a band the vtime makes sure that
nothing starves. The values are in `source/defines.h`.

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
