# Sentinel — embedded sensor telemetry platform (C / C++ only)

Capstone project for the Wipro LSP + LDD embedded track. It ties the whole syllabus together:
a **Linux kernel character-device driver (C)** produces sensor samples, a **C++17 daemon** collects,
analyses and serves them, and a **C++ CLI** plus a **C test tool** consume them.

```
 ┌────────────────────────── kernel ──────────────┐
 │ sentinel_drv.ko  hrtimer → workqueue → FIFO     │  /dev/sentinel0  (read, poll, ioctl)
 │                  sysfs: /sys/class/sentinel/…   │
 └─────────────────────────────────────────────────┘
                     │ read(2) batches of 16        (or SimulatedSource: no hardware needed)
 ┌────────────────────▼──────────── sentineld ─────────────────────────────────────┐
 │ Producer thread → SampleStore (ring) ─┬─ epoll TCP server ◄──► sentinel-cli / nc │
 │                                       ├─ worker ThreadPool (STATS / HISTORY)     │
 │                                       └─ POSIX shm seqlock ──► sentinel-cli shm  │
 └──────────────────────────────────────────────────────────────────────────────────┘
```

## Requirements
Linux, `g++` ≥ 9 (C++17), `gcc`, `make`. Kernel module additionally needs kernel headers
(`sudo apt install build-essential linux-headers-$(uname -r)`) and Linux ≥ 5.10.

## Build and test
```
make                # release build -> build/release/{sentineld,sentinel-cli,sentinel_read}
make test           # 31 unit + integration tests
make check          # same tests under ASan+UBSan, then ThreadSanitizer
make lint           # cppcheck (optional, apt install cppcheck)
sudo make install   # /usr/local/{sbin,bin}   (PREFIX= and DESTDIR= supported)
```

## Run it (no hardware, no driver)
```
build/release/sentineld --source sim --period 100 --listen 127.0.0.1:9090 --verbose
# in another terminal
build/release/sentinel-cli ping
build/release/sentinel-cli latest
build/release/sentinel-cli stats 50
build/release/sentinel-cli history 5
build/release/sentinel-cli watch 10        # live stream
build/release/sentinel-cli set-period 20
build/release/sentinel-cli shm             # lock-free read of shared memory, no network
printf 'PING\nLATEST\nQUIT\n' | nc 127.0.0.1 9090
```
Stop with Ctrl-C (SIGTERM/SIGINT = clean shutdown, SIGHUP = reopen log file, SIGUSR1 = log stats).

## Run as a daemon / service
```
sentineld --source sim --daemon --pidfile /tmp/sentineld.pid --log-file /tmp/sentineld.log
kill -TERM $(cat /tmp/sentineld.pid)
```
systemd: `sudo make install install-service && sudo systemctl enable --now sentineld`
(runs in the foreground under systemd with hardening options; edit `ExecStart` in
`packaging/sentineld.service` to switch to `--source device`).

## Kernel driver
```
make driver                                  # builds driver/sentinel_drv.ko
sudo insmod driver/sentinel_drv.ko period_ms=100 fifo_depth=256
ls -l /dev/sentinel0 ; dmesg | tail
sudo build/release/sentinel_read -n 5 -s     # first check: prints samples + ioctl stats
sudo build/release/sentineld --source device --device /dev/sentinel0
cat /sys/class/sentinel/sentinel0/stats      # sysfs
echo 50 | sudo tee /sys/class/sentinel/sentinel0/period_ms
sudo rmmod sentinel_drv
```
The device is exclusive (second `open` gives `EBUSY`); sampling runs only while it is open.
Try it first in a VM or QEMU guest, not on a machine you care about:
`make -C driver KDIR=/path/to/guest/kernel/build`.

**Status:** the driver has *not* been compiled or run in the environment this project was written in
(no kernel headers there). It follows current kernel APIs with version guards for 5.10 – 6.15+, but
expect to fix a compile error or two on your kernel. Everything in user space was built warning-free
and tested.

## Protocol (text, one command per line)
| Command | Reply |
|---|---|
| `PING`, `INFO`, `LATEST` | `OK {json}` |
| `STATS [n]`, `HISTORY n` (1..1000) | `OK {json}` (computed on a worker thread) |
| `SET PERIOD ms` (10..10000) | `OK {"period_ms":…}` |
| `SUBSCRIBE` / `UNSUBSCRIBE` | `OK …`, then pushed `EVT {json}` lines |
| `QUIT` | `OK {"bye":true}` and close |

Errors: `ERR <BAD_COMMAND|BAD_ARGUMENT|NO_DATA|BUSY|FAILED|TOO_LONG|TOO_MANY> message`.

## Syllabus mapping
| Syllabus topic | Where |
|---|---|
| C++ OOP, RAII, smart pointers, STL | `util.hpp` (UniqueFd), `source.*`, `store.hpp`, `containers.hpp` |
| Threads, mutexes, thread pool | `thread_pool.hpp`, `source.cpp` (Producer), `server.cpp` |
| Sockets, epoll, IPC | `server.cpp`, `sentinel_cli.cpp` |
| POSIX shared memory, atomics | `shm.*` (seqlock) |
| Signals, daemons, fork/exec | `sentineld.cpp` (signalfd), `daemonize.cpp` (double fork, flock pidfile) |
| g++, Makefiles, gdb, sanitizers | `Makefile` (`MODE=debug/asan/tsan`) |
| Kernel modules, char device, cdev, class | `driver/sentinel_drv.c` |
| Spinlock, mutex, wait queue, poll | `sentinel_drv.c` |
| Timers, workqueues | hrtimer + work item in the driver |
| sysfs, ioctl, copy_to_user | driver attributes, `uapi.h`, `sentinel_read.c` |
| Testing | `tests/test_main.cpp` |

## Production notes and limits
* The TCP protocol has no authentication or TLS; it binds to localhost by default. Do not expose it
  as is, or put it behind a tunnel / reverse proxy.
* The simulated source and the driver both generate synthetic data; replace the sample generator
  with a real GPIO/I2C/SPI read to use a real sensor.
* The ioctl magic number is a demo value; register one before shipping a driver publicly.
* Add a `LICENSE` file before publishing (the kernel module is declared GPL-2.0).
