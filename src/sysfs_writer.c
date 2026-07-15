#define _GNU_SOURCE
#include "sysfs_writer.h"
#include "log.h"
#include "config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <time.h>

/* Helper: monotonic nanoseconds */
static uint64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec;
}

/* Internal sysfs writer state */
struct SysfsWriter {
    uint64_t batch_window_ns;
    uint64_t last_flush_ns;

    WriteRequest pending[SYSFS_BATCH_MAX];
    int nr_pending;

    const Config *cfg;

    /* Last written values for deduplication */
    char last_values[SYSFS_BATCH_MAX][MAX_PATH_LEN];
};

SysfsWriter *sysfs_writer_new(const Config *cfg, uint64_t batch_window_ns) {
    SysfsWriter *w = calloc(1, sizeof(*w));
    if (!w) return NULL;

    w->cfg = cfg;
    w->batch_window_ns = batch_window_ns;
    w->last_flush_ns = now_ns();
    w->nr_pending = 0;

    log_info("SysfsWriter created: batch_window=%llu ns",
             (unsigned long long)batch_window_ns);
    return w;
}

void sysfs_writer_free(SysfsWriter *w) {
    if (!w) return;
    sysfs_writer_flush(w);
    free(w);
    log_debug("SysfsWriter destroyed");
}

/* Write a single value to a sysfs path */
static int write_sysfs_value(const char *path, const char *value) {
    int fd = open(path, O_WRONLY);
    if (fd < 0) {
        if (errno == ENOENT) {
            log_warn("sysfs_writer: path not found %s, skipping", path);
        } else if (errno == EACCES || errno == EPERM) {
            log_error("sysfs_writer: permission denied for %s", path);
        } else {
            log_error("sysfs_writer: cannot write %s: %s", path, strerror(errno));
        }
        return -1;
    }
    ssize_t n = write(fd, value, strlen(value));
    if (n < 0) {
        log_error("sysfs_writer: write to %s failed: %s", path, strerror(errno));
        close(fd);
        return -1;
    }
    close(fd);
    return 0;
}

/* Queue a write if the value has changed (dedup check) */
static int queue_deduped(SysfsWriter *w, const char *path, const char *value) {
    char *current = sysfs_reader_read(path);
    if (current && strcmp(current, value) == 0) {
        free(current);
        return 0;
    }
    free(current);
    return sysfs_writer_queue_raw(w, path, value);
}

/* Compute cluster CPU ranges from power model */
static void compute_cluster_ranges(const Config *cfg,
                                    int cluster_start[MAX_CLUSTERS],
                                    int cluster_end[MAX_CLUSTERS]) {
    int cpu_offset = 0;
    for (int c = 0; c < cfg->cpu.nr_clusters; c++) {
        cluster_start[c] = cpu_offset;
        cpu_offset += cfg->cpu.power_model[c].nr_cores;
        cluster_end[c] = cpu_offset - 1;
    }
}

/* Resolve a knob name to its value string from ActionParams.
 * Returns 1 if a value was written to 'value', 0 if no matching field. */
static int resolve_knob_value(const KnobDef *knob, const ActionParams *params,
                               int cluster, char *value, size_t value_size) {
    if (strcmp(knob->name, "cpufreqMax") == 0) {
        if (!params->has_cpu_freq_max || params->cpu_freq_max[cluster] <= 0)
            return 0;
        snprintf(value, value_size, "%d", params->cpu_freq_max[cluster]);
        return 1;
    }
    if (strcmp(knob->name, "cpufreqMin") == 0) {
        if (!params->has_cpu_freq_min || params->cpu_freq_min[cluster] <= 0)
            return 0;
        snprintf(value, value_size, "%d", params->cpu_freq_min[cluster]);
        return 1;
    }
    if (strcmp(knob->name, "cpufreqGovernor") == 0) {
        if (!params->has_governor || params->governor[0] == '\0')
            return 0;
        strncpy(value, params->governor, value_size - 1);
        value[value_size - 1] = '\0';
        return 1;
    }
    if (strcmp(knob->name, "gpuMaxFreq") == 0) {
        if (!params->has_gpu_max_freq || params->gpu_max_freq <= 0)
            return 0;
        snprintf(value, value_size, "%d", params->gpu_max_freq);
        return 1;
    }
    if (strcmp(knob->name, "gpuMinFreq") == 0) {
        if (!params->has_gpu_min_freq || params->gpu_min_freq <= 0)
            return 0;
        snprintf(value, value_size, "%d", params->gpu_min_freq);
        return 1;
    }
    if (strcmp(knob->name, "gpuGovernor") == 0) {
        if (!params->has_governor || params->governor[0] == '\0')
            return 0;
        strncpy(value, params->governor, value_size - 1);
        value[value_size - 1] = '\0';
        return 1;
    }
    if (strcmp(knob->name, "memBwMax") == 0) {
        if (!params->has_ddr_max_freq || params->ddr_max_freq <= 0)
            return 0;
        snprintf(value, value_size, "%d", params->ddr_max_freq);
        return 1;
    }
    if (strcmp(knob->name, "socMaxFreq") == 0) {
        if (!params->has_ddr_max_freq || params->ddr_max_freq <= 0)
            return 0;
        snprintf(value, value_size, "%d", params->ddr_max_freq);
        return 1;
    }
    if (strcmp(knob->name, "uclampMin") == 0) {
        if (!params->has_uclamp_min || params->uclamp_min[cluster] <= 0)
            return 0;
        snprintf(value, value_size, "%d", params->uclamp_min[cluster]);
        return 1;
    }
    if (strcmp(knob->name, "uclampMax") == 0) {
        if (!params->has_uclamp_max || params->uclamp_max[cluster] <= 0)
            return 0;
        snprintf(value, value_size, "%d", params->uclamp_max[cluster]);
        return 1;
    }
    return 0;
}

