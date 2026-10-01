# Setup guide

Everything below assumes a Debian/Ubuntu or Fedora style Linux host. Commands that touch
the kernel are marked **sudo**.

---

## 1. Prerequisites

```bash
# Debian / Ubuntu
sudo apt update
sudo apt install build-essential linux-headers-$(uname -r) git gnuplot

# Fedora / RHEL
sudo dnf install gcc gcc-c++ make kernel-devel git gnuplot
```

Verify:

```bash
gcc --version
g++ --version
make --version
ls /lib/modules/$(uname -r)/build    # must exist
```

If `/lib/modules/$(uname -r)/build` is missing, install the headers package matching the
*running* kernel. Building a module against a different kernel than the one running will
compile and then fail to load.

---

## 2. Build the user-space application

No root is required for this step, and the resulting binary works without the driver.

```bash
cd smart-meter-project
make
```

Output: `build/app`.

Manual equivalent:

```bash
g++ -std=c++17 -O2 -Wall -Wextra -pthread -Iinclude user_app/*.cpp -o app -lrt
```

Optional targets:

```bash
make ipc-client     # build/ipc/sm-ipc-client  (POSIX C IPC helper)
make test           # build/selftest, then run it
```

Quick smoke test without any hardware:

```bash
./build/app --demo
```

Expected: ~30 s of progress lines, an injected 600-pulse spike near the end, a statistics
report, an anomaly report, and `logs/pulses.csv`.

---

## 3. Build and load the kernel driver

### 3.1 Build

```bash
cd driver
make
```

Expected: `smart_meter_driver.ko`. To build against an explicit kernel tree:

```bash
make KDIR=/lib/modules/6.1.0-18-amd64/build
```

Troubleshooting a build failure:

| Message | Cause | Fix |
|---|---|---|
| `No rule to make target .../scripts/Makefile.build` | wrong/absent kernel tree | install `linux-headers-$(uname -r)` or set `KDIR` |
| `implicit declaration of 'sysfs_emit'` | kernel < 5.10 | the driver ships a `sprintf` shim; a hard error means the header set is partial |
| `'noop_llseek' undeclared` | kernel 6.12+ | the driver has a `no_llseek`/`noop_llseek` shim |

### 3.2 Load

```bash
sudo make install        # optional: copies into /lib/modules/$(uname -r)/extra
cd ..
sudo insmod driver/smart_meter_driver.ko simulate=1 pulses_per_hour=1800
```

Or from the project root:

```bash
sudo make load SIMULATE=1 PULSES_PER_HOUR=1800
```

Verify the node exists:

```bash
ls -l /dev/smart_meter
# crw------- 1 root root 10, 122 /dev/smart_meter
```

Confirm the kernel side is counting:

```bash
cat /proc/smart_meter
```

```
smart_meter driver 1.0.0
pulse_count       : 412
sequence          : 412
interrupt_count   : 0
simulated_count   : 412
injected_count    : 0
source            : simulated
pulses_per_hour   : 1800
wh_per_pulse      : 1
enabled           : 1
uptime_ms         : 123456
open_fds          : 0
```

### 3.3 sysfs

```bash
ls /sys/kernel/smart_meter/
# injected_count  irq_count  pulse_count  pulses_per_hour  running  simulated_count  source

cat /sys/kernel/smart_meter/pulse_count

# pause and resume the counter
echo 0 | sudo tee /sys/kernel/smart_meter/running
echo 1 | sudo tee /sys/kernel/smart_meter/running
```

### 3.4 Unload

```bash
sudo rmmod smart_meter_driver
# or: sudo make unload
```

---

## 4. Run the dashboard

```bash
./build/app                      # interactive, --source auto
```

The dashboard menu:

```
 1) refresh        2) statistics     3) hourly usage   4) daily usage
 5) peak usage     6) anomalies      7) inject pulses  8) set rate
 9) start/stop     10) export data   11) tail CSV log  12) reset counters
13) load CSV log   14) send IPC cmd    0) quit
```

Useful actions while it runs:

* **7) inject pulses** — write(2) into the driver, so the counter jumps. Try `3600` to
  simulate one hour of consumption instantly.
* **8) set rate** — `ioctl(SM_IOC_SET_CONFIG)`; change the simulated rate live.
* **9) start/stop** — pause the acquisition thread without tearing anything down.
* **10) export data** — writes `<base>_samples.csv`, `<base>_summary.json` and a gnuplot
  script.
