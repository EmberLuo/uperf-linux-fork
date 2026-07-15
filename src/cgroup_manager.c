#define _GNU_SOURCE
#include "cgroup_manager.h"
#include "log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <linux/sched.h>
#include <linux/sched/types.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/types.h>

#define CGROUP_ROOT "/sys/fs/cgroup"
#define CGROUP_PATH_LEN (MAX_PATH_LEN + 64)

/* Internal cgroup manager state */
struct CgroupManager {
    bool available;
    char root_path[CGROUP_PATH_LEN];
    char paths[SLICE_NUM][CGROUP_PATH_LEN];
};

/* sched_{get,set}attr wrappers were added to glibc after the syscalls and are
 * not available on Ubuntu 24.04.  Call the Linux ABI directly so the daemon
 * builds consistently across libc versions. */
static int uperf_sched_getattr(pid_t pid, struct sched_attr *attr,
                               unsigned int size, unsigned int flags) {
#ifdef SYS_sched_getattr
    return (int)syscall(SYS_sched_getattr, pid, attr, size, flags);
#else
    (void)pid;
    (void)attr;
    (void)size;
    (void)flags;
    errno = ENOSYS;
    return -1;
#endif
}

static int uperf_sched_setattr(pid_t pid, struct sched_attr *attr,
                               unsigned int flags) {
#ifdef SYS_sched_setattr
    return (int)syscall(SYS_sched_setattr, pid, attr, flags);
#else
    (void)pid;
    (void)attr;
    (void)flags;
    errno = ENOSYS;
    return -1;
#endif
}

static bool find_own_cgroup(char *path, size_t size) {
    FILE *fp = fopen("/proc/self/cgroup", "r");
    if (!fp) return false;
    char line[CGROUP_PATH_LEN];
    bool found = false;
    while (fgets(line, sizeof(line), fp)) {
        if (strncmp(line, "0::", 3) != 0) continue;
        char *relative = line + 3;
        relative[strcspn(relative, "\r\n")] = '\0';
        int written = snprintf(path, size, "%s%s", CGROUP_ROOT, relative);
        found = written > 0 && written < (int)size;
        break;
    }
    fclose(fp);
    return found;
}

static bool make_child_path(char *out, size_t size, const char *parent,
                            const char *child) {
    int written = snprintf(out, size, "%s/%s", parent, child);
    return written > 0 && written < (int)size;
}

bool cgroup_manager_is_available(void) {
    struct stat st;
    if (stat(CGROUP_ROOT, &st) != 0 || !S_ISDIR(st.st_mode))
        return false;

    /* Check if cgroup2 is mounted */
    FILE *fp = fopen("/proc/mounts", "r");
    if (!fp) return false;

    char line[256];
    bool found = false;
    while (fgets(line, sizeof(line), fp)) {
        if (strstr(line, "cgroup2") && strstr(line, "/sys/fs/cgroup")) {
            found = true;
            break;
        }
    }
    fclose(fp);
    return found;
}

CgroupManager *cgroup_manager_new(void) {
    CgroupManager *cm = calloc(1, sizeof(*cm));
    if (!cm) return NULL;

    cm->available = cgroup_manager_is_available();
    if (cm->available && !find_own_cgroup(cm->root_path,
                                          sizeof(cm->root_path)))
        cm->available = false;
    if (!cm->available) {
        log_warn("cgroup v2 not mounted — cgroup_manager will fall back to prctl");
    }

    /* Initialize paths */
    if (cm->available &&
        (!make_child_path(cm->paths[SLICE_GAME], sizeof(cm->paths[SLICE_GAME]),
                          cm->root_path, "game") ||
         !make_child_path(cm->paths[SLICE_SYSTEM],
                          sizeof(cm->paths[SLICE_SYSTEM]), cm->root_path,
                          "system") ||
         !make_child_path(cm->paths[SLICE_BACKGROUND],
                          sizeof(cm->paths[SLICE_BACKGROUND]), cm->root_path,
                          "background")))
        cm->available = false;

    log_info("CgroupManager created: available=%d", cm->available);
    return cm;
}

void cgroup_manager_free(CgroupManager *cm) {
    if (!cm) return;
    log_debug("CgroupManager destroyed");
    free(cm);
}

static int mkdir_recursive(const char *path, mode_t mode) {
    /* Simple mkdir — for cgroup hierarchy, parents should already exist */
    if (mkdir(path, mode) < 0 && errno != EEXIST) {
        log_error("mkdir %s: %s", path, strerror(errno));
        return -1;
    }
    return 0;
}

static int write_file(const char *path, const char *value) {
    int fd = open(path, O_WRONLY);
    if (fd < 0) {
        log_warn("write %s: %s", path, strerror(errno));
        return -1;
    }
    size_t len = strlen(value);
    ssize_t n = write(fd, value, len);
    close(fd);
    if (n < 0 || (size_t)n != len) {
        log_warn("write %s: partial/write error: %s", path, strerror(errno));
        return -1;
    }
    return 0;
}

int cgroup_manager_init(CgroupManager *cm) {
    if (!cm || !cm->available) return -1;

    /* systemd Delegate=yes grants access inside the service's own cgroup, not
     * at /sys/fs/cgroup. Create children there and move the daemon into the
     * system child before enabling controllers at the now-empty parent. */
    for (int i = 0; i < SLICE_NUM; i++) {
        if (mkdir_recursive(cm->paths[i], 0755) < 0)
            return -1;
    }

    char path[CGROUP_PATH_LEN + 32];
    if (!make_child_path(path, sizeof(path), cm->paths[SLICE_SYSTEM],
                         "cgroup.procs"))
        return -1;
    char pid_text[32];
    snprintf(pid_text, sizeof(pid_text), "%d", getpid());
    if (write_file(path, pid_text) < 0) return -1;

    if (!make_child_path(path, sizeof(path), cm->root_path,
                         "cgroup.subtree_control"))
        return -1;
    if (write_file(path, "+cpu") < 0) return -1;

    log_info("CgroupManager initialized under delegated cgroup: %s",
             cm->root_path);
    return 0;
}