void sysfs_writer_apply(const SysfsWriter *w, const ActionParams *params,
                        PowerMode mode) {
    if (!w || !params) return;
    (void)mode;

    /* Flush any pending writes before applying new params */
    if (w->nr_pending > 0) {
        sysfs_writer_flush((SysfsWriter*)w);
    }

    int cluster_start[MAX_CLUSTERS] = {0};
    int cluster_end[MAX_CLUSTERS] = {0};
    compute_cluster_ranges(w->cfg, cluster_start, cluster_end);

    for (int k = 0; k < w->cfg->sysfs.nr_knobs; k++) {
        const KnobDef *knob = &w->cfg->sysfs.knobs[k];
        if (!knob->enabled) continue;

        char value[32];

        switch (knob->type) {
            case KNOB_PERCPU:
                for (int c = 0; c < w->cfg->cpu.nr_clusters; c++) {
                    if (!resolve_knob_value(knob, params, c, value, sizeof(value)))
                        continue;
                    for (int cpu = cluster_start[c]; cpu <= cluster_end[c]; cpu++) {
                        char path[MAX_PATH_LEN];
                        snprintf(path, sizeof(path), knob->path, cpu);
                        queue_deduped((SysfsWriter*)w, path, value);
                    }
                }
                break;

            case KNOB_PERCLUSTER:
                for (int c = 0; c < w->cfg->cpu.nr_clusters; c++) {
                    if (!resolve_knob_value(knob, params, c, value, sizeof(value)))
                        continue;
                    char path[MAX_PATH_LEN];
                    snprintf(path, sizeof(path), knob->path, c);
                    queue_deduped((SysfsWriter*)w, path, value);
                }
                break;

            case KNOB_DEVFREQ:
                if (!resolve_knob_value(knob, params, 0, value, sizeof(value)))
                    continue;
                queue_deduped((SysfsWriter*)w, knob->path, value);
                break;

            case KNOB_UCLAMP:
                for (int c = 0; c < w->cfg->cpu.nr_clusters; c++) {
                    if (!resolve_knob_value(knob, params, c, value, sizeof(value)))
                        continue;
                    char path[MAX_PATH_LEN];
                    snprintf(path, sizeof(path), knob->path, c);
                    queue_deduped((SysfsWriter*)w, path, value);
                }
                break;

            case KNOB_STRING:
            case KNOB_FILE:
                if (!resolve_knob_value(knob, params, 0, value, sizeof(value)))
                    continue;
                queue_deduped((SysfsWriter*)w, knob->path, value);
                break;

            default:
                break;
        }
    }

    sysfs_writer_flush((SysfsWriter*)w);
    log_debug("sysfs_writer_apply: applied mode=%d", mode);
}

void sysfs_writer_flush(SysfsWriter *w) {
    if (!w) return;

    uint64_t now = now_ns();
    if (w->batch_window_ns > 0 && now - w->last_flush_ns < w->batch_window_ns) {
        return;  /* Still within batch window */
    }

    /* Process pending writes */
    for (int i = 0; i < w->nr_pending; i++) {
        if (w->pending[i].has_value) {
            write_sysfs_value(w->pending[i].path, w->pending[i].value);
        }
    }
    w->nr_pending = 0;
    w->last_flush_ns = now;
}

int sysfs_writer_queue_raw(SysfsWriter *w, const char *path, const char *value) {
    if (!w) return -1;
    if (!path || !value) return -1;
    if (w->nr_pending >= SYSFS_BATCH_MAX) {
        sysfs_writer_flush(w);
    }

    WriteRequest *req = &w->pending[w->nr_pending++];
    strncpy(req->path, path, MAX_PATH_LEN - 1);
    req->path[MAX_PATH_LEN - 1] = '\0';
    strncpy(req->value, value, MAX_PATH_LEN - 1);
    req->value[MAX_PATH_LEN - 1] = '\0';
    req->has_value = true;

    return 0;
}

char *sysfs_reader_read(const char *path) {
    if (!path) return NULL;

    FILE *fp = fopen(path, "r");
    if (!fp) return NULL;

    char buf[256];
    size_t n = fread(buf, 1, sizeof(buf) - 1, fp);
    fclose(fp);

    if (n == 0) return NULL;
    buf[n] = '\0';

    /* Strip trailing whitespace */
    while (n > 0 && (buf[n-1] == '\n' || buf[n-1] == '\r' || buf[n-1] == ' '))
        buf[--n] = '\0';

    char *result = strdup(buf);
    return result;
}
