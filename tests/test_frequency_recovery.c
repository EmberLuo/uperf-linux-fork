#define _POSIX_C_SOURCE 200809L
#include "frequency_recovery.h"
#include "frequency_state.h"
#include "runtime_backend.h"
#include "sysfs_writer.h"
#include "golden_trace.h"

#include <assert.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

static void write_file(const char *path, const char *value) {
    FILE *file = fopen(path, "w");
    assert(file != NULL);
    assert(fputs(value, file) >= 0);
    assert(fclose(file) == 0);
}

static long long read_logical_value(const char *path) {
    char *text = sysfs_reader_read(path);
    assert(text != NULL);
    char *end = NULL;
    long long value = strtoll(text, &end, 10);
    assert(end != text && *end == '\0');
    free(text);
    return value;
}

int main(void) {
    char root[] = "/tmp/uperf-recovery-XXXXXX";
    assert(mkdtemp(root) != NULL);
    char proc[128], sysfs[128], policy[192], min_file[256], max_file[256];
    char state_file[256];
    snprintf(proc, sizeof(proc), "%s/proc", root);
    snprintf(sysfs, sizeof(sysfs), "%s/sys", root);
    snprintf(policy, sizeof(policy), "%s/policy0", sysfs);
    snprintf(min_file, sizeof(min_file), "%s/min", policy);
    snprintf(max_file, sizeof(max_file), "%s/max", policy);
    snprintf(state_file, sizeof(state_file), "%s/frequency-state", root);
    assert(mkdir(proc, 0700) == 0);
    assert(mkdir(sysfs, 0700) == 0);
    assert(mkdir(policy, 0700) == 0);
    write_file(min_file, "300");
    write_file(max_file, "900");
    assert(runtime_backend_configure(proc, sysfs, NULL, NULL) == 0);

    int ready[2];
    assert(pipe(ready) == 0);
    pid_t first = fork();
    assert(first >= 0);
    if (first == 0) {
        close(ready[0]);
        FrequencyStateEntry entry = {0};
        strcpy(entry.min_path, "/sys/policy0/min");
        strcpy(entry.max_path, "/sys/policy0/max");
        strcpy(entry.original_min, "300");
        strcpy(entry.original_max, "900");
        assert(frequency_state_save(state_file, &entry, 1) ==
               FREQUENCY_STATE_OK);
        Config config = {0};
        SysfsWriter *writer = sysfs_writer_new(&config);
        assert(writer != NULL);
        assert(sysfs_writer_write_raw(writer, entry.max_path, "700") == 0);
        assert(sysfs_writer_write_raw(writer, entry.min_path, "500") == 0);
        assert(write(ready[1], "R", 1) == 1);
        for (;;) pause();
    }
    close(ready[1]);
    char marker = 0;
    assert(read(ready[0], &marker, 1) == 1 && marker == 'R');
    close(ready[0]);
    assert(kill(first, SIGKILL) == 0);
    int status = 0;
    assert(waitpid(first, &status, 0) == first);
    assert(WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL);
    long long killed_min = read_logical_value("/sys/policy0/min");
    long long killed_max = read_logical_value("/sys/policy0/max");

    pid_t restart = fork();
    assert(restart >= 0);
    if (restart == 0) {
        Config config = {0};
        SysfsWriter *writer = sysfs_writer_new(&config);
        assert(writer != NULL);
        size_t restored = 0;
        assert(frequency_recovery_restore(state_file, writer, &restored) ==
               FREQUENCY_RECOVERY_OK);
        assert(restored == 1);
        sysfs_writer_free(writer);
        _exit(0);
    }
    assert(waitpid(restart, &status, 0) == restart);
    assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);

    long long restored_min = read_logical_value("/sys/policy0/min");
    long long restored_max = read_logical_value("/sys/policy0/max");
    char trace[192];
    snprintf(trace, sizeof(trace),
             "after_sigkill=%lld,%lld\nafter_restart=%lld,%lld\nsnapshot=cleared\n",
             killed_min, killed_max, restored_min, restored_max);
    assert(golden_trace_matches(
        TEST_SOURCE_DIR "/tests/golden/frequency_recovery.trace", trace) == 0);
    assert(access(state_file, F_OK) != 0 && errno == ENOENT);

    /* A failed restart must preserve the snapshot for the next retry. */
    FrequencyStateEntry broken = {0};
    strcpy(broken.min_path, "/sys/policy0/min");
    strcpy(broken.max_path, "/sys/policy0/missing-max");
    strcpy(broken.original_min, "300");
    strcpy(broken.original_max, "900");
    assert(frequency_state_save(state_file, &broken, 1) ==
           FREQUENCY_STATE_OK);
    Config config = {0};
    SysfsWriter *writer = sysfs_writer_new(&config);
    assert(writer != NULL);
    assert(frequency_recovery_restore(state_file, writer, NULL) ==
           FREQUENCY_RECOVERY_ERROR);
    assert(access(state_file, F_OK) == 0);
    sysfs_writer_free(writer);
    assert(frequency_state_clear(state_file) == FREQUENCY_STATE_OK);

    runtime_backend_reset();
    unlink(min_file);
    unlink(max_file);
    rmdir(policy);
    rmdir(sysfs);
    rmdir(proc);
    rmdir(root);
    puts(trace);
    return 0;
}