int cgroup_manager_assign_pid(CgroupManager *cm, pid_t pid, CgroupSlice slice) {
    if (!cm || slice < 0 || slice >= SLICE_NUM) return -1;

    if (cm->available) {
        /* Write PID to cgroup.procs in the target slice */
        char path[CGROUP_PATH_LEN + 32];
        if (!make_child_path(path, sizeof(path), cm->paths[slice],
                             "cgroup.procs")) return -1;
        char buf[16];
        snprintf(buf, sizeof(buf), "%d", pid);
        return write_file(path, buf);
    }

    /* Fallback: just log, no cgroup available */
    log_warn("cgroup v2 not available — cannot assign PID %d to slice %d", pid, slice);
    return -1;
}

int cgroup_manager_set_slice_cpus(CgroupManager *cm, CgroupSlice slice,
                                   uint64_t cpu_mask) {
    if (!cm || slice < 0 || slice >= SLICE_NUM) return -1;

    if (cm->available) {
        char path[CGROUP_PATH_LEN + 32];
        if (!make_child_path(path, sizeof(path), cm->paths[slice],
                             "cpuset.cpus")) return -1;
        char buf[64];
        size_t offset = 0;
        for (int cpu = 0; cpu < 64; cpu++) {
            if (!(cpu_mask & (UINT64_C(1) << cpu))) continue;
            int written = snprintf(buf + offset, sizeof(buf) - offset,
                                   "%s%d", offset ? "," : "", cpu);
            if (written < 0 || written >= (int)(sizeof(buf) - offset))
                return -1;
            offset += (size_t)written;
        }
        if (offset == 0) return -1;
        return write_file(path, buf);
    }
    return -1;
}

int cgroup_manager_set_slice_weight(CgroupManager *cm, CgroupSlice slice,
                                     int weight) {
    if (!cm || slice < 0 || slice >= SLICE_NUM) return -1;
    if (weight < 1) weight = 1;
    if (weight > 10000) weight = 10000;

    if (cm->available) {
        char path[CGROUP_PATH_LEN + 32];
        if (!make_child_path(path, sizeof(path), cm->paths[slice],
                             "cpu.weight")) return -1;
        char buf[16];
        snprintf(buf, sizeof(buf), "%d", weight);
        return write_file(path, buf);
    }
    return -1;
}

int cgroup_manager_set_slice_uclamp(CgroupManager *cm, CgroupSlice slice,
                                     int uclamp_min, int uclamp_max) {
    if (!cm || slice < 0 || slice >= SLICE_NUM) return -1;
    if (uclamp_min < 0) uclamp_min = 0;
    if (uclamp_min > 1024) uclamp_min = 1024;
    if (uclamp_max < 0) uclamp_max = 0;
    if (uclamp_max > 1024) uclamp_max = 1024;

    if (cm->available) {
        char path_min[CGROUP_PATH_LEN + 32], path_max[CGROUP_PATH_LEN + 32];
        if (!make_child_path(path_min, sizeof(path_min), cm->paths[slice],
                             "cpu.uclamp.min") ||
            !make_child_path(path_max, sizeof(path_max), cm->paths[slice],
                             "cpu.uclamp.max")) return -1;

        char buf[16];
        snprintf(buf, sizeof(buf), "%.2f", uclamp_min * 100.0 / 1024.0);
        int min_result = write_file(path_min, buf);
        snprintf(buf, sizeof(buf), "%.2f", uclamp_max * 100.0 / 1024.0);
        int max_result = write_file(path_max, buf);
        return min_result == 0 && max_result == 0 ? 0 : -1;
    }
    return -1;
}

const char *cgroup_manager_get_path(const CgroupManager *cm, CgroupSlice slice) {
    if (!cm || slice < 0 || slice >= SLICE_NUM) return "(null)";
    return cm->paths[slice];
}

int cgroup_manager_set_pid_uclamp(pid_t pid, int uclamp_min, int uclamp_max) {
    if (pid <= 0 || uclamp_min < 0 || uclamp_min > 1024 ||
        uclamp_max < 0 || uclamp_max > 1024 || uclamp_min > uclamp_max) {
        errno = EINVAL;
        return -1;
    }

    /* Read-modify-write preserves the process's scheduler policy, nice value,
     * RT priority and deadline fields while changing only utilization clamps. */
    struct sched_attr attr;
    memset(&attr, 0, sizeof(attr));
    attr.size = sizeof(attr);
    if (uperf_sched_getattr(pid, &attr, sizeof(attr), 0) < 0) {
        log_error("sched_getattr(%d) failed: %s", pid, strerror(errno));
        return -1;
    }
    attr.sched_flags |= SCHED_FLAG_UTIL_CLAMP_MIN |
                        SCHED_FLAG_UTIL_CLAMP_MAX;
    attr.sched_util_min = (uint32_t)uclamp_min;
    attr.sched_util_max = (uint32_t)uclamp_max;
    if (uperf_sched_setattr(pid, &attr, 0) < 0) {
        log_error("sched_setattr(%d, uclamp=[%d,%d]) failed: %s",
                  pid, uclamp_min, uclamp_max, strerror(errno));
        return -1;
    }
    return 0;
}
