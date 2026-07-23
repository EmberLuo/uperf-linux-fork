#ifndef UPERF_FREQUENCY_RECOVERY_H
#define UPERF_FREQUENCY_RECOVERY_H

#include <stddef.h>

#include "sysfs_writer.h"

enum {
    FREQUENCY_RECOVERY_OK = 0,
    FREQUENCY_RECOVERY_NOT_NEEDED = 1,
    FREQUENCY_RECOVERY_ERROR = -1,
};

/* Restore every min/max pair recorded before a previous daemon instance
 * changed it.  Values are read back before the crash snapshot is removed.
 * On any failure the snapshot remains for a later retry. */
int frequency_recovery_restore(const char *state_path, SysfsWriter *writer,
                               size_t *restored_count);

#endif /* UPERF_FREQUENCY_RECOVERY_H */
