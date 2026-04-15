#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/resource.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "monitor_ioctl.h"

#define STACK_SIZE (1024 * 1024)
#define CONTAINER_ID_LEN 32
#define COMMAND_LEN 512
#define RESPONSE_LEN 16384
#define SOCKET_PATH "/tmp/mini_runtime.sock"
#define LOG_DIR "logs"
#define LOG_CHUNK_SIZE 1024
#define LOG_BUFFER_CAPACITY 64
#define DEFAULT_SOFT_LIMIT (40UL << 20)
#define DEFAULT_HARD_LIMIT (64UL << 20)

typedef enum {
    CMD_SUPERVISOR = 0,
    CMD_START,
    CMD_RUN,
    CMD_PS,
    CMD_LOGS,
    CMD_STOP
} command_kind_t;

typedef enum {
    CONTAINER_STARTING = 0,
    CONTAINER_RUNNING,
    CONTAINER_STOPPED,
    CONTAINER_EXITED,
    CONTAINER_KILLED,
    CONTAINER_HARD_LIMIT_KILLED
} container_state_t;

typedef struct {
    command_kind_t kind;
    char container_id[CONTAINER_ID_LEN];
    char rootfs[PATH_MAX];
    char command[COMMAND_LEN];
    unsigned long soft_limit_bytes;
    unsigned long hard_limit_bytes;
    int nice_value;
} control_request_t;

typedef struct {
    int status;
    int exit_code;
    int exit_signal;
    int is_final;
    char payload[RESPONSE_LEN];
} control_response_t;

typedef struct {
    char container_id[CONTAINER_ID_LEN];
    size_t length;
    char data[LOG_CHUNK_SIZE];
} log_item_t;

typedef struct {
    log_item_t items[LOG_BUFFER_CAPACITY];
    size_t head;
    size_t tail;
    size_t count;
    int shutting_down;
    pthread_mutex_t mutex;
    pthread_cond_t not_empty;
    pthread_cond_t not_full;
} bounded_buffer_t;

typedef struct {
    char id[CONTAINER_ID_LEN];
    char rootfs[PATH_MAX];
    char command[COMMAND_LEN];
    int nice_value;
    int log_write_fd;
} child_config_t;

typedef struct container_record {
    char id[CONTAINER_ID_LEN];
    char rootfs[PATH_MAX];
    char command[COMMAND_LEN];
    char log_path[PATH_MAX];
    pid_t host_pid;
    time_t started_at;
    unsigned long soft_limit_bytes;
    unsigned long hard_limit_bytes;
    int nice_value;
    container_state_t state;
    int exit_code;
    int exit_signal;
    int stop_requested;
    int monitor_registered;
    int producer_joined;
    int log_pipe_read_fd;
    int wait_client_fd;
    void *child_stack;
    pthread_t producer_thread;
    struct container_record *next;
} container_record_t;

typedef struct {
    bounded_buffer_t log_buffer;
    pthread_mutex_t metadata_lock;
    pthread_t consumer_thread;
    container_record_t *containers;
    int server_fd;
    int monitor_fd;
    int shutting_down;
    char base_rootfs[PATH_MAX];
} supervisor_ctx_t;

typedef struct {
    supervisor_ctx_t *ctx;
    container_record_t *container;
} producer_arg_t;

static supervisor_ctx_t *g_ctx;
static volatile sig_atomic_t g_sigchld_pending;
static volatile sig_atomic_t g_shutdown_requested;
static volatile sig_atomic_t g_client_stop_requested;

static void usage(const char *prog)
{
    fprintf(stderr,
            "Usage:\n"
            "  %s supervisor <rootfs>\n"
            "  %s start <id> <rootfs> <cmd> [--soft-mib N] [--hard-mib N] [--nice N]\n"
            "  %s run <id> <rootfs> <cmd> [--soft-mib N] [--hard-mib N] [--nice N]\n"
            "  %s ps\n"
            "  %s logs <id>\n"
            "  %s stop <id>\n",
            prog, prog, prog, prog, prog, prog);
}

