# OS-Jackfruit: Multi-Container Runtime

A lightweight Linux container runtime in C with a long-running parent supervisor and a kernel-space memory monitor.

---

## 1. Team Information

| Name | SRN |
|------|-----|
| [Krrish P Raju] | [PES1UG24CS708] |
| [A R Akshay Kumar] | [PES1UG24CS705] |

---

## 2. Build, Load, and Run Instructions

### Prerequisites
```bash
sudo apt update
sudo apt install -y build-essential linux-headers-$(uname -r)
```

### Get Alpine Root Filesystem
```bash
mkdir rootfs
wget https://dl-cdn.alpinelinux.org/alpine/v3.20/releases/x86_64/alpine-minirootfs-3.20.3-x86_64.tar.gz
tar -xzf alpine-minirootfs-3.20.3-x86_64.tar.gz -C rootfs
```

### Build
```bash
make user
make kernel
```

### Load Kernel Module
```bash
sudo insmod monitor.ko
ls -l /dev/container_monitor
sudo dmesg | tail -5
```

### Start the Supervisor
```bash
sudo ./engine supervisor ./rootfs
```

### Using the CLI
```bash
sudo ./engine start alpha ./rootfs /bin/sleep 60
sudo ./engine start beta ./rootfs /bin/sleep 60
sudo ./engine ps
sudo ./engine logs alpha
sudo ./engine stop alpha
```

### Teardown
```bash
sudo ./engine stop alpha
sudo ./engine stop beta
sudo rmmod monitor
sudo dmesg | tail -3
```

---

## 4. Engineering Analysis

### 4.1 Isolation Mechanisms

The runtime achieves isolation through Linux namespaces and chroot. Each container is spawned using clone() with three namespace flags: CLONE_NEWPID gives the container its own PID namespace, CLONE_NEWUTS gives it its own hostname, and CLONE_NEWNS gives it its own mount namespace. chroot() restricts the filesystem view to the provided rootfs. The host kernel is still shared across all containers — same system call interface, same network stack, same physical hardware.

### 4.2 Supervisor and Process Lifecycle

A long-running parent supervisor is necessary because Linux ties child process reaping to the parent. When a child exits it becomes a zombie until the parent calls waitpid(). The supervisor maintains a container_t registry protected by a mutex. SIGCHLD is handled with waitpid(-1, WNOHANG) in a loop to reap all children atomically. SIGTERM/SIGINT sets a flag causing orderly shutdown.

### 4.3 IPC, Threads, and Synchronization

Two IPC mechanisms are used. Pipes carry container output to the supervisor — each container's stdout/stderr is redirected into a pipe write end via dup2(), and a logger thread reads from the read end. A UNIX domain socket carries CLI commands from the client to the supervisor. The container registry is protected by pthread_mutex_t. The log ring buffer uses pthread_mutex_t plus two condition variables (not_full, not_empty) to synchronize producers and consumer without busy-waiting.

### 4.4 Memory Management and Enforcement

RSS (Resident Set Size) counts physical pages currently in RAM for a process. A soft limit logs a warning when first exceeded without killing the process. A hard limit sends SIGKILL when exceeded. Enforcement belongs in kernel space because user-space polling is racy — the kernel module uses delayed_work to check RSS every 1000ms with direct access to task_struct and mm_struct.

### 4.5 Scheduling Behavior

Linux CFS assigns CPU time proportional to weight derived from nice value. nice=0 has weight 1024, nice=19 has weight 15. When competing, nice=0 gets ~98.6% of CPU. I/O-bound processes voluntarily yield on fsync(), so they get scheduling boosts on wakeup but still cannot compete with a continuously runnable CPU-bound process.

---

## 5. Design Decisions and Tradeoffs

**Namespace Isolation:** Used PID+UTS+mount namespaces. No network namespace to avoid veth complexity. Sufficient for demonstrating meaningful isolation.

**Supervisor Architecture:** Single-threaded accept loop. Tradeoff: slow commands block others. Justified for lab scope — easy to upgrade to thread-per-connection.

**IPC and Logging:** Pipe per container for logging, UNIX socket for CLI. Pipes are unidirectional but EOF naturally signals container exit to the logger thread.

**Kernel Monitor:** mutex + delayed_work + dmesg-only reporting. A full notification path would require extra character device complexity not needed for this project.

**Scheduler Experiments:** Used nice values and host-level time measurement for clean reproducible results without container overhead confounding the data.

---

## 6. Scheduler Experiment Results

### Experiment 1: CPU-Bound with Different Priorities

| Configuration | Wall-Clock Time |
|---------------|----------------|
| nice -n 0 (high priority) | ~0.180s |
| nice -n 19 (low priority) | ~0.174s |

When run sequentially both complete similarly — no CPU competition. When run concurrently nice=19 gets ~1.5% CPU vs ~98.5% for nice=0, matching CFS weight ratio of 15:1024.

### Experiment 2: CPU-Bound vs I/O-Bound

| Workload | CPU% | Wall-Clock Time |
|----------|------|----------------|
| workload_cpu (200M iterations) | ~95-98% | ~0.180s |
| workload_io (2000 rounds) | ~2-5% | ~8s |

The CPU-bound workload dominates because workload_io blocks on fsync() most of the time. CFS correctly gives idle CPU to the CPU-bound process when the I/O-bound one is sleeping.

---

## License

Educational project — PES University OS Lab.
