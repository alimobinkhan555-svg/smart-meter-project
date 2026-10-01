# Architecture and design notes

This document explains *why* the system is built the way it is. The README covers what
it does; this covers the reasoning, the concurrency model and the trade-offs.

---

## 1. The problem

A smart energy meter (IEC 62053-21 class) emits a pulse train: one short pulse every time a
fixed amount of energy has been consumed, typically 1 Wh. The information content of that
signal is a single monotonically increasing integer — the **totaliser**.

The engineering problem is therefore not "what does the data mean" but "how do you turn a
noisy, timing-critical hardware signal into that integer without losing a single edge, and
then do something useful with the resulting time series."

Two rules follow from that, and they dictate the whole architecture:

1. **Edge counting must happen in a context that cannot be starved.** User space can be
   descheduled, blocked on a disk, or page-faulting. Kernel interrupt context cannot.
2. **Everything else belongs in user space.** Analytics, logging, formatting and UI are
   untrusted, bug-prone and slow. Putting them in the kernel buys nothing and risks the
   machine.

Hence the kernel/user split.

---

## 2. Kernel space

### 2.1 Why a character device

A character device is the right interface because the data is a *stream of events*, not a
file with content:

| Kernel concept | Where it is used | Why |
|---|---|---|
| `misc_register` | `/dev/smart_meter` | No static major/minor allocation; udev creates the node automatically |
| `file_operations` | `open/read/write/poll/ioctl/release` | Standard VFS dispatch, familiar to every Linux systems programmer |
| `poll` | `sm_poll` | Lets a reader **block** until pulses arrive instead of spinning |
| `ioctl` | config/stats/reset | Control plane kept out of the data path |
| `atomic64_t` | `pulse_count`, `sequence` | Lock-free increments from ISR and process context |
| `mutex` | `sm_dev.lock` | Only around reconfiguration; never in the ISR |
| `request_irq` | `sm_gpio_isr` | Hardware edge detection, rising edge |
| `hrtimer` | `sm_sim_timer_fn` | Nanosecond-resolution virtual pulse generator |
| `atomic64` + `__atomic` | shared-memory ring (user space) | Cross-process lock-free publication |

### 2.2 The ISR discipline

```c
static irqreturn_t sm_gpio_isr(int irq, void *data)
{
        atomic64_inc(&sm_dev.irq_count);
        sm_record_pulses(1);          /* one atomic_inc + one atomic_add + ktime_get_ns */
        return IRQ_HANDLED;
}
```

This is the only code that runs per pulse, and it is deliberately trivial. Nothing that can
sleep, allocate, or take a lock appears in interrupt context. Consequences:

* **No pulse loss.** The counter is updated before the ISR returns; user space may be
  arbitrarily behind and still get a correct cumulative total when it next reads.
* **Bounded ISR latency.** The work is O(1) and independent of the number of open
  descriptors or the analytics workload.
* **ktime_get_ns() is safe** in interrupt context because it reads `CLOCK_MONOTONIC`, which
  never takes the timekeeper seqlock in a way that can sleep.

For 1 pulse/second this is nothing. At the meter's theoretical maximum (12.5 Hz at 50 A,
50 Hz) it is still ~80 µs of work per second.

### 2.3 Why `read()` returns a snapshot, not a byte stream

An obvious alternative is a stream of 1 byte per pulse. It is worse:

* a partial `read()` would lose events, because the "events" are really deltas of a counter;
* userspace would have to re-derive the total, and would have no idea how many pulses it
  had missed while blocked.

Instead, `read()` returns the whole `struct sm_sample`. That makes the interface
**lossless by construction**: reading is idempotent, and a reader that is late simply reads
a larger jump. `sequence` lets the application detect that it happened.

The snapshot is copied with a **single `copy_to_user()`**, so it is atomic with respect to
other readers for a single-meter device. There is no window in which `sequence` and
`pulse_count` disagree.

### 2.4 `poll()` instead of busy-waiting

`sm_poll` reports `POLLIN` when `sequence != consumed_sequence`. A UI polling at 4 Hz
consumes almost no CPU, and a reader that wants event-driven behaviour blocks in `poll()`
and is woken by the producer. The `poll_wait()` registration is mandatory — without it a
blocking `poll()` would never be woken.

### 2.5 The simulated source

The `hrtimer` source exists so the whole system is demonstrable and testable without
hardware:

```c
period_ns = (u64)NSEC_PER_SEC * 3600ULL / (u64)sm_dev.config_pph;
hrtimer_start(&sm_dev.sim_timer, ns_to_ktime(period_ns), HRTIMER_MODE_REL);
```

`hrtimer_forward_now()` is used rather than `HRTIMER_RESTART` from an absolute expiry so
that the callback's own execution time does not accumulate as drift. The source can be
switched at runtime through `SM_IOC_SET_CONFIG` or `echo 0 > .../running`, and the
per-source tallies (`simulated_count`, `irq_count`, `injected_count`) make it obvious in
`/proc/smart_meter` which path is actually producing data.