static int write_full(int fd, const void *buf, size_t count)
{
    const char *ptr = buf;
    size_t written = 0;

    while (written < count) {
        ssize_t rc = write(fd, ptr + written, count - written);
        if (rc < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        if (rc == 0) {
            return -1;
        }
        written += (size_t)rc;
    }
    return 0;
}

static int read_full(int fd, void *buf, size_t count)
{
    char *ptr = buf;
    size_t read_bytes = 0;

    while (read_bytes < count) {
        ssize_t rc = read(fd, ptr + read_bytes, count - read_bytes);
        if (rc < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        if (rc == 0) {
            return -1;
        }
        read_bytes += (size_t)rc;
    }
    return 0;
}

static void set_response(control_response_t *resp,
                         int status,
                         int exit_code,
                         int exit_signal,
                         int is_final,
                         const char *fmt,
                         ...)
{
    va_list ap;

    memset(resp, 0, sizeof(*resp));
    resp->status = status;
    resp->exit_code = exit_code;
    resp->exit_signal = exit_signal;
    resp->is_final = is_final;

    va_start(ap, fmt);
    vsnprintf(resp->payload, sizeof(resp->payload), fmt, ap);
    va_end(ap);
}

static const char *state_to_string(container_state_t state)
{
    switch (state) {
    case CONTAINER_STARTING:
        return "starting";
    case CONTAINER_RUNNING:
        return "running";
    case CONTAINER_STOPPED:
        return "stopped";
    case CONTAINER_EXITED:
        return "exited";
    case CONTAINER_KILLED:
        return "killed";
    case CONTAINER_HARD_LIMIT_KILLED:
        return "hard_limit_killed";
    default:
        return "unknown";
    }
}

static int ensure_log_dir(void)
{
    if (mkdir(LOG_DIR, 0755) == 0 || errno == EEXIST) {
        return 0;
    }
    perror("mkdir logs");
    return -1;
}

static int parse_mib_flag(const char *flag, const char *value, unsigned long *target_bytes)
{
    char *end = NULL;
    unsigned long mib;

    errno = 0;
    mib = strtoul(value, &end, 10);
    if (errno != 0 || end == value || *end != '\0') {
        fprintf(stderr, "invalid value for %s: %s\n", flag, value);
        return -1;
    }

    if (mib > ULONG_MAX / (1UL << 20)) {
        fprintf(stderr, "value too large for %s: %s\n", flag, value);
        return -1;
    }

    *target_bytes = mib * (1UL << 20);
    return 0;
}

static int parse_optional_flags(control_request_t *req, int argc, char *argv[], int start_index)
{
    int i;

    for (i = start_index; i < argc; i += 2) {
        char *end = NULL;
        long nice_value;

        if (i + 1 >= argc) {
            fprintf(stderr, "missing value for option: %s\n", argv[i]);
            return -1;
        }

        if (strcmp(argv[i], "--soft-mib") == 0) {
            if (parse_mib_flag(argv[i], argv[i + 1], &req->soft_limit_bytes) != 0) {
                return -1;
            }
            continue;
        }

        if (strcmp(argv[i], "--hard-mib") == 0) {
            if (parse_mib_flag(argv[i], argv[i + 1], &req->hard_limit_bytes) != 0) {
                return -1;
            }
            continue;
        }

        if (strcmp(argv[i], "--nice") == 0) {
            errno = 0;
            nice_value = strtol(argv[i + 1], &end, 10);
            if (errno != 0 || end == argv[i + 1] || *end != '\0' ||
                nice_value < -20 || nice_value > 19) {
                fprintf(stderr, "invalid value for --nice: %s\n", argv[i + 1]);
                return -1;
            }
            req->nice_value = (int)nice_value;
            continue;
        }

        fprintf(stderr, "unknown option: %s\n", argv[i]);
        return -1;
    }

    if (req->soft_limit_bytes > req->hard_limit_bytes) {
        fprintf(stderr, "soft limit cannot exceed hard limit\n");
        return -1;
    }

    return 0;
}

static int bounded_buffer_init(bounded_buffer_t *buffer)
{
    int rc;

    memset(buffer, 0, sizeof(*buffer));

    rc = pthread_mutex_init(&buffer->mutex, NULL);
    if (rc != 0) {
        errno = rc;
        return -1;
    }
    rc = pthread_cond_init(&buffer->not_empty, NULL);
    if (rc != 0) {
        errno = rc;
        pthread_mutex_destroy(&buffer->mutex);
        return -1;
    }
    rc = pthread_cond_init(&buffer->not_full, NULL);
    if (rc != 0) {
        errno = rc;
        pthread_cond_destroy(&buffer->not_empty);
        pthread_mutex_destroy(&buffer->mutex);
        return -1;
    }
    return 0;
}

static void bounded_buffer_shutdown(bounded_buffer_t *buffer)
{
    pthread_mutex_lock(&buffer->mutex);
    buffer->shutting_down = 1;
    pthread_cond_broadcast(&buffer->not_empty);
    pthread_cond_broadcast(&buffer->not_full);
    pthread_mutex_unlock(&buffer->mutex);
}

static void bounded_buffer_destroy(bounded_buffer_t *buffer)
{
    pthread_cond_destroy(&buffer->not_full);
    pthread_cond_destroy(&buffer->not_empty);
    pthread_mutex_destroy(&buffer->mutex);
}

static int bounded_buffer_push(bounded_buffer_t *buffer, const log_item_t *item)
{
    pthread_mutex_lock(&buffer->mutex);
    while (buffer->count == LOG_BUFFER_CAPACITY && !buffer->shutting_down) {
        pthread_cond_wait(&buffer->not_full, &buffer->mutex);
    }
    if (buffer->shutting_down) {
        pthread_mutex_unlock(&buffer->mutex);
        return -1;
    }

    buffer->items[buffer->tail] = *item;
    buffer->tail = (buffer->tail + 1) % LOG_BUFFER_CAPACITY;
    buffer->count++;

    pthread_cond_signal(&buffer->not_empty);
    pthread_mutex_unlock(&buffer->mutex);
    return 0;
}

static int bounded_buffer_pop(bounded_buffer_t *buffer, log_item_t *item)
{
    pthread_mutex_lock(&buffer->mutex);
    while (buffer->count == 0 && !buffer->shutting_down) {
        pthread_cond_wait(&buffer->not_empty, &buffer->mutex);
    }

    if (buffer->count == 0 && buffer->shutting_down) {
        pthread_mutex_unlock(&buffer->mutex);
        return -1;
    }

    *item = buffer->items[buffer->head];
    buffer->head = (buffer->head + 1) % LOG_BUFFER_CAPACITY;
    buffer->count--;

    pthread_cond_signal(&buffer->not_full);
    pthread_mutex_unlock(&buffer->mutex);
    return 0;
}

static void *log_consumer_thread(void *arg)
{
    supervisor_ctx_t *ctx = arg;
    log_item_t item;

    while (bounded_buffer_pop(&ctx->log_buffer, &item) == 0) {
        char path[PATH_MAX];
        int fd;

        snprintf(path, sizeof(path), "%s/%s.log", LOG_DIR, item.container_id);
        fd = open(path, O_CREAT | O_WRONLY | O_APPEND, 0644);
        if (fd < 0) {
            continue;
        }

        (void)write_full(fd, item.data, item.length);
        close(fd);
    }

    return NULL;
}

static void *log_producer_thread(void *arg)
{
    producer_arg_t *producer_arg = arg;
    supervisor_ctx_t *ctx = producer_arg->ctx;
    container_record_t *container = producer_arg->container;
    char chunk[LOG_CHUNK_SIZE];
    ssize_t nread;

    free(producer_arg);

    for (;;) {
        log_item_t item;

        nread = read(container->log_pipe_read_fd, chunk, sizeof(chunk));
        if (nread < 0) {
            if (errno == EINTR) {
                continue;
            }
            break;
        }
        if (nread == 0) {
            break;
        }

        memset(&item, 0, sizeof(item));
        strncpy(item.container_id, container->id, sizeof(item.container_id) - 1);
        item.length = (size_t)nread;
        memcpy(item.data, chunk, item.length);

        if (bounded_buffer_push(&ctx->log_buffer, &item) != 0) {
            break;
        }
    }

    if (container->log_pipe_read_fd >= 0) {
        close(container->log_pipe_read_fd);
        container->log_pipe_read_fd = -1;
    }

    return NULL;
}

static int child_fn(void *arg)
{
    child_config_t *cfg = arg;
    int devnull_fd;

    if (sethostname(cfg->id, strlen(cfg->id)) < 0) {
        perror("sethostname");
        return 1;
    }

    if (mount(NULL, "/", NULL, MS_REC | MS_PRIVATE, NULL) < 0) {
        perror("mount private");
        return 1;
    }

    if (chdir(cfg->rootfs) < 0) {
        perror("chdir rootfs");
        return 1;
    }

    if (chroot(".") < 0) {
        perror("chroot");
        return 1;
    }

    if (chdir("/") < 0) {
        perror("chdir /");
        return 1;
    }

    if (mkdir("/proc", 0555) < 0 && errno != EEXIST) {
        perror("mkdir /proc");
        return 1;
    }

    if (mount("proc", "/proc", "proc", 0, NULL) < 0) {
        perror("mount /proc");
        return 1;
    }

    if (cfg->nice_value != 0 && setpriority(PRIO_PROCESS, 0, cfg->nice_value) < 0) {
        perror("setpriority");
    }

    if (dup2(cfg->log_write_fd, STDOUT_FILENO) < 0 ||
        dup2(cfg->log_write_fd, STDERR_FILENO) < 0) {
        perror("dup2");
        return 1;
    }

    devnull_fd = open("/dev/null", O_RDONLY);
    if (devnull_fd >= 0) {
        (void)dup2(devnull_fd, STDIN_FILENO);
        close(devnull_fd);
    }

    if (cfg->log_write_fd > STDERR_FILENO) {
        close(cfg->log_write_fd);
    }

    execl("/bin/sh", "/bin/sh", "-c", cfg->command, (char *)NULL);
    perror("exec");
    return 1;
}

static int register_with_monitor(int monitor_fd,
                                 const char *container_id,
                                 pid_t host_pid,
                                 unsigned long soft_limit_bytes,
                                 unsigned long hard_limit_bytes)
{
    struct monitor_request req;

    if (monitor_fd < 0) {
        return 0;
    }

    memset(&req, 0, sizeof(req));
    req.pid = host_pid;
    req.soft_limit_bytes = soft_limit_bytes;
    req.hard_limit_bytes = hard_limit_bytes;
    strncpy(req.container_id, container_id, sizeof(req.container_id) - 1);

    if (ioctl(monitor_fd, MONITOR_REGISTER, &req) < 0) {
        return -1;
    }
    return 0;
}

static void unregister_from_monitor(int monitor_fd, const char *container_id, pid_t host_pid)
{
    struct monitor_request req;

    if (monitor_fd < 0) {
        return;
    }

    memset(&req, 0, sizeof(req));
    req.pid = host_pid;
    strncpy(req.container_id, container_id, sizeof(req.container_id) - 1);
    (void)ioctl(monitor_fd, MONITOR_UNREGISTER, &req);
}

static container_record_t *find_container_by_id_locked(supervisor_ctx_t *ctx, const char *id)
{
    container_record_t *cur;

    for (cur = ctx->containers; cur; cur = cur->next) {
        if (strncmp(cur->id, id, sizeof(cur->id)) == 0) {
            return cur;
        }
    }
    return NULL;
}

static container_record_t *find_container_by_pid_locked(supervisor_ctx_t *ctx, pid_t pid)
{
    container_record_t *cur;

    for (cur = ctx->containers; cur; cur = cur->next) {
        if (cur->host_pid == pid) {
            return cur;
        }
    }
    return NULL;
}

static int rootfs_in_use_locked(supervisor_ctx_t *ctx, const char *rootfs)
{
    container_record_t *cur;

    for (cur = ctx->containers; cur; cur = cur->next) {
        if (strncmp(cur->rootfs, rootfs, sizeof(cur->rootfs)) == 0 &&
            (cur->state == CONTAINER_STARTING || cur->state == CONTAINER_RUNNING)) {
            return 1;
        }
    }
    return 0;
}

static int send_response_and_close(int fd, const control_response_t *resp)
{
    int rc = write_full(fd, resp, sizeof(*resp));
    close(fd);
    return rc;
}

static int start_container(supervisor_ctx_t *ctx,
                           const control_request_t *req,
                           int wait_client_fd,
                           control_response_t *resp)
{
    container_record_t *container;
    child_config_t *cfg;
    producer_arg_t *producer_arg;
    int pipefd[2] = {-1, -1};
    pid_t pid;

    if (ensure_log_dir() != 0) {
        set_response(resp, -1, 0, 0, 1, "failed to create log directory");
        return -1;
    }

    pthread_mutex_lock(&ctx->metadata_lock);
    if (find_container_by_id_locked(ctx, req->container_id)) {
        pthread_mutex_unlock(&ctx->metadata_lock);
        set_response(resp, -1, 0, 0, 1, "container id already exists");
        return -1;
    }
    if (rootfs_in_use_locked(ctx, req->rootfs)) {
        pthread_mutex_unlock(&ctx->metadata_lock);
        set_response(resp, -1, 0, 0, 1, "rootfs already in use by a running container");
        return -1;
    }
    pthread_mutex_unlock(&ctx->metadata_lock);

    if (pipe(pipefd) < 0) {
        set_response(resp, -1, 0, 0, 1, "pipe failed: %s", strerror(errno));
        return -1;
    }

    container = calloc(1, sizeof(*container));
    cfg = calloc(1, sizeof(*cfg));
    producer_arg = calloc(1, sizeof(*producer_arg));
    if (!container || !cfg || !producer_arg) {
        set_response(resp, -1, 0, 0, 1, "allocation failed");
        free(container);
        free(cfg);
        free(producer_arg);
        close(pipefd[0]);
        close(pipefd[1]);
        return -1;
    }

    strncpy(container->id, req->container_id, sizeof(container->id) - 1);
    strncpy(container->rootfs, req->rootfs, sizeof(container->rootfs) - 1);
    strncpy(container->command, req->command, sizeof(container->command) - 1);
    snprintf(container->log_path, sizeof(container->log_path), "%s/%s.log", LOG_DIR, req->container_id);
    container->started_at = time(NULL);
    container->soft_limit_bytes = req->soft_limit_bytes;
    container->hard_limit_bytes = req->hard_limit_bytes;
    container->nice_value = req->nice_value;
    container->state = CONTAINER_STARTING;
    container->log_pipe_read_fd = pipefd[0];
    container->wait_client_fd = wait_client_fd;
    container->child_stack = malloc(STACK_SIZE);
    if (!container->child_stack) {
        set_response(resp, -1, 0, 0, 1, "stack allocation failed");
        free(container);
        free(cfg);
        free(producer_arg);
        close(pipefd[0]);
        close(pipefd[1]);
        return -1;
    }

    strncpy(cfg->id, req->container_id, sizeof(cfg->id) - 1);
    strncpy(cfg->rootfs, req->rootfs, sizeof(cfg->rootfs) - 1);
    strncpy(cfg->command, req->command, sizeof(cfg->command) - 1);
    cfg->nice_value = req->nice_value;
    cfg->log_write_fd = pipefd[1];

    pid = clone(child_fn,
                (char *)container->child_stack + STACK_SIZE,
                CLONE_NEWPID | CLONE_NEWUTS | CLONE_NEWNS | SIGCHLD,
                cfg);
    if (pid < 0) {
        set_response(resp, -1, 0, 0, 1, "clone failed: %s", strerror(errno));
        free(container->child_stack);
        free(container);
        free(cfg);
        free(producer_arg);
        close(pipefd[0]);
        close(pipefd[1]);
        return -1;
    }

    container->host_pid = pid;
    close(pipefd[1]);
    free(cfg);

    if (register_with_monitor(ctx->monitor_fd,
                              container->id,
                              container->host_pid,
                              container->soft_limit_bytes,
                              container->hard_limit_bytes) == 0) {
        container->monitor_registered = 1;
    }

    producer_arg->ctx = ctx;
    producer_arg->container = container;
    if (pthread_create(&container->producer_thread, NULL, log_producer_thread, producer_arg) != 0) {
        set_response(resp, -1, 0, 0, 1, "failed to start log producer");
        if (container->monitor_registered) {
            unregister_from_monitor(ctx->monitor_fd, container->id, container->host_pid);
        }
        kill(pid, SIGKILL);
        free(container->child_stack);
        free(container);
        free(producer_arg);
        return -1;
    }

    pthread_mutex_lock(&ctx->metadata_lock);
    container->state = CONTAINER_RUNNING;
    container->next = ctx->containers;
    ctx->containers = container;
    pthread_mutex_unlock(&ctx->metadata_lock);

    set_response(resp, 0, 0, 0, wait_client_fd < 0, "started container %s pid=%d",
                 container->id, container->host_pid);
    return 0;
}

static void finish_run_waiter(container_record_t *container)
{
    control_response_t resp;
    int status_code = container->exit_code;

    if (container->wait_client_fd < 0) {
        return;
    }

    if (container->exit_signal > 0) {
        status_code = 128 + container->exit_signal;
    }

    set_response(&resp,
                 0,
                 status_code,
                 container->exit_signal,
                 1,
                 "container=%s state=%s pid=%d exit_code=%d signal=%d",
                 container->id,
                 state_to_string(container->state),
                 container->host_pid,
                 container->exit_code,
                 container->exit_signal);
    (void)send_response_and_close(container->wait_client_fd, &resp);
    container->wait_client_fd = -1;
}

static void reap_children(supervisor_ctx_t *ctx)
{
    int status;
    pid_t pid;

    for (;;) {
        container_record_t *container;

        pid = waitpid(-1, &status, WNOHANG);
        if (pid <= 0) {
            break;
        }

        pthread_mutex_lock(&ctx->metadata_lock);
        container = find_container_by_pid_locked(ctx, pid);
        if (!container) {
            pthread_mutex_unlock(&ctx->metadata_lock);
            continue;
        }

        if (WIFEXITED(status)) {
            container->exit_code = WEXITSTATUS(status);
            container->exit_signal = 0;
            container->state = container->stop_requested ? CONTAINER_STOPPED : CONTAINER_EXITED;
        } else if (WIFSIGNALED(status)) {
            container->exit_code = -1;
            container->exit_signal = WTERMSIG(status);
            if (container->stop_requested) {
                container->state = CONTAINER_STOPPED;
            } else if (container->exit_signal == SIGKILL) {
                container->state = CONTAINER_HARD_LIMIT_KILLED;
            } else {
                container->state = CONTAINER_KILLED;
            }
        }
        pthread_mutex_unlock(&ctx->metadata_lock);

        if (container->monitor_registered) {
            unregister_from_monitor(ctx->monitor_fd, container->id, container->host_pid);
            container->monitor_registered = 0;
        }
        if (!container->producer_joined) {
            pthread_join(container->producer_thread, NULL);
            container->producer_joined = 1;
        }
        finish_run_waiter(container);
    }
}

static int stop_container(supervisor_ctx_t *ctx, const char *id, control_response_t *resp)
{
    container_record_t *container;

    pthread_mutex_lock(&ctx->metadata_lock);
    container = find_container_by_id_locked(ctx, id);
    if (!container) {
        pthread_mutex_unlock(&ctx->metadata_lock);
        set_response(resp, -1, 0, 0, 1, "unknown container %s", id);
        return -1;
    }

    if (container->state != CONTAINER_STARTING && container->state != CONTAINER_RUNNING) {
        pthread_mutex_unlock(&ctx->metadata_lock);
        set_response(resp, -1, 0, 0, 1, "container %s is not running", id);
        return -1;
    }

    container->stop_requested = 1;
    pthread_mutex_unlock(&ctx->metadata_lock);

    if (kill(container->host_pid, SIGTERM) < 0) {
        set_response(resp, -1, 0, 0, 1, "failed to stop %s: %s", id, strerror(errno));
        return -1;
    }

    set_response(resp, 0, 0, 0, 1, "stop requested for %s", id);
    return 0;
}

static void append_text(char *dst, size_t dst_len, size_t *used, const char *fmt, ...)
{
    va_list ap;
    int written;

    if (*used >= dst_len) {
        return;
    }

    va_start(ap, fmt);
    written = vsnprintf(dst + *used, dst_len - *used, fmt, ap);
    va_end(ap);
    if (written < 0) {
        return;
    }

    if ((size_t)written >= dst_len - *used) {
        *used = dst_len;
    } else {
        *used += (size_t)written;
    }
}


static int handle_ps(supervisor_ctx_t *ctx, control_response_t *resp)
{
    size_t used = 0;
    container_record_t *cur;

    append_text(resp->payload, sizeof(resp->payload), &used,
                "%-16s %-16s %-10s %-8s %-5s %s\n",
                "ID", "STATE", "PID", "EXIT", "SIG", "CMD");

    pthread_mutex_lock(&ctx->metadata_lock);
    for (cur = ctx->containers; cur; cur = cur->next) {
        append_text(resp->payload, sizeof(resp->payload), &used,
                    "%-16.16s %-16s %-10d %-8d %-5d %s\n",
                    cur->id,
                    state_to_string(cur->state),
                    cur->host_pid,
                    cur->exit_code,
                    cur->exit_signal,
                    cur->command);
    }
    pthread_mutex_unlock(&ctx->metadata_lock);

    resp->status = 0;
    resp->is_final = 1;

    return 0;
}

static int handle_logs(supervisor_ctx_t *ctx, const char *id, control_response_t *resp)
{
    char path[PATH_MAX];
    int fd;
    ssize_t n;
    size_t used = 0;

    snprintf(path, sizeof(path), "%s/%s.log", LOG_DIR, id);
    fd = open(path, O_RDONLY);
    if (fd < 0) {
        set_response(resp, -1, 0, 0, 1, "failed to open log file for %s", id);
        return -1;
    }

    resp->status = 0;
    resp->is_final = 1;
    
    while (used < sizeof(resp->payload) - 1) {
        n = read(fd, resp->payload + used, sizeof(resp->payload) - 1 - used);
        if (n < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (n == 0) break;
        used += n;
    }
    resp->payload[used] = '\0';
    close(fd);
    return 0;
}

static void handle_client_connection(supervisor_ctx_t *ctx, int client_fd)
{
    control_request_t req;
    control_response_t resp;

    if (read_full(client_fd, &req, sizeof(req)) != 0) {
        close(client_fd);
        return;
    }

    memset(&resp, 0, sizeof(resp));

    switch (req.kind) {
    case CMD_START:
        if (start_container(ctx, &req, -1, &resp) == 0) {
            (void)send_response_and_close(client_fd, &resp);
        } else {
            (void)send_response_and_close(client_fd, &resp);
        }
        break;

    case CMD_RUN:
        if (start_container(ctx, &req, client_fd, &resp) == 0) {
            (void)write_full(client_fd, &resp, sizeof(resp));
            // Do not close client_fd here, it will be closed by finish_run_waiter
        } else {
            (void)send_response_and_close(client_fd, &resp);
        }
        break;

    case CMD_PS:
        handle_ps(ctx, &resp);
        (void)send_response_and_close(client_fd, &resp);
        break;

    case CMD_LOGS:
        handle_logs(ctx, req.container_id, &resp);
        (void)send_response_and_close(client_fd, &resp);
        break;

    case CMD_STOP:
        stop_container(ctx, req.container_id, &resp);
        (void)send_response_and_close(client_fd, &resp);
        break;

    default:
        set_response(&resp, -1, 0, 0, 1, "unknown command");
        (void)send_response_and_close(client_fd, &resp);
        break;
    }
}

static void sigchld_handler(int signum)
{
    (void)signum;
    g_sigchld_pending = 1;
}

static void sigterm_handler(int signum)
{
    (void)signum;
    g_shutdown_requested = 1;
}

static int supervisor_main(const char *rootfs)
{
    struct sockaddr_un addr;
    int listen_fd = -1;
    supervisor_ctx_t ctx;
    struct sigaction sa;
    container_record_t *cur;

    memset(&ctx, 0, sizeof(ctx));
    ctx.shutting_down = 0;
    strncpy(ctx.base_rootfs, rootfs, sizeof(ctx.base_rootfs) - 1);

    if (bounded_buffer_init(&ctx.log_buffer) != 0) {
        fprintf(stderr, "failed to initialize log buffer\n");
        return 1;
    }

    if (pthread_mutex_init(&ctx.metadata_lock, NULL) != 0) {
        fprintf(stderr, "failed to initialize metadata lock\n");
        return 1;
    }

    if (pthread_create(&ctx.consumer_thread, NULL, log_consumer_thread, &ctx) != 0) {
        fprintf(stderr, "failed to start log consumer\n");
        return 1;
    }

    ctx.monitor_fd = open("/dev/container_monitor", O_WRONLY);
    if (ctx.monitor_fd < 0) {
        fprintf(stderr, "warning: could not open /dev/container_monitor, running without limits\n");
    }

    unlink(SOCKET_PATH);
    listen_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (listen_fd < 0) {
        perror("socket");
        return 1;
    }

    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, SOCKET_PATH, sizeof(addr.sun_path) - 1);

    if (bind(listen_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind");
        return 1;
    }

    if (listen(listen_fd, 10) < 0) {
        perror("listen");
        return 1;
    }

    ctx.server_fd = listen_fd;
    g_ctx = &ctx;

    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = sigchld_handler;
    sa.sa_flags = SA_RESTART | SA_NOCLDSTOP;
    sigaction(SIGCHLD, &sa, NULL);

    sa.sa_handler = sigterm_handler;
    sa.sa_flags = 0;
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);

    printf("supervisor listening on %s\n", SOCKET_PATH);

    while (!g_shutdown_requested) {
        fd_set rfds;
        int maxfd = listen_fd;
        int rc;
        struct timeval tv = {0, 100000}; 

        FD_ZERO(&rfds);
        FD_SET(listen_fd, &rfds);

        rc = select(maxfd + 1, &rfds, NULL, NULL, &tv);
        if (rc < 0) {
            if (errno == EINTR) {
                if (g_sigchld_pending) {
                    g_sigchld_pending = 0;
                    reap_children(&ctx);
                }
                continue;
            }
            perror("select");
            break;
        }

        if (g_sigchld_pending) {
            g_sigchld_pending = 0;
            reap_children(&ctx);
        }

        if (rc > 0 && FD_ISSET(listen_fd, &rfds)) {
            int client_fd = accept(listen_fd, NULL, NULL);
            if (client_fd >= 0) {
                handle_client_connection(&ctx, client_fd);
            }
        }
    }

    printf("supervisor shutting down...\n");

    pthread_mutex_lock(&ctx.metadata_lock);
    cur = ctx.containers;
    while (cur) {
        if (cur->state == CONTAINER_STARTING || cur->state == CONTAINER_RUNNING) {
            kill(cur->host_pid, SIGTERM);
            cur->stop_requested = 1;
        }
        cur = cur->next;
    }
    pthread_mutex_unlock(&ctx.metadata_lock);

    int wait_iters = 0;
    while (wait_iters < 10) {
        reap_children(&ctx);
        int all_exited = 1;
        pthread_mutex_lock(&ctx.metadata_lock);
        for (cur = ctx.containers; cur; cur = cur->next) {
            if (cur->state == CONTAINER_STARTING || cur->state == CONTAINER_RUNNING) {
                all_exited = 0;
                break;
            }
        }
        pthread_mutex_unlock(&ctx.metadata_lock);
        if (all_exited) break;
        usleep(100000);
        wait_iters++;
    }

    bounded_buffer_shutdown(&ctx.log_buffer);
    pthread_join(ctx.consumer_thread, NULL);
    bounded_buffer_destroy(&ctx.log_buffer);

    if (ctx.monitor_fd >= 0) {
        close(ctx.monitor_fd);
    }
    close(listen_fd);
    unlink(SOCKET_PATH);
    
    cur = ctx.containers;
    while (cur) {
        container_record_t *next = cur->next;
        free(cur->child_stack);
        free(cur);
        cur = next;
    }

    return 0;
}

static int send_request_recv_responses(control_request_t *req, int print_payload)
{
    int fd;
    struct sockaddr_un addr;
    control_response_t resp;
    int final_status = 0;

    fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        perror("socket");
        return 1;
    }

    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, SOCKET_PATH, sizeof(addr.sun_path) - 1);

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("connect");
        close(fd);
        return 1;
    }

    if (write_full(fd, req, sizeof(*req)) != 0) {
        fprintf(stderr, "failed to send request\n");
        close(fd);
        return 1;
    }

    while (1) {
        if (read_full(fd, &resp, sizeof(resp)) != 0) {
            break;
        }

        if (print_payload && strlen(resp.payload) > 0) {
            printf("%s", resp.payload);
            if (resp.payload[strlen(resp.payload)-1] != '\n') {
                printf("\n");
            }
        } else if (resp.status != 0 && strlen(resp.payload) > 0) {
            fprintf(stderr, "Error: %s\n", resp.payload);
        }

        if (resp.is_final) {
            final_status = resp.status != 0 ? resp.status : (resp.exit_code != 0 ? resp.exit_code : (resp.exit_signal != 0 ? 128 + resp.exit_signal : 0));
            break;
        }
    }

    close(fd);
    return final_status;
}

