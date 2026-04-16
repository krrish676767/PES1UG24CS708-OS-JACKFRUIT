/* engine.c — Multi-Container Runtime + Supervisor
 * OS-Jackfruit Project
 *
 * Two modes:
 *   ./engine supervisor [rootfs]      — start the long-running supervisor
 *   ./engine <cmd> [args...]          — CLI client (connects to supervisor)
 *
 * Build:  make user   (or: gcc -Wall -Wextra -g -D_GNU_SOURCE -o engine engine.c -lpthread)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <time.h>
#include <signal.h>
#include <pthread.h>

#include <sys/wait.h>
#include <sys/stat.h>
#include <sys/mount.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/ioctl.h>
#include <sys/types.h>

#include <sched.h>   /* clone() */

#include "monitor_ioctl.h"

/* ═══════════════════════════════════════════════════════════════════════════
 *  CONFIGURATION
 * ═══════════════════════════════════════════════════════════════════════════ */

#define MAX_CONTAINERS    16
#define STACK_SIZE        (1024 * 1024)   /* 1 MB stack per container   */
#define LOG_BUF_SLOTS     256             /* bounded buffer capacity    */
#define LOG_ENTRY_MAX     4096            /* max bytes per log chunk    */
#define SOCKET_PATH       "/tmp/jackfruit.sock"
#define LOG_DIR           "/tmp/jackfruit_logs"
#define MONITOR_DEV       "/dev/container_monitor"
#define CMD_MAX           2048

/* ═══════════════════════════════════════════════════════════════════════════
 *  DATA STRUCTURES
 * ═══════════════════════════════════════════════════════════════════════════ */

/* ── Container state ─────────────────────────────────────────────────────── */
typedef enum {
    CSTATE_STARTING,
    CSTATE_RUNNING,
    CSTATE_STOPPED,
    CSTATE_KILLED,
    CSTATE_LIMIT_KILLED,
} container_state_t;

static const char *state_str(container_state_t s)
{
    switch (s) {
    case CSTATE_STARTING:     return "starting";
    case CSTATE_RUNNING:      return "running";
    case CSTATE_STOPPED:      return "stopped";
    case CSTATE_KILLED:       return "killed";
    case CSTATE_LIMIT_KILLED: return "limit-killed";
    default:                  return "unknown";
    }
}

/* ── Per-container metadata ──────────────────────────────────────────────── */
typedef struct {
    int    active;             /* 1 = slot in use                          */
    int    id;                 /* index in registry array                  */
    char   name[64];
    pid_t  host_pid;
    time_t start_time;
    container_state_t state;
    long   soft_limit_kb;
    long   hard_limit_kb;
    char   log_path[256];
    int    exit_status;        /* filled by SIGCHLD handler                */
    int    log_pipe_read;      /* supervisor reads container output here   */
} container_t;

/* ── Container registry ──────────────────────────────────────────────────── */
typedef struct {
    container_t     slots[MAX_CONTAINERS];
    pthread_mutex_t lock;
} registry_t;

static registry_t reg;

static void registry_init(void)
{
    memset(&reg, 0, sizeof(reg));
    pthread_mutex_init(&reg.lock, NULL);
}

/* Allocate a free slot.  Returns pointer with reg.lock HELD.
 * Caller must call registry_unlock() when done initialising the slot. */
static container_t *registry_alloc(void)
{
    pthread_mutex_lock(&reg.lock);
    for (int i = 0; i < MAX_CONTAINERS; i++) {
        if (!reg.slots[i].active) {
            memset(&reg.slots[i], 0, sizeof(container_t));
            reg.slots[i].active = 1;
            reg.slots[i].id     = i;
            return &reg.slots[i];
        }
    }
    pthread_mutex_unlock(&reg.lock);
    return NULL; /* full */
}

static void registry_unlock(void) { pthread_mutex_unlock(&reg.lock); }

/* Caller must hold reg.lock */
static container_t *registry_find_by_name(const char *name)
{
    for (int i = 0; i < MAX_CONTAINERS; i++)
        if (reg.slots[i].active && strcmp(reg.slots[i].name, name) == 0)
            return &reg.slots[i];
    return NULL;
}