### 2.6 Teardown ordering

Module removal order matters and is easy to get wrong:

```
sm_sim_stop()        → no new pulses can be generated
free_irq/gpio_free   → no new pulses from hardware
sm_wake_readers()    → blocked poll() calls return instead of hanging on a dead device
sysfs/procfs remove  → no user can observe freed state
misc_deregister()    → /dev/smart_meter disappears last
mutex_destroy()
```

If `hrtimer_cancel()` or `free_irq()` were skipped, the callback could run against a
partially destroyed device — the classic use-after-free in drivers.

---

## 3. User space

### 3.1 Layering

```
main.cpp            dashboard, menu, argument parsing, IPC wiring
   │
   ├── PulseSource      device | simulated | file      (abstract)
   ├── Collector        pthread: read → RawSample
   ├── LoggerWorker     pthread: RawSample → Reading → CSV
   ├── AnalyticsEngine  pure computation, no I/O, no globals
   ├── CsvLogger/CsvReader
   ├── csv_export       CSV + JSON + gnuplot
   └── ipc_channel      shm ring + message queue
```

`AnalyticsEngine` is deliberately a pure function of its input history. It has no file
handles, no clock calls, no globals, and therefore no locking — and it is directly unit
testable, which `tests/selftest.cpp` relies on.

### 3.2 The pipeline and its back-pressure

```
PulseSource ─► Collector thread ─► BoundedQueue<RawSample> ─► LoggerWorker thread ─► BoundedQueue<Reading> ─► main
               (poll + read)          capacity 4096              (delta, energy, CSV)     capacity 4096      (analytics, render)
```

Three separate stages, each on its own thread, because they have very different costs:

| Stage | Typical cost | Failure mode if merged |
|---|---|---|
| read | ~10 µs | none |
| compute + write | ~100 µs – 10 ms (disk!) | a slow disk stalls acquisition and pulses queue up in the kernel's unread buffer |
| render | ~1 ms (terminal I/O) | a redraw stalls everything |

The queue is **bounded** on purpose. An unbounded queue would convert a slow disk into an
out-of-memory kill; a bounded queue converts it into back-pressure: the collector blocks
once 4096 samples are pending. Since samples are *cumulative* snapshots, blocking loses
nothing — the next sample still contains every pulse that occurred meanwhile. This is a
direct benefit of the snapshot design in section 2.3.

### 3.3 Why the main thread owns the engine

The main thread drains the results queue into `AnalyticsEngine` exactly once per iteration
and then renders. Because there is exactly one writer and it is the same thread that reads,
the engine needs no mutex. The alternative — letting the logger thread update the engine —
would force a lock around every statistics call and make the dashboard's read-modify-render
cycle racy.

### 3.4 Threading library

`std::thread` is used, which is the C++11 spelling of `pthread_create`; the binary links
with `-pthread`. The pipeline maps 1:1 onto pthreads:

| Thread | Entry point | Role |
|---|---|---|
| main | `main()` | render, menu, analytics |
| worker 1 | `Collector::threadMain` | `poll()` + `read()` on the device |
| worker 2 | `LoggerWorker::threadMain` | CSV write |
| (optional) | `SimulatedPulseSource::emitterLoop` | virtual pulse generator |

The simulated source adds a fourth thread only when the driver is not loaded.

### 3.5 Statistics

* **Energy**: `energy_wh = pulse_delta / pulses_per_wh`, default `1 pulse = 1 Wh`.
* **Deltas**: `pulse_count - previous`. If the driver counter goes *backwards* (module
  reloaded, `SM_IOC_RESET`, device unplugged) the engine rebases instead of wrapping to a
  huge unsigned value. This is a real failure mode that a naive implementation gets wrong.
* **Buckets**: `epoch / 3600` and `epoch / 86400` — UTC aligned, so a run that crosses
  midnight splits correctly. `gmtime_r` + `timegm` avoids the local-time and DST ambiguity
  of `mktime`.
* **Anomalies**: rolling window of the previous 20 samples; the current sample is excluded
  from its own baseline so a spike cannot mask itself. Z-score ≥ 3 flags an anomaly, and
  when σ = 0 (perfectly flat baseline) the test falls back to `value ≥ 2 × mean` instead of
  dividing by zero.
* **Peak**: three answers, because "peak usage" is ambiguous — the worst single sample, the
  worst hour, and the busiest hour-of-day averaged across days. The hour-of-day profile is
  the one that actually tells a user *when* to run the dishwasher.

### 3.6 Logging

* One row per sample: `timestamp,pulse_count,energy_consumed`.
* ISO-8601 UTC, so the file sorts lexicographically and needs no timezone column.
* Writes are **batched** (up to 64 rows or 250 ms) — an `fsync` per sample would dominate
  the runtime at 1 pulse/second sampling.