* **14) send IPC cmd** — sends `reset`, `rate=1800`, `status`, `pause` or `resume` over the
  POSIX message queue.

Headless runs:

```bash
./build/app --run-seconds 600 --log logs/6h.csv      # 10 minutes, no UI
./build/app --demo                                    # scripted 30 s demo
./build/app --source file --replay samples/sample_history.csv
```

---

## 5. Real hardware (GPIO)

1. Wire the meter pulse output through an optocoupler and a Schmitt-trigger buffer
   (e.g. 74HC14). **Do not connect a meter output to a GPIO pin directly** — the isolation
   and level shifting matter.
2. Find the GPIO number (`gpioinfo`, or `/sys/class/gpio/`).
3. Load with the GPIO source:

```bash
sudo insmod driver/smart_meter_driver.ko simulate=0 gpio_pin=17
dmesg | tail -5
# smart_meter: listening on GPIO 17 (IRQ 331), rising edge
```

4. Verify `interrupt_count` rises while `simulated_count` stays at 0:

```bash
watch -n1 'cat /sys/kernel/smart_meter/{pulse_count,irq_count}'
```

5. Calibrate: count pulses over a known interval and set `--pulses-per-wh` accordingly
   (e.g. 4000 pulses = 1 kWh → `--pulses-per-wh 0.00025`).

---

## 6. IPC demonstration

Two terminals.

Terminal 1 — producer:

```bash
./build/app --source sim --publish --interval-ms 500
```

Terminal 2 — consumer:

```bash
./build/app --ipc --shm /smart_meter_pulses
```

Terminal 3 — inspect or inject from a pure C client:

```bash
./build/sm-ipc-client status
./build/sm-ipc-client consume 10
./build/sm-ipc-client publish 250
./build/sm-ipc-client cmd "rate=7200"
```

Clean up the shared objects:

```bash
rm -f /dev/shm/smart_meter_pulses
rm -f /dev/mqueue/smart_meter_cmd
```

---

## 7. Visualisation

After a run with `--export`:

```bash
./build/app --demo --export exports/demo
gnuplot exports/demo.gp            # writes exports/demo_samples.csv.png
```

Or in Python-free tooling:

```bash
cut -d, -f1,3 exports/demo_samples.csv | head
```

---

## 8. Troubleshooting

| Symptom | Likely cause | Fix |
|---|---|---|
| `open(/dev/smart_meter) failed: No such file or directory` | module not loaded | `lsmod \| grep smart_meter`, `sudo insmod ...` |
| `open(...) failed: Permission denied` | node mode / user groups | `sudo chmod 0666 /dev/smart_meter` or run as root |
| `Invalid argument` on `ioctl` | driver/user ABI mismatch | rebuild both from the same `include/driver_interface.h` |
| `insmod: ERROR: could not insert ... Invalid module format` | headers do not match the running kernel | install `linux-headers-$(uname -r)` |
| `insmod: ERROR: Key ...` / `symbol not found` | stale module in `/lib/modules` | `sudo rmmod` then rebuild, or reboot |
| `no anomalies detected` with obviously spiky data | `--interval-ms` too long, so samples merge | lower the interval to 250–500 ms |
| dashboard counts nothing | `--source device` but the module is absent | use `--source auto` or `--source sim` |
| `attach failed` in `--ipc` mode | no producer yet | start `./build/app --publish` first |
| CSV stays empty | log directory not writable | `mkdir -p logs && chmod 755 logs` |
| gnuplot produces no PNG | gnuplot not installed | `sudo apt install gnuplot` |

---

## 9. Cleaning up

```bash
sudo rmmod smart_meter_driver        # if still loaded
make clean                            # build artefacts + kernel objects
make distclean                        # also removes logs/*.csv and exports/
rm -f /dev/shm/smart_meter_pulses /dev/mqueue/smart_meter_cmd
```

---

## 10. Kernel compatibility

| Kernel | Status |
|---|---|
| 5.4 – 5.9 | works; `sysfs_emit` falls back to `sprintf`, `proc_ops` is available from 5.6 |
| 5.10 – 6.4 | fully supported (`sysfs_emit`, `compat_ptr_ioctl`) |
| 6.5 – 6.11 | supported; the deprecated `gpio_direction_input` call is skipped |
| 6.12+ | supported; the `no_llseek` → `noop_llseek` shim is applied |
| 6.13+ | expected to work; if the legacy GPIO API is removed entirely, build with `simulate=1` or convert the GPIO path to `gpiod` |