/* Caller must hold reg.lock */
static container_t *registry_find_by_pid(pid_t pid)
{
    for (int i = 0; i < MAX_CONTAINERS; i++)
        if (reg.slots[i].active && reg.slots[i].host_pid == pid)
            return &reg.slots[i];
    return NULL;
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  BOUNDED LOG BUFFER  (Task 3)
 *
 *  One ring buffer shared by all logger threads (producers) and one
 *  consumer thread.  Synchronised with a mutex + two condition variables.
 *
 *  Race conditions without synchronisation:
 *    - Two producers could write to the same slot simultaneously.
 *    - head/tail/count could be read/written non-atomically.
 *    - Consumer could read an empty or half-written entry.
 *
 *  Choice: mutex guards all shared state; condvars eliminate busy-waiting.
 * ═══════════════════════════════════════════════════════════════════════════ */
typedef struct {
    char   data[LOG_ENTRY_MAX];
    size_t len;
    int    container_id;
} log_entry_t;

typedef struct {
    log_entry_t     entries[LOG_BUF_SLOTS];
    int             head;        /* next slot consumer reads              */
    int             tail;        /* next slot producer writes             */
    int             count;
    int             shutdown;    /* set to 1 to wake sleeping consumer    */
    pthread_mutex_t lock;
    pthread_cond_t  not_full;
    pthread_cond_t  not_empty;
} log_buffer_t;

static log_buffer_t log_buf;

static void log_buf_init(void)
{
    memset(&log_buf, 0, sizeof(log_buf));
    pthread_mutex_init(&log_buf.lock,     NULL);
    pthread_cond_init(&log_buf.not_full,  NULL);
    pthread_cond_init(&log_buf.not_empty, NULL);
}

/* Producer: blocks when buffer is full */
static void log_buf_push(int container_id, const char *data, size_t len)
{
    if (len > LOG_ENTRY_MAX) len = LOG_ENTRY_MAX;
    pthread_mutex_lock(&log_buf.lock);

    while (log_buf.count == LOG_BUF_SLOTS && !log_buf.shutdown)
        pthread_cond_wait(&log_buf.not_full, &log_buf.lock);

    if (!log_buf.shutdown) {
        log_entry_t *e  = &log_buf.entries[log_buf.tail];
        memcpy(e->data, data, len);
        e->len           = len;
        e->container_id  = container_id;
        log_buf.tail     = (log_buf.tail + 1) % LOG_BUF_SLOTS;
        log_buf.count++;
        pthread_cond_signal(&log_buf.not_empty);
    }

    pthread_mutex_unlock(&log_buf.lock);
}

/* Consumer: blocks when buffer is empty */
static int log_buf_pop(log_entry_t *out)
{
    pthread_mutex_lock(&log_buf.lock);

    while (log_buf.count == 0 && !log_buf.shutdown)
        pthread_cond_wait(&log_buf.not_empty, &log_buf.lock);

    if (log_buf.count == 0) {   /* shutdown + empty */
        pthread_mutex_unlock(&log_buf.lock);
        return 0;               /* signal consumer to exit */
    }

    *out         = log_buf.entries[log_buf.head];
    log_buf.head = (log_buf.head + 1) % LOG_BUF_SLOTS;
    log_buf.count--;
    pthread_cond_signal(&log_buf.not_full);

    pthread_mutex_unlock(&log_buf.lock);
    return 1;
}

static void log_buf_shutdown(void)
{
    pthread_mutex_lock(&log_buf.lock);
    log_buf.shutdown = 1;
    pthread_cond_broadcast(&log_buf.not_empty);
    pthread_cond_broadcast(&log_buf.not_full);
    pthread_mutex_unlock(&log_buf.lock);
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  SIGCHLD HANDLER  (Task 2)
 * ═══════════════════════════════════════════════════════════════════════════ */
static volatile sig_atomic_t supervisor_running = 1;

static void sigchld_handler(int sig)
{
    (void)sig;
    int saved_errno = errno;
    int status;
    pid_t pid;

    /* Reap ALL available children without blocking */
    while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
        /* Lock registry and update container state */
        pthread_mutex_lock(&reg.lock);
        container_t *c = registry_find_by_pid(pid);
        if (c) {
            if (WIFEXITED(status)) {
                c->state       = CSTATE_STOPPED;
                c->exit_status = WEXITSTATUS(status);
            } else if (WIFSIGNALED(status)) {
                c->state       = CSTATE_KILLED;
                c->exit_status = WTERMSIG(status);
            }
        }
        pthread_mutex_unlock(&reg.lock);
    }
    errno = saved_errno;
}

static void sigterm_handler(int sig)
{
    (void)sig;
    supervisor_running = 0;
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  CONTAINER LAUNCH  (Task 1)
 * ═══════════════════════════════════════════════════════════════════════════ */

typedef struct {
    char  rootfs[256];
    char *argv[32];
    int   argc;
    int   pipe_write_fd;  /* container's stdout/stderr go here */
    char  name[64];       /* for sethostname                   */
} container_launch_args_t;

/* Runs inside the new namespaces.  Must NOT return on success. */
static int container_fn(void *arg)
{
    container_launch_args_t *a = (container_launch_args_t *)arg;

    /* Redirect stdout + stderr into the logging pipe */
    dup2(a->pipe_write_fd, STDOUT_FILENO);
    dup2(a->pipe_write_fd, STDERR_FILENO);
    close(a->pipe_write_fd);

    /* Set container hostname */
    sethostname(a->name, strlen(a->name));

    /* Mount /proc inside the new mount namespace */
    char proc_path[300];
    snprintf(proc_path, sizeof(proc_path), "%s/proc", a->rootfs);
    mkdir(proc_path, 0555);
    mount("proc", proc_path, "proc", 0, NULL);

    /* chroot into rootfs */
    if (chroot(a->rootfs) < 0) {
        perror("chroot");
        /* Fall through without chroot if rootfs not fully set up */
    } else {
        chdir("/");
    }

    /* exec the requested command */
    execvp(a->argv[0], a->argv);
    perror("execvp");

    return 1;
}

/* Launches a container; returns host PID, or -1 on error. */
static pid_t launch_container(container_t *c,
                               const char *rootfs,
                               char *const argv[],
                               int argc,
                               long soft_kb,
                               long hard_kb)
{
    /* Create pipe: container writes, supervisor reads */
    int pipefd[2];
    if (pipe(pipefd) < 0) { perror("pipe"); return -1; }

    /* Allocate a stack for the cloned thread */
    char *stack = malloc(STACK_SIZE);
    if (!stack) { perror("malloc(stack)"); close(pipefd[0]); close(pipefd[1]); return -1; }
    char *stack_top = stack + STACK_SIZE;   /* stacks grow downward on x86 */

    /* Build args struct */
    static container_launch_args_t cargs;
    memset(&cargs, 0, sizeof(cargs));
    strncpy(cargs.rootfs, rootfs, sizeof(cargs.rootfs) - 1);
    strncpy(cargs.name,   c->name, sizeof(cargs.name) - 1);
    cargs.pipe_write_fd = pipefd[1];
    cargs.argc          = argc;
    for (int i = 0; i < argc && i < 31; i++)
        cargs.argv[i] = argv[i];
    cargs.argv[argc] = NULL;

    int flags = CLONE_NEWPID   /* new PID namespace            */
              | CLONE_NEWUTS   /* new hostname namespace        */
              | CLONE_NEWNS    /* new mount namespace           */
              | SIGCHLD;       /* parent gets SIGCHLD on exit  */

    pid_t pid = clone(container_fn, stack_top, flags, &cargs);
    close(pipefd[1]);   /* supervisor never writes into the pipe */

    if (pid < 0) {
        perror("clone");
        free(stack);
        close(pipefd[0]);
        return -1;
    }

    /* Fill container metadata */
    c->host_pid      = pid;
    c->log_pipe_read = pipefd[0];
    c->start_time    = time(NULL);
    c->state         = CSTATE_RUNNING;
    c->soft_limit_kb = soft_kb;
    c->hard_limit_kb = hard_kb;
    snprintf(c->log_path, sizeof(c->log_path), LOG_DIR "/%s.log", c->name);

    /* Register with kernel monitor module */
    int mfd = open(MONITOR_DEV, O_RDWR);
    if (mfd >= 0) {
        struct container_limits lim = { pid, soft_kb, hard_kb };
        ioctl(mfd, MONITOR_REGISTER, &lim);
        close(mfd);
    }

    return pid;
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  LOGGER THREAD  (Task 3 — Producer)
 *
 *  One thread per running container.
 *  Reads from the container's pipe end and pushes chunks into log_buf.
 * ═══════════════════════════════════════════════════════════════════════════ */
typedef struct {
    int container_id;
    int pipe_fd;
} logger_args_t;

static void *logger_thread(void *arg)
{
    logger_args_t *a = (logger_args_t *)arg;
    char buf[LOG_ENTRY_MAX];
    ssize_t n;

    while ((n = read(a->pipe_fd, buf, sizeof(buf))) > 0)
        log_buf_push(a->container_id, buf, (size_t)n);

    /* EOF: container exited or pipe closed */
    close(a->pipe_fd);
    free(a);
    return NULL;
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  CONSUMER THREAD  (Task 3)
 *
 *  Single thread: pops entries from log_buf and writes to per-container
 *  log files.  Opens log files lazily on first write.
 * ═══════════════════════════════════════════════════════════════════════════ */
static void *consumer_thread(void *arg)
{
    (void)arg;
    int log_fds[MAX_CONTAINERS];
    for (int i = 0; i < MAX_CONTAINERS; i++) log_fds[i] = -1;

    mkdir(LOG_DIR, 0755);

    log_entry_t entry;
    while (log_buf_pop(&entry)) {
        int id = entry.container_id;
        if (id < 0 || id >= MAX_CONTAINERS) continue;

        /* Open log file lazily */
        if (log_fds[id] < 0) {
            char path[256];
            pthread_mutex_lock(&reg.lock);
            strncpy(path, reg.slots[id].log_path, sizeof(path) - 1);
            pthread_mutex_unlock(&reg.lock);

            log_fds[id] = open(path, O_WRONLY | O_CREAT | O_APPEND, 0644);
            if (log_fds[id] < 0) {
                perror("open log file");
                continue;
            }
        }

        write(log_fds[id], entry.data, entry.len);
    }

    /* Flush and close all log files on shutdown */
    for (int i = 0; i < MAX_CONTAINERS; i++)
        if (log_fds[i] >= 0) close(log_fds[i]);

    return NULL;
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  IPC COMMAND PROTOCOL  (Task 2)
 * ═══════════════════════════════════════════════════════════════════════════ */

static void handle_command(int client_fd)
{
    char cmd[CMD_MAX];
    ssize_t n = read(client_fd, cmd, sizeof(cmd) - 1);
    if (n <= 0) return;
    cmd[n] = '\0';
    cmd[strcspn(cmd, "\n")] = '\0';   /* strip trailing newline */

    char *tok = strtok(cmd, " ");
    if (!tok) { dprintf(client_fd, "ERR empty command\n"); return; }

    /* ── start / run ─────────────────────────────────────────────────────── */
    if (strcmp(tok, "start") == 0 || strcmp(tok, "run") == 0) {
        int foreground = (strcmp(tok, "run") == 0);

        char *name   = strtok(NULL, " ");
        char *rootfs = strtok(NULL, " ");
        if (!name || !rootfs) {
            dprintf(client_fd, "ERR usage: start <n> <rootfs> <cmd...>\n");
            return;
        }

        /* Collect remaining tokens as the command argv */
        char *argv_ptrs[32]; int argc = 0;
        char *t;
        while ((t = strtok(NULL, " ")) && argc < 31)
            argv_ptrs[argc++] = t;
        argv_ptrs[argc] = NULL;
        if (argc == 0) { dprintf(client_fd, "ERR no command given\n"); return; }

        /* Check for duplicate name */
        pthread_mutex_lock(&reg.lock);
        if (registry_find_by_name(name)) {
            pthread_mutex_unlock(&reg.lock);
            dprintf(client_fd, "ERR name '%s' already in use\n", name);
            return;
        }
        pthread_mutex_unlock(&reg.lock);

        /* Allocate metadata slot (returns with lock held) */
        container_t *c = registry_alloc();
        if (!c) {
            dprintf(client_fd, "ERR too many containers\n");
            return;
        }
        strncpy(c->name, name, sizeof(c->name) - 1);
        registry_unlock();

        long soft_kb = 0, hard_kb = 0;

        pid_t pid = launch_container(c, rootfs, argv_ptrs, argc, soft_kb, hard_kb);
        if (pid < 0) {
            pthread_mutex_lock(&reg.lock);
            c->active = 0;
            registry_unlock();
            dprintf(client_fd, "ERR launch failed\n");
            return;
        }

        /* Start a logger thread for this container */
        pthread_t ltid;
        logger_args_t *la = malloc(sizeof(*la));
        la->container_id  = c->id;
        la->pipe_fd       = c->log_pipe_read;
        pthread_create(&ltid, NULL, logger_thread, la);
        pthread_detach(ltid);

        dprintf(client_fd, "OK pid=%d name=%s\n", pid, name);

        /* If 'run': wait in foreground */
        if (foreground) {
            int ws;
            waitpid(pid, &ws, 0);
            pthread_mutex_lock(&reg.lock);
            container_t *fc = registry_find_by_pid(pid);
            if (fc) {
                if (WIFEXITED(ws))
                    fc->state = CSTATE_STOPPED, fc->exit_status = WEXITSTATUS(ws);
                else if (WIFSIGNALED(ws))
                    fc->state = CSTATE_KILLED,  fc->exit_status = WTERMSIG(ws);
            }
            pthread_mutex_unlock(&reg.lock);
        }

    /* ── ps ──────────────────────────────────────────────────────────────── */
    } else if (strcmp(tok, "ps") == 0) {
        pthread_mutex_lock(&reg.lock);
        dprintf(client_fd, "%-4s  %-20s  %-8s  %-14s  %s\n",
                "ID", "NAME", "PID", "STATE", "LOG");
        for (int i = 0; i < MAX_CONTAINERS; i++) {
            container_t *c = &reg.slots[i];
            if (!c->active) continue;
            dprintf(client_fd, "%-4d  %-20s  %-8d  %-14s  %s\n",
                    c->id, c->name, c->host_pid,
                    state_str(c->state), c->log_path);
        }
        pthread_mutex_unlock(&reg.lock);

    /* ── logs ────────────────────────────────────────────────────────────── */
    } else if (strcmp(tok, "logs") == 0) {
        char *name = strtok(NULL, " ");
        if (!name) { dprintf(client_fd, "ERR usage: logs <n>\n"); return; }

        pthread_mutex_lock(&reg.lock);
        container_t *c = registry_find_by_name(name);
        if (!c) {
            pthread_mutex_unlock(&reg.lock);
            dprintf(client_fd, "ERR container '%s' not found\n", name);
            return;
        }
        char path[256];
        strncpy(path, c->log_path, sizeof(path) - 1);
        pthread_mutex_unlock(&reg.lock);

        int fd = open(path, O_RDONLY);
        if (fd < 0) { dprintf(client_fd, "ERR log file not found\n"); return; }
        char buf[4096]; ssize_t r;
        while ((r = read(fd, buf, sizeof(buf))) > 0)
            write(client_fd, buf, r);
        close(fd);

    /* ── stop ────────────────────────────────────────────────────────────── */
    } else if (strcmp(tok, "stop") == 0) {
        char *name = strtok(NULL, " ");
        if (!name) { dprintf(client_fd, "ERR usage: stop <n>\n"); return; }

        pthread_mutex_lock(&reg.lock);
        container_t *c = registry_find_by_name(name);
        if (!c || c->state != CSTATE_RUNNING) {
            pthread_mutex_unlock(&reg.lock);
            dprintf(client_fd, "ERR container '%s' not running\n", name);
            return;
        }
        pid_t pid = c->host_pid;
        c->state  = CSTATE_KILLED;
        pthread_mutex_unlock(&reg.lock);

        /* Graceful: SIGTERM first, then SIGKILL if still alive after 3 s */
        kill(pid, SIGTERM);

        int ws; int tries = 30;
        while (tries-- > 0) {
            if (waitpid(pid, &ws, WNOHANG) > 0) break;
            usleep(100000);   /* 100 ms */
        }
        /* Force kill if still alive */
        if (kill(pid, 0) == 0) kill(pid, SIGKILL);

        /* Unregister from kernel monitor */
        int mfd = open(MONITOR_DEV, O_RDWR);
        if (mfd >= 0) {
            ioctl(mfd, MONITOR_UNREGISTER, &pid);
            close(mfd);
        }

        dprintf(client_fd, "OK stopped %s (pid=%d)\n", name, pid);

    } else {
        dprintf(client_fd, "ERR unknown command: %s\n", tok);
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  SUPERVISOR MAIN LOOP
 * ═══════════════════════════════════════════════════════════════════════════ */
static void run_supervisor(const char *rootfs)
{
    (void)rootfs;

    /* ── Signal handlers ────────────────────────────────────────────────── */
    struct sigaction sa_chld = {0};
    sa_chld.sa_handler = sigchld_handler;
    sigemptyset(&sa_chld.sa_mask);
    sa_chld.sa_flags = SA_RESTART | SA_NOCLDSTOP;
    sigaction(SIGCHLD, &sa_chld, NULL);

    struct sigaction sa_term = {0};
    sa_term.sa_handler = sigterm_handler;
    sigemptyset(&sa_term.sa_mask);
    sigaction(SIGTERM, &sa_term, NULL);
    sigaction(SIGINT,  &sa_term, NULL);

    /* ── Initialise subsystems ──────────────────────────────────────────── */
    registry_init();
    log_buf_init();
    mkdir(LOG_DIR, 0755);

    /* ── Start consumer thread ──────────────────────────────────────────── */
    pthread_t cons_tid;
    pthread_create(&cons_tid, NULL, consumer_thread, NULL);

    /* ── Create UNIX domain socket ──────────────────────────────────────── */
    int srv_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (srv_fd < 0) { perror("socket"); exit(1); }

    struct sockaddr_un addr = {0};
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, SOCKET_PATH, sizeof(addr.sun_path) - 1);
    unlink(SOCKET_PATH);

    if (bind(srv_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind"); exit(1);
    }
    if (listen(srv_fd, 8) < 0) { perror("listen"); exit(1); }

    printf("[supervisor] ready — socket: %s\n", SOCKET_PATH);
    fflush(stdout);

    /* ── Accept loop ────────────────────────────────────────────────────── */
    while (supervisor_running) {
        int client_fd = accept(srv_fd, NULL, NULL);
        if (client_fd < 0) {
            if (errno == EINTR) continue;
            perror("accept");
            break;
        }
        handle_command(client_fd);
        close(client_fd);
    }

    /* ── Orderly shutdown — SIGTERM all running containers ──────────────── */
    printf("[supervisor] shutting down...\n");

    pthread_mutex_lock(&reg.lock);
    for (int i = 0; i < MAX_CONTAINERS; i++) {
        container_t *c = &reg.slots[i];
        if (c->active && c->state == CSTATE_RUNNING) {
            kill(c->host_pid, SIGTERM);
        }
    }
    pthread_mutex_unlock(&reg.lock);

    /* Give containers a moment to exit, then reap */
    sleep(1);
    while (waitpid(-1, NULL, WNOHANG) > 0) {}

    log_buf_shutdown();
    pthread_join(cons_tid, NULL);

    close(srv_fd);
    unlink(SOCKET_PATH);

    printf("[supervisor] exited cleanly\n");
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  CLI CLIENT
 * ═══════════════════════════════════════════════════════════════════════════ */
static void run_cli(int argc, char *argv[])
{
    char cmd[CMD_MAX] = {0};
    for (int i = 1; i < argc; i++) {
        if (i > 1) strncat(cmd, " ", sizeof(cmd) - strlen(cmd) - 1);
        strncat(cmd, argv[i], sizeof(cmd) - strlen(cmd) - 1);
    }
    strncat(cmd, "\n", sizeof(cmd) - strlen(cmd) - 1);

    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) { perror("socket"); exit(1); }

    struct sockaddr_un addr = {0};
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, SOCKET_PATH, sizeof(addr.sun_path) - 1);

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        fprintf(stderr, "Cannot connect to supervisor at %s\n"
                        "Is it running?  Try: sudo ./engine supervisor\n",
                SOCKET_PATH);
        exit(1);
    }

    write(fd, cmd, strlen(cmd));
    shutdown(fd, SHUT_WR);

    char buf[4096]; ssize_t n;
    while ((n = read(fd, buf, sizeof(buf))) > 0)
        write(STDOUT_FILENO, buf, n);
    close(fd);
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  ENTRY POINT
 * ═══════════════════════════════════════════════════════════════════════════ */
int main(int argc, char *argv[])
{
    if (argc < 2) {
        fprintf(stderr,
            "Usage:\n"
            "  %s supervisor [rootfs]              start the supervisor\n"
            "  %s start <n> <rootfs> <cmd...>   launch a container\n"
            "  %s run   <n> <rootfs> <cmd...>   launch and wait\n"
            "  %s ps                               list containers\n"
            "  %s logs  <n>                     show container log\n"
            "  %s stop  <n>                     stop a container\n",
            argv[0], argv[0], argv[0], argv[0], argv[0], argv[0]);
        return 1;
    }

    if (strcmp(argv[1], "supervisor") == 0) {
        const char *rootfs = (argc >= 3) ? argv[2] : "./rootfs";
        run_supervisor(rootfs);
    } else {
        run_cli(argc, argv);
    }
    return 0;
}
