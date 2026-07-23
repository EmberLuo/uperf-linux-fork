#include "frequency_recovery.h"

#include "frequency_state.h"
#include "log.h"

#include <errno.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

static int parse_value(const char *text, long long *value) {
    if (!text || !value || text[0] == '\0') return -1;
    char *end = NULL;
    errno = 0;
    long long parsed = strtoll(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' || parsed <= 0) return -1;
    *value = parsed;
    return 0;
}

static int write_changed(SysfsWriter *writer, const char *path,
                         const char *current, const char *desired) {
    if (strcmp(current, desired) == 0) return 0;
    return sysfs_writer_write_raw(writer, path, desired);
}

static int restore_entry(SysfsWriter *writer,
                         const FrequencyStateEntry *entry) {
    char *current_min = sysfs_reader_read(entry->min_path);
    char *current_max = sysfs_reader_read(entry->max_path);
    long long current_minimum = 0, current_maximum = 0;
    long long desired_minimum = 0, desired_maximum = 0;
    if (!current_min || !current_max ||
        parse_value(current_min, &current_minimum) != 0 ||
        parse_value(current_max, &current_maximum) != 0 ||
        parse_value(entry->original_min, &desired_minimum) != 0 ||
        parse_value(entry->original_max, &desired_maximum) != 0 ||
        current_minimum > current_maximum || desired_minimum > desired_maximum) {
        free(current_min);
        free(current_max);
        return -1;
    }

    int first, second;
    if (desired_maximum < current_minimum) {
        first = write_changed(writer, entry->min_path, current_min,
                              entry->original_min);
        second = first == 0
            ? write_changed(writer, entry->max_path, current_max,
                            entry->original_max) : -1;
    } else {
        first = write_changed(writer, entry->max_path, current_max,
                              entry->original_max);
        second = first == 0
            ? write_changed(writer, entry->min_path, current_min,
                            entry->original_min) : -1;
    }
    if (first != 0 || second != 0) {
        free(current_min);
        free(current_max);
        return -1;
    }

    char *actual_min = sysfs_reader_read(entry->min_path);
    char *actual_max = sysfs_reader_read(entry->max_path);
    long long readback_min = 0, readback_max = 0;
    int result = actual_min && actual_max &&
        parse_value(actual_min, &readback_min) == 0 &&
        parse_value(actual_max, &readback_max) == 0 &&
        readback_min == desired_minimum && readback_max == desired_maximum
            ? 0 : -1;
    free(actual_min);
    free(actual_max);
    free(current_min);
    free(current_max);
    return result;
}

int frequency_recovery_restore(const char *state_path, SysfsWriter *writer,
                               size_t *restored_count) {
    if (restored_count) *restored_count = 0;
    if (!state_path || !writer) return FREQUENCY_RECOVERY_ERROR;

    FrequencyStateEntry entries[FREQUENCY_STATE_MAX_ENTRIES] = {0};
    size_t count = 0;
    int load = frequency_state_load(state_path, entries,
                                    FREQUENCY_STATE_MAX_ENTRIES, &count);
    if (load == FREQUENCY_STATE_NOT_FOUND)
        return FREQUENCY_RECOVERY_NOT_NEEDED;
    if (load != FREQUENCY_STATE_OK) return FREQUENCY_RECOVERY_ERROR;

    size_t restored = 0;
    for (size_t i = 0; i < count; i++) {
        if (restore_entry(writer, &entries[i]) != 0) {
            log_error("Failed to restore and verify frequency range %s / %s",
                      entries[i].min_path, entries[i].max_path);
            continue;
        }
        restored++;
    }
    if (restored_count) *restored_count = restored;
    if (restored != count) return FREQUENCY_RECOVERY_ERROR;
    return frequency_state_clear(state_path) == FREQUENCY_STATE_OK
        ? FREQUENCY_RECOVERY_OK : FREQUENCY_RECOVERY_ERROR;
}
