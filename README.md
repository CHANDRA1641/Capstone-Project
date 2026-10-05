# File Integrity & Tamper Evidence Audit System
# File Integrity & Tamper Evidence Audit System

## Project Title

**File Integrity & Tamper Evidence Audit System**

### Project Name: Sentinel

Sentinel is a Linux-based system-level security and monitoring project developed as part of the Wipro LSP + Linux Device Driver and Embedded Systems training track.

The project integrates Linux system programming, C/C++, Linux kernel device drivers, inter-process communication, multithreading, networking, system monitoring, and audit-oriented mechanisms into a single system.

---

## 1. Project Overview

The **File Integrity & Tamper Evidence Audit System** is designed to demonstrate how system-level monitoring can be used to observe system activity, detect changes, and maintain evidence for auditing.

The project is implemented around **Sentinel**, a Linux-based monitoring platform consisting of:

- A Linux kernel character-device driver written in C
- A C++17 user-space daemon
- A command-line client
- A C-based device testing utility
- Simulation support for running the system without physical hardware
- TCP-based communication
- POSIX shared memory
- Multithreading and thread pools
- Linux system calls and IPC mechanisms

The project brings together concepts covered during the training, including:

- Linux
- C/C++
- Linux Device Drivers
- System Programming
- Computer Architecture
- Hardware and Software
- Networking
- Inter-Process Communication
- Multithreading
- File and system monitoring

---

## 2. Problem Statement

Unauthorized modification, unexpected system activity, or changes to monitored resources can affect system reliability, security, and data integrity.

Traditional applications may not provide sufficient visibility into system-level events or provide enough information for later investigation.

The objective of this project is to build a system-level monitoring and auditing platform that can collect system events, process them, maintain historical information, and expose the collected information through controlled interfaces.

The system demonstrates the fundamental concepts required for detecting and investigating changes at the system level.

---

## 3. Proposed Solution

Sentinel uses a layered architecture to collect, process, store, and expose system data.

At the lower level, a Linux character-device driver can generate sensor samples using kernel mechanisms such as:

- High-resolution timers
- Workqueues
- FIFO buffers
- Character-device interfaces
- `ioctl`
- `poll`
- `sysfs`

At the user-space level, a C++ daemon collects the samples and stores them in an in-memory ring buffer.

The daemon provides:

- Data collection
- Sample storage
- Statistics
- Historical information
- TCP communication
- Shared-memory access
- Multithreaded processing

A simulation source is also provided so that the complete user-space system can be built and tested without requiring physical sensor hardware.

---

## 4. Objectives

The main objectives of the project are:

1. Demonstrate Linux system-level programming.
2. Implement a Linux character-device driver.
3. Demonstrate communication between kernel space and user space.
4. Collect and process system or sensor data.
5. Maintain historical data for analysis.
6. Provide command-line access to collected information.
7. Demonstrate IPC mechanisms such as TCP sockets and POSIX shared memory.
8. Apply multithreading and synchronization concepts.
9. Demonstrate system monitoring and audit-oriented design.
10. Provide a buildable and testable project suitable for Linux environments.

---

## 5. Key Features

### User-Space Features

- C++17 based monitoring daemon
- Simulation mode for testing without hardware
- Real-time sample collection
- Ring-buffer based sample storage
- Thread pool for processing requests
- TCP server
- Command-line client
- Historical data retrieval
- Statistics generation
- Live data subscription
- POSIX shared-memory interface
- Signal handling
- Graceful shutdown

### Kernel-Space Features

- Linux character-device driver
- `/dev/sentinel0` device interface
- Kernel FIFO
- High-resolution timer
- Workqueue
- `ioctl` interface
- `poll` support
- `sysfs` attributes
- Synchronization using kernel locking mechanisms
- Device access control

---

## 6. System Architecture

