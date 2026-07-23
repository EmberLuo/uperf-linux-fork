#ifndef UPERF_RUNTIME_BACKEND_H
#define UPERF_RUNTIME_BACKEND_H

#include <dirent.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/stat.h>

/* Process-global runtime boundary.  The daemon has one platform view; tests
 * may replace its procfs/sysfs roots and monotonic clock before constructing
 * components, then call reset afterwards. */
typedef uint64_t (*RuntimeMonotonicMsFn)(void *context);

int runtime_backend_configure(const char *proc_root, const char *sysfs_root,
                              RuntimeMonotonicMsFn monotonic_ms,
                              void *clock_context);
void runtime_backend_reset(void);

/* Map logical /proc/... and /sys/... paths into the configured roots.  Other
 * absolute paths are copied unchanged. */
int runtime_backend_resolve(const char *path, char *resolved,
                            size_t resolved_size);

FILE *runtime_backend_fopen(const char *path, const char *mode);
DIR *runtime_backend_opendir(const char *path);
int runtime_backend_open(const char *path, int flags);
int runtime_backend_stat(const char *path, struct stat *st);
int runtime_backend_access(const char *path, int mode);
uint64_t runtime_backend_monotonic_ms(void);

#endif /* UPERF_RUNTIME_BACKEND_H */