static void client_sigint_handler(int signum)
{
    (void)signum;
    g_client_stop_requested = 1;
}

static int cmd_run(control_request_t *req)
{
    int fd;
    struct sockaddr_un addr;
    control_response_t resp;
    int final_status = 0;
    struct sigaction sa;

    fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        perror("socket");
        return 1;
    }

    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, SOCKET_PATH, sizeof(addr.sun_path) - 1);

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("connect");
        close(fd);
        return 1;
    }

    if (write_full(fd, req, sizeof(*req)) != 0) {
        fprintf(stderr, "failed to send request\n");
        close(fd);
        return 1;
    }

    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = client_sigint_handler;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    while (1) {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(fd, &rfds);

        struct timeval tv = {0, 100000};
        int rc = select(fd + 1, &rfds, NULL, NULL, &tv);

        if (g_client_stop_requested) {
            g_client_stop_requested = 0;
            
            control_request_t stop_req;
            memset(&stop_req, 0, sizeof(stop_req));
            stop_req.kind = CMD_STOP;
            strncpy(stop_req.container_id, req->container_id, sizeof(stop_req.container_id) - 1);
            
            int stop_fd = socket(AF_UNIX, SOCK_STREAM, 0);
            if (stop_fd >= 0) {
                struct sockaddr_un stop_addr;
                memset(&stop_addr, 0, sizeof(stop_addr));
                stop_addr.sun_family = AF_UNIX;
                strncpy(stop_addr.sun_path, SOCKET_PATH, sizeof(stop_addr.sun_path) - 1);
                if (connect(stop_fd, (struct sockaddr *)&stop_addr, sizeof(stop_addr)) == 0) {
                    write_full(stop_fd, &stop_req, sizeof(stop_req));
                }
                close(stop_fd);
            }
        }

        if (rc > 0 && FD_ISSET(fd, &rfds)) {
            if (read_full(fd, &resp, sizeof(resp)) != 0) {
                break;
            }
            if (strlen(resp.payload) > 0) {
                printf("%s", resp.payload);
                if (resp.payload[strlen(resp.payload)-1] != '\n') {
                    printf("\n");
                }
            }
            if (resp.is_final) {
                final_status = resp.status != 0 ? resp.status : (resp.exit_code != 0 ? resp.exit_code : (resp.exit_signal != 0 ? 128 + resp.exit_signal : 0));
                break;
            }
        } else if (rc < 0 && errno != EINTR) {
            perror("select");
            break;
        }
    }

    close(fd);
    return final_status;
}

