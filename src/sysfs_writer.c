#define _GNU_SOURCE
#include "sysfs_writer.h"
#include "log.h"
#include "config.h"
#include "runtime_backend.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>

/* Internal sysfs writer state.
 *
 * Writes are accumulated in `pending` and emitted on flush (or when the queue
 * fills).  There is no time-based batch window: the historical batch_window_ns
 * parameter never influenced flushing, so it is not stored.  There is also no
 * value de-duplication — every queued write is emitted verbatim. */
struct SysfsWriter {
    WriteRequest pending[SYSFS_BATCH_MAX];
    int nr_pending;
    SysfsWriteFn write_fn;
    void *write_context;
};

static ssize_t default_write(int fd, const void *buffer, size_t length,
                             void *context) {
    (void)context;
    return write(fd, buffer, length);
}

SysfsWriter *sysfs_writer_new_with_io(const Config *cfg,
                                      const SysfsWriterIo *io) {
    (void)cfg;
    SysfsWriter *w = calloc(1, sizeof(*w));
    if (!w) return NULL;

    w->nr_pending = 0;
    w->write_fn = io && io->write_fn ? io->write_fn : default_write;
    w->write_context = io ? io->context : NULL;

    log_info("SysfsWriter created (count-based flush, no batch window)");
    return w;
}

SysfsWriter *sysfs_writer_new(const Config *cfg) {
    return sysfs_writer_new_with_io(cfg, NULL);
}

void sysfs_writer_free(SysfsWriter *w) {
    if (!w) return;
    if (sysfs_writer_flush(w) != 0)
        log_error("sysfs_writer: final flush failed during destruction");
    free(w);
    log_debug("SysfsWriter destroyed");
}

/* Write a single value to a sysfs path */
static int write_sysfs_value(SysfsWriter *w, const char *path,
                             const char *value) {
    /* O_TRUNC matches the `echo value > knob` semantic every userspace tool
     * uses: a shorter value must fully replace a longer previous one.  On real
     * sysfs O_TRUNC is a harmless no-op, but against any regular-file backing
     * (tests, or a knob exposed as a plain file) it prevents leftover trailing
     * digits, e.g. writing "600000" over "1000000" reading back as "6000000". */
    int fd = runtime_backend_open(path, O_WRONLY | O_TRUNC);
    if (fd < 0) {
        log_error("sysfs_writer: cannot write %s: %s", path, strerror(errno));
        return -1;
    }
    size_t len = strlen(value);
    ssize_t n = w->write_fn(fd, value, len, w->write_context);
    if (n < 0) {
        log_error("sysfs_writer: write to %s failed: %s", path, strerror(errno));
        close(fd);
        return -1;
    }
    /* sysfs attribute writes are expected to consume the whole buffer in one
     * call. A short write means the kernel handler rejected part of the value;
     * treat it as failure rather than silently applying a truncated value (and
     * do NOT retry — many sysfs knobs reject a second write() for one value). */
    if ((size_t)n != len) {
        log_error("sysfs_writer: short write to %s (%zd/%zu bytes)",
                  path, n, len);
        close(fd);
        return -1;
    }
    close(fd);
    return 0;
}

int sysfs_writer_flush(SysfsWriter *w) {
    if (!w) return -1;

    /* Process pending writes */
    int failures = 0;
    for (int i = 0; i < w->nr_pending; i++) {
        if (w->pending[i].has_value) {
            if (write_sysfs_value(w, w->pending[i].path,
                                  w->pending[i].value) != 0)
                failures++;
        }
    }
    w->nr_pending = 0;
    return failures == 0 ? 0 : -1;
}

int sysfs_writer_queue_raw(SysfsWriter *w, const char *path, const char *value) {
    if (!w) return -1;
    if (!path || !value) return -1;
    if (w->nr_pending >= SYSFS_BATCH_MAX) {
        if (sysfs_writer_flush(w) != 0) return -1;
    }

    WriteRequest *req = &w->pending[w->nr_pending++];
    strncpy(req->path, path, MAX_PATH_LEN - 1);
    req->path[MAX_PATH_LEN - 1] = '\0';
    strncpy(req->value, value, MAX_PATH_LEN - 1);
    req->value[MAX_PATH_LEN - 1] = '\0';
    req->has_value = true;

    return 0;
}

int sysfs_writer_write_raw(SysfsWriter *w, const char *path, const char *value) {
    if (!w || !path || !value) return -1;
    return write_sysfs_value(w, path, value);
}

char *sysfs_reader_read(const char *path) {
    if (!path) return NULL;

    FILE *fp = runtime_backend_fopen(path, "r");
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