```text
                         SENTINEL SYSTEM
                              │
                              ▼
┌───────────────────────────────────────────────────────────────┐
│                       Linux Kernel                            │
│                                                               │
│   ┌───────────────────────────────────────────────────────┐   │
│   │             sentinel_drv.ko                           │   │
│   │                                                       │   │
│   │  hrtimer → workqueue → FIFO → character device       │   │
│   │                                                       │   │
│   │  /dev/sentinel0                                      │   │
│   │  ioctl / poll / read                                 │   │
│   │  sysfs attributes                                    │   │
│   └───────────────────────────────────────────────────────┘   │
└──────────────────────────────┬────────────────────────────────┘
                               │
                         read(2) / ioctl
                               │
                               ▼
┌───────────────────────────────────────────────────────────────┐
│                     User Space                                │
│                                                               │
│                    ┌───────────────┐                          │
│                    │   sentineld   │                          │
│                    │  C++17 Daemon │                          │
│                    └───────┬───────┘                          │
│                            │                                  │
│             ┌──────────────┼───────────────┐                  │
│             ▼              ▼               ▼                  │
│       SampleStore      ThreadPool       Shared Memory         │
│       Ring Buffer      Processing       POSIX IPC             │
│             │              │               │                  │
│             └──────────────┼───────────────┘                  │
│                            ▼                                  │
│                      TCP Server                               │
└────────────────────────────┬──────────────────────────────────┘
                             │
                 ┌───────────┴───────────┐
                 ▼                       ▼
          sentinel-cli                  netcat
          Command Line                 TCP Client
SimulatedSource
      │
      ▼
 sentineld
      │
      ├── SampleStore
      ├── ThreadPool
      ├── TCP Server
      └── Shared Memory

File Integrity & Tamper Evidence Audit System
│
├── driver/
│   ├── sentinel_drv.c
│   └── ...
│
├── include/
│   ├── source.hpp
│   ├── store.hpp
│   ├── thread_pool.hpp
│   └── ...
│
├── src/
│   ├── sentineld.cpp
│   ├── sentinel_cli.cpp
│   ├── server.cpp
│   ├── source.cpp
│   ├── shm.cpp
│   └── ...
│
├── tests/
│   └── test_main.cpp
│
├── tools/
│   └── sentinel_read.c
│
├── packaging/
│   └── sentineld.service
│
├── Makefile
└── README.md
User Application
       │
       │ read()
       │ ioctl()
       │ poll()
       ▼
/dev/sentinel0
       │
       ▼
Character Device Driver
       │
       ├── FIFO
       ├── hrtimer
       ├── workqueue
       ├── synchronization
       └── sysfs
# How to Execute

This section provides the complete instructions required to build, test, and run the Sentinel project on Ubuntu/Linux.

---

## 1. Prerequisites

The project requires:

- Ubuntu/Linux
- GCC
- G++
- C++17
- GNU Make
- Git
- Netcat (for TCP testing)
- Linux kernel headers (required only for kernel-driver development)

Install the required packages:

```bash
sudo apt update

sudo apt install -y \
    build-essential \
    gcc \
    g++ \
    make \
    git \
    netcat-openbsd
#commands to run in terminal
terminal
 sudo apt update && sudo apt install -y build-essential gcc g++ make git netcat-openbsd && cd ~/sentinel && make clean && make && make test && ./build/release/sentineld --source sim --period 100 --listen 127.0.0.1:9090 --verbose
 in Second terminal:
  cd ~/sentinel && ./build/release/sentinel-cli ping && ./build/release/sentinel-cli latest && ./build/release/sentinel-cli stats 50 && ./build/release/sentinel-cli history 5 && ./build/release/sentinel-cli watch 10 && ./build/release/sentinel-cli shm && printf 'PING\nLATEST\nQUIT\n' | nc 127.0.0.1 9090
for kernel driver testing :
sudo apt install -y linux-headers-$(uname -r) && cd ~/sentinel && make driver && sudo insmod driver/sentinel_drv.ko period_ms=100 fifo_depth=256 && ls -l /dev/sentinel0 && dmesg | tail -20 && sudo ./build/release/sentinel_read -n 5 -s
to stop:
 Ctrl+C
sudo rmmod sentinel_drv
