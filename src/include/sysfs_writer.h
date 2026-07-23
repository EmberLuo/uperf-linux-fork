#ifndef UPERF_SYSFS_WRITER_H
#define UPERF_SYSFS_WRITER_H

#include <stdint.h>
#include <stddef.h>
#include <sys/types.h>
#include "config.h"

/* Maximum batch size: number of pending writes before forced flush */
#define SYSFS_BATCH_MAX  64

/* Write request queued for batching */
typedef struct {
    char path[MAX_PATH_LEN];
    char value[MAX_PATH_LEN];
    bool has_value;  /* false = delete/close (e.g. set governor to "" to reset) */
} WriteRequest;

/* SysfsWriter opaque handle */
typedef struct SysfsWriter SysfsWriter;

/* Injectable low-level write hook.  Production uses write(2); tests can force
 * deterministic EIO/short-write behavior without depending on kernel timing. */
typedef ssize_t (*SysfsWriteFn)(int fd, const void *buffer, size_t length,
                                void *context);
typedef struct {
    SysfsWriteFn write_fn;
    void *context;
} SysfsWriterIo;

/* Create a new sysfs writer backed by the given config.
 * Queued writes accumulate until sysfs_writer_flush() is called or the queue
 * reaches SYSFS_BATCH_MAX entries; there is no time-based batching.
 * Returns NULL on failure. */
SysfsWriter *sysfs_writer_new(const Config *cfg);

/* Same writer with an injected write operation. NULL/empty IO uses write(2). */
SysfsWriter *sysfs_writer_new_with_io(const Config *cfg,
                                      const SysfsWriterIo *io);

/* Destroy and free a sysfs writer. Flushes any pending writes first. */
void sysfs_writer_free(SysfsWriter *w);

/* Force an immediate flush of all pending writes. Every entry is attempted;
 * returns -1 if any write failed, otherwise 0. */
int sysfs_writer_flush(SysfsWriter *w);

/* Queue a single raw write. Returns 0 on success. */
int sysfs_writer_queue_raw(SysfsWriter *w, const char *path, const char *value);

/* Write a value immediately and report the kernel write result. This is used
 * for explicit user requests where success must not be acknowledged early. */
int sysfs_writer_write_raw(SysfsWriter *w, const char *path, const char *value);

/* Read the current value of a sysfs path (trailing whitespace stripped).
 * Returns a newly-allocated string, or NULL on failure. Caller frees. */
char *sysfs_reader_read(const char *path);

#endif /* UPERF_SYSFS_WRITER_H */
