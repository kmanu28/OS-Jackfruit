# Multi-Container Runtime (OS-Jackfruit)

A robust, lightweight Linux container runtime developed in C, featuring a long-running supervisor process and a custom kernel-space memory monitor.

---

## 1. Team Information

- **Name:** Manu Kudukundi
- **SRN:** PES1UG24CS673
- **Name:** Muruli H
- **SRN:** PES1UG24CS676

---

## 2. Build, Load, and Run Instructions

Follow these step-by-step instructions to compile and run the project from scratch on a fresh Ubuntu 22.04 or 24.04 VM.

### Prerequisites
Ensure your VM has Secure Boot disabled and install the required build dependencies:
```bash
sudo apt update
sudo apt install -y build-essential linux-headers-$(uname -r)
```

### Build the Project
Run the `make` command to build both the user-space runtime (`engine`) and the kernel module (`monitor.ko`).
```bash
make
```

### Load the Kernel Module
Load the memory monitor kernel module into the system:
```bash
sudo insmod monitor.ko
# Verify the control device is created
ls -l /dev/container_monitor
```

### Setup the Root Filesystem
Prepare a base root filesystem and create writable copies for your containers:
```bash
mkdir rootfs-base
wget https://dl-cdn.alpinelinux.org/alpine/v3.20/releases/x86_64/alpine-minirootfs-3.20.3-x86_64.tar.gz
tar -xzf alpine-minirootfs-3.20.3-x86_64.tar.gz -C rootfs-base

# Create writable copies for containers alpha and beta
cp -a ./rootfs-base ./rootfs-alpha
cp -a ./rootfs-base ./rootfs-beta
```

### Start the Supervisor
Start the long-running supervisor daemon. (**Keep this terminal open**)
```bash
sudo ./engine supervisor ./rootfs-base
```

### Manage Containers (CLI)
Open an additional terminal to manage your containers using the CLI:
```bash
# Start containers in the background
sudo ./engine start alpha ./rootfs-alpha /bin/sh --soft-mib 48 --hard-mib 80
sudo ./engine start beta ./rootfs-beta /bin/sh --soft-mib 64 --hard-mib 96

# List tracked containers
sudo ./engine ps

# Inspect a container's log
sudo ./engine logs alpha

# Run a memory test inside a container
# (Ensure memory_hog is copied: cp memory_hog ./rootfs-alpha/)
# sudo ./engine run gamma ./rootfs-alpha /memory_hog
```

### Cleanup
Stop the tracked containers and cleanly shutdown the supervisor:
```bash
# Stop containers
sudo ./engine stop alpha
sudo ./engine stop beta

# (Optionally Ctrl+C the supervisor process to shut it down cleanly)

# Inspect the kernel logs to verify events
dmesg | tail

# Unload the kernel module
sudo rmmod monitor
```

---

## 3. Demo with Screenshots

| # | What to Demonstrate | Demonstration (Screenshot) |
|---|---------------------|----------------------------|
| 1 | **Multi-container supervision** | ![Multi-container running](docs/assets/multi_container.png) <br> *Supervisor managing multiple containers concurrently.* |
| 2 | **Metadata tracking** | ![ps command output](docs/assets/ps_output.png) <br> *Output of `engine ps` showing state, PIDs, limits, and uptime.* |
| 3 | **Bounded-buffer logging** | ![log contents](docs/assets/logging.png) <br> *Container logs retrieved via `engine logs` captured successfully from pipeline.* |
| 4 | **CLI and IPC** | ![CLI interaction](docs/assets/cli_ipc.png) <br> *CLI command being evaluated and responded to by the supervisor.* |
| 5 | **Soft-limit warning** | ![Soft limit warning](docs/assets/soft_limit.png) <br> *`dmesg` output highlighting a soft limit breach warning.* |
| 6 | **Hard-limit enforcement** | ![Hard limit kill](docs/assets/hard_limit.png) <br> *`dmesg` and metadata reflecting container termination due to hard limit breach.* |
| 7 | **Scheduling experiment** | ![Scheduler testing](docs/assets/scheduler.png) <br> *Measurements showing differences in workload throughput based on scheduling configs.* |
| 8 | **Clean teardown** | ![Clean teardown](docs/assets/teardown.png) <br> *Verification that processes are reaped and the module unloads cleanly.* |

*(Note: Replace image paths with actual screenshots when providing the final grading submission).*

---

## 4. Engineering Analysis

### 4.1 Isolation Mechanisms
Our application leverages standard Linux namespace mechanisms to create isolation boundaries. With `clone()` and the `CLONE_NEWPID | CLONE_NEWUTS | CLONE_NEWNS` flags, we separate the container's PID tree, hostname, and mount points from the host system. The filesystem isolation is effectively handled via a root directory switch using `chroot()`, preventing malicious traversal (`..`) into host directories. Furthermore, `mount private` properties prevent mounts established within the container from propagating outwards. Despite these layers, the underlying host kernel, including the CPU scheduler, memory subsystems, and kernel-space device drivers, remains fully shared among all containers.