* `#` comment lines carry configuration metadata and are skipped by the reader.
* On restart the logger reads the last row and uses it as the delta baseline, so appending a
  new session never produces a bogus "everything at once" first record.
* The engine also loads the whole log at startup, so the dashboard shows history from the
  moment it opens rather than from the moment it was launched.

### 3.7 The `PulseSource` abstraction

The dashboard contains no `#ifdef __linux__`, no `/dev/smart_meter` string and no assumption
that a driver is loaded. Three implementations satisfy one interface:

| Implementation | Used for |
|---|---|
| `DevicePulseSource` | production |
| `SimulatedPulseSource` | demos, CI, machines without `CAP_SYS_MODULE` |
| `FilePulseSource` | regression testing against recorded history |

`--source auto` picks the device if the node exists, else the simulator, and says so on
stderr. This is what makes the project demonstrable in a container, on a laptop, or in a
grading environment where `insmod` is not permitted.

### 3.8 IPC

Two mechanisms, chosen for two different jobs:

* **Shared memory ring** for the pulse stream. `shm_open` + `ftruncate` + `mmap`, with
  independent `write_index`/`read_index` advanced by `__atomic_*` builtins (valid across
  processes because the page is naturally aligned and the operations are lock-free). When
  the ring is full the producer recycles the oldest slot and increments `dropped` — the
  dashboard can therefore *tell* the user data was lost instead of silently skipping it.
* **POSIX message queue** for commands (`reset`, `rate=1800`, `pause`, `resume`, `status`).
  Here delivery guarantees matter more than throughput, and `mq_send`/`mq_timedreceive`
  give blocking-with-timeout semantics for free.

`ipc/sm_ipc_client.c` is a pure C client, which proves the layout is genuinely
language-agnostic and not an accident of the C++ implementation.

---

## 4. Data flow, end to end

1. A pulse edge arrives on the GPIO line (or the hrtimer fires).
2. The ISR increments `atomic64_t pulse_count` and `sequence`, and stamps `last_pulse_ns`.
3. The `Collector` thread's `poll()` returns `POLLIN`.
4. `read()` copies one `sm_sample` into the thread's stack; `copy_to_user` publishes it.
5. The `Collector` pushes a `RawSample` onto the bounded queue.
6. `LoggerWorker` pops it, computes `pulse_delta` and `energy_wh`, appends a CSV row, and
   pushes a `Reading` onto the results queue.
7. The main thread drains the results queue into `AnalyticsEngine`, recomputes statistics,
   detects spikes, and redraws the dashboard.
8. If `--publish` is set, each `Reading` is also written into the shared memory ring for
   other processes.

---

## 5. Error handling

| Failure | Handling |
|---|---|
| `poll()` returns `-1/EINTR` | treated as "no data", loop continues |
| `poll()` timeout | no error: the UI simply redraws unchanged |
| `read()` short | reported as an error, not silently accepted |
| device node missing | `auto` falls back to the simulator with a warning |
| `open()` fails (permissions) | error message names the path and `errno` |
| driver counter goes backwards | engine rebases the baseline (section 3.5) |
| CSV write fails | error count incremented, thread keeps running |
| shm ring full | oldest slot recycled, `dropped` counter incremented |
| mq full | `mq_send` returns `EAGAIN`, reported to the sender |
| SIGINT/SIGTERM | `sig_atomic_t` flag, threads joined, log flushed, IPC detached |
| malformed CSV line | skipped and counted, reported to the user |

---

## 6. Known limitations

* **One meter per module instance.** The driver uses a single global `sm_dev`. Supporting
  several meters means one instance per device, which on modern kernels means a proper
  platform driver (or a `misc` device per meter with a lookup table).
* **`poll()` is edge-triggered in spirit but level-triggered in practice**: it reports
  "something new since the last read", not "one pulse". This is intentional and documented.
* **No rate limiting on `/dev/smart_meter` writes.** A buggy user-space program can inflate
  the counter at will. The `injected_count` tally makes it visible.
* **The GPIO path uses the legacy GPIO API.** It is stable and widely used, but from 6.5 the
  preferred interface is `gpiod`. The driver guards the direction call with a kernel version
  check; converting to `gpiod` properly means becoming a platform driver with
  `devm_gpiod_get_index()`.
* **The dashboard redraws the whole screen.** Adequate for a terminal, not for a
  60 fps animation.
* **CSV does not scale past a few million rows.** The export path exists for that reason;
  SQLite is the natural next step.

---

## 7. What was deliberately left out

* **A userspace `netlink` or `uevent` notification path.** `poll()` covers the requirement
  with far less machinery.
* **A custom kernel `proc_ops` per-CPU counter array.** One meter, one counter.
* **Floating-point accumulation in the kernel.** The kernel stores integers only; all
  floating-point work happens in user space where it is safe to be wrong.
* **Blocking writes to user space from the ISR.** The classic "push model" that loses data
  when the buffer is full. The pull model (`read()` a snapshot) is strictly better here.
