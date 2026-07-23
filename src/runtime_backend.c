#define _GNU_SOURCE
#include "runtime_backend.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

typedef struct {
    char proc_root[PATH_MAX];
    char sysfs_root[PATH_MAX];
    RuntimeMonotonicMsFn monotonic_ms;
    void *clock_context;
} RuntimeBackendState;

static RuntimeBackendState state = {
    .proc_root = "/proc",
    .sysfs_root = "/sys",
};
static pthread_mutex_t state_mutex = PTHREAD_MUTEX_INITIALIZER;

static uint64_t system_monotonic_ms(void *context) {
    (void)context;
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) return 0;
    return (uint64_t)now.tv_sec * 1000u +
        (uint64_t)now.tv_nsec / 1000000u;
}

static int copy_root(char *destination, size_t size, const char *source) {
    if (!source || source[0] != '/') return -1;
    size_t length = strlen(source);
    while (length > 1 && source[length - 1] == '/') length--;
    if (length >= size) return -1;
    memcpy(destination, source, length);
    destination[length] = '\0';
    return 0;
}

int runtime_backend_configure(const char *proc_root, const char *sysfs_root,
                              RuntimeMonotonicMsFn monotonic_ms,
                              void *clock_context) {
    RuntimeBackendState next = {0};
    if (copy_root(next.proc_root, sizeof(next.proc_root), proc_root) != 0 ||
        copy_root(next.sysfs_root, sizeof(next.sysfs_root), sysfs_root) != 0) {
        errno = EINVAL;
        return -1;
    }
    next.monotonic_ms = monotonic_ms;
    next.clock_context = clock_context;
    pthread_mutex_lock(&state_mutex);
    state = next;
    pthread_mutex_unlock(&state_mutex);
    return 0;
}

void runtime_backend_reset(void) {
    (void)runtime_backend_configure("/proc", "/sys", NULL, NULL);
}

int runtime_backend_resolve(const char *path, char *resolved,
                            size_t resolved_size) {
    if (!path || !resolved || resolved_size == 0) {
        errno = EINVAL;
        return -1;
    }
    RuntimeBackendState snapshot;
    pthread_mutex_lock(&state_mutex);
    snapshot = state;
    pthread_mutex_unlock(&state_mutex);

    const char *root = NULL;
    const char *suffix = NULL;
    if (strcmp(path, "/proc") == 0 || strncmp(path, "/proc/", 6) == 0) {
        root = snapshot.proc_root;
        suffix = path + 5;
    } else if (strcmp(path, "/sys") == 0 ||
               strncmp(path, "/sys/", 5) == 0) {
        root = snapshot.sysfs_root;
        suffix = path + 4;
    }
    int written = root
        ? snprintf(resolved, resolved_size, "%s%s", root, suffix)
        : snprintf(resolved, resolved_size, "%s", path);
    if (written < 0 || (size_t)written >= resolved_size) {
        errno = ENAMETOOLONG;
        return -1;
    }
    return 0;
}

FILE *runtime_backend_fopen(const char *path, const char *mode) {
    char resolved[PATH_MAX];
    if (runtime_backend_resolve(path, resolved, sizeof(resolved)) != 0)
        return NULL;
    return fopen(resolved, mode);
}

DIR *runtime_backend_opendir(const char *path) {
    char resolved[PATH_MAX];
    if (runtime_backend_resolve(path, resolved, sizeof(resolved)) != 0)
        return NULL;
    return opendir(resolved);
}

int runtime_backend_open(const char *path, int flags) {
    char resolved[PATH_MAX];
    if (runtime_backend_resolve(path, resolved, sizeof(resolved)) != 0)
        return -1;
    return open(resolved, flags);
}

int runtime_backend_stat(const char *path, struct stat *st) {
    char resolved[PATH_MAX];
    if (runtime_backend_resolve(path, resolved, sizeof(resolved)) != 0)
        return -1;
    return stat(resolved, st);
}

int runtime_backend_access(const char *path, int mode) {
    char resolved[PATH_MAX];
    if (runtime_backend_resolve(path, resolved, sizeof(resolved)) != 0)
        return -1;
    return access(resolved, mode);
}

uint64_t runtime_backend_monotonic_ms(void) {
    RuntimeMonotonicMsFn clock_fn;
    void *context;
    pthread_mutex_lock(&state_mutex);
    clock_fn = state.monotonic_ms;
    context = state.clock_context;
    pthread_mutex_unlock(&state_mutex);
    return clock_fn ? clock_fn(context) : system_monotonic_ms(NULL);
}