int main(int argc, char *argv[])
{
    control_request_t req;

    signal(SIGPIPE, SIG_IGN);

    if (argc < 2) {
        usage(argv[0]);
        return 1;
    }

    memset(&req, 0, sizeof(req));
    req.soft_limit_bytes = DEFAULT_SOFT_LIMIT;
    req.hard_limit_bytes = DEFAULT_HARD_LIMIT;

    if (strcmp(argv[1], "supervisor") == 0) {
        if (argc != 3) {
            usage(argv[0]);
            return 1;
        }
        return supervisor_main(argv[2]);
    } else if (strcmp(argv[1], "start") == 0) {
        if (argc < 5) {
            usage(argv[0]);
            return 1;
        }
        req.kind = CMD_START;
        strncpy(req.container_id, argv[2], sizeof(req.container_id) - 1);
        strncpy(req.rootfs, argv[3], sizeof(req.rootfs) - 1);
        strncpy(req.command, argv[4], sizeof(req.command) - 1);
        if (parse_optional_flags(&req, argc, argv, 5) != 0) {
            return 1;
        }
        return send_request_recv_responses(&req, 1);
    } else if (strcmp(argv[1], "run") == 0) {
        if (argc < 5) {
            usage(argv[0]);
            return 1;
        }
        req.kind = CMD_RUN;
        strncpy(req.container_id, argv[2], sizeof(req.container_id) - 1);
        strncpy(req.rootfs, argv[3], sizeof(req.rootfs) - 1);
        strncpy(req.command, argv[4], sizeof(req.command) - 1);
        if (parse_optional_flags(&req, argc, argv, 5) != 0) {
            return 1;
        }
        return cmd_run(&req);
    } else if (strcmp(argv[1], "ps") == 0) {
        req.kind = CMD_PS;
        return send_request_recv_responses(&req, 1);
    } else if (strcmp(argv[1], "logs") == 0) {
        if (argc != 3) {
            usage(argv[0]);
            return 1;
        }
        req.kind = CMD_LOGS;
        strncpy(req.container_id, argv[2], sizeof(req.container_id) - 1);
        return send_request_recv_responses(&req, 1);
    } else if (strcmp(argv[1], "stop") == 0) {
        if (argc != 3) {
            usage(argv[0]);
            return 1;
        }
        req.kind = CMD_STOP;
        strncpy(req.container_id, argv[2], sizeof(req.container_id) - 1);
        return send_request_recv_responses(&req, 1);
    } else {
        usage(argv[0]);
        return 1;
    }
}