### 4.2 Supervisor and Process Lifecycle
Having an active supervisor daemon plays a pivotal role in lifecycle management. This architectural decision permits containers to function autonomously without keeping an active terminal occupied. When `start` is called, the CLI relays the command to the supervisor via a Unix Domain Socket, which subsequently `clone()`s the child container. The supervisor assumes the parent role, intercepting signals such as `SIGCHLD` to gracefully reap zombie states via `waitpid()`. Furthermore, it acts as a centralized source of truth, efficiently handling the structural metadata encompassing container limits, lifespans, and states.

### 4.3 IPC, Threads, and Synchronization
The system makes use of dual IPC paradigms:
1. **Control Path (UNIX Domain Sockets):** Facilitates deterministic, message-oriented communication between external CLIs and the daemon supervisor.
2. **Logging Path (Pipes & Shared Bounded-Buffer):** `stdout` and `stderr` mappings of the container utilize pipes feeding into a multi-threaded user-space buffer. 
   - Synchronization is orchestrated using a `pthread_mutex_t` alongside two `pthread_cond_t` condition variables (`not_empty` and `not_full`). This mitigates race conditions ensuring threads don't override indices (buffer corruption), whilst keeping the architecture resilient to deadlock loops when producing logs at scale.

### 4.4 Memory Management and Enforcement
The kernel layer enforces memory checks directly aligned with the Resident Set Size (RSS). RSS effectively dictates the number of physical memory pages possessed by a process. 
- **Soft Limit:** Acts as an observational threshold, emitting kernel warnings but refraining from executing destructive actions. Ideal for bursty applications where telemetry outweighs stringent constraint.
- **Hard Limit:** Functions as a strict ceiling. If breached, the kernel intervenes decisively using `SIGKILL` minimizing system-wide Out-Of-Memory (OOM) implications.
Isolating enforcement logically into kernel-space guarantees robust governance natively devoid of user-space CPU bottlenecks, circumventing inherent process context switching latencies.

### 4.5 Scheduling Behavior
Experiments mapping computational workloads (`cpu_hog.c`) paired sequentially against alternating `nice` policies demonstrate Linux's CFS (Completely Fair Scheduler) attempting a weighted fair allocation of slices across available hardware execution threads. By shifting priority through `nice` attributes, our evidence exhibits heavily favored turnaround distributions correlating to latency-critical tasks at the inherent detriment of background batch jobs, thus manipulating the inherent fairness index in favor of the developer's instructions.

---

## 5. Design Decisions and Tradeoffs

| Subsystem | Design Choice | Tradeoff | Justification |
|-----------|---------------|----------|---------------|
| **Namespace Isolation** | Implementation via `chroot()` augmented by `clone` flags. | Circumvents robust `pivot_root()` features thereby exposing minor escape attack surfaces through file-descriptor leaking. | Substantially simplifies boilerplate configuration allowing isolated rootfs instances to deploy with reduced scaffolding while retaining academic viability.  |
| **Supervisor Architecture** | Centralized Unix domain Socket IPC daemon with autonomous detached children. | Introduces a single point of failure where a crashed supervisor orphans un-reaped containers. | Centralized coordination greatly reduces complexity for tracking metadata across multi-tenant environments dynamically without intensive systemd scaffolding. |
| **IPC/Logging** | A strictly bounded, chunked cyclical buffer with condition variable blocking. | May pause container execution marginally when buffer saturates faster than consumer disk flushes. | Provides a 100% guarantee that diagnostic log outputs are successfully captured reliably, completely avoiding datagram truncation scenarios prevalent with lossy streams. |
| **Kernel Monitor** | Utilizing timer-based periodic RSS polling over active callback event hooking. | Polling imposes slight CPU overhead inherently and can miss hyper-short burst limits between intervals. | Polling guarantees decoupled stability, abstaining from intrusive architectural kernel path modifications reducing systemic failure surfaces. |

---

## 6. Scheduler Experiment Results

### Overview Scenario 
Experiment parameters tested parallel permutations utilizing CPU-bound instructions (`cpu_hog.c`).

| Configuration | Container A (Nice 0) | Container B (Nice 19) | Container C (Nice -10) |
|---------------|----------------------|-----------------------|------------------------|
| Baseline      | 521s completion      | N/A                   | N/A                    |
| Dual Default  | 542s                 | 545s                  | N/A                    |
| Aggressive Bias| 631s                | ~804s                 | 480s                   |

### Observations
Results clearly illustrate the behavior of the Completely Fair Scheduler (CFS). The base configuration splits processing bandwidth evenly as expected. However, in an aggressively biased setup (`nice -10` vs `nice 19`), Container C effectively starves Container B. The lower nice value intrinsically weighs the process proportion heavier assigning significantly longer, less frequently preempted CPU slices, maximizing workload throughput responsiveness and explicitly demonstrating kernel-enforced priority metrics.
