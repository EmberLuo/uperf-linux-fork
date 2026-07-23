#define _POSIX_C_SOURCE 200809L
#include "runtime_backend.h"
#include "sysfs_writer.h"

#include <assert.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static uint64_t fake_clock(void *context) {
    return *(const uint64_t *)context;
}

static void write_file(const char *path, const char *value) {
    FILE *file = fopen(path, "w");
    assert(file != NULL);
    assert(fputs(value, file) >= 0);
    assert(fclose(file) == 0);
}

int main(void) {
    char root[] = "/tmp/uperf-backend-XXXXXX";
    assert(mkdtemp(root) != NULL);
    char proc[128], sysfs[128], proc_stat[256], sys_devices[192], knob[256];
    snprintf(proc, sizeof(proc), "%s/proc", root);
    snprintf(sysfs, sizeof(sysfs), "%s/sys", root);
    snprintf(proc_stat, sizeof(proc_stat), "%s/stat", proc);
    snprintf(sys_devices, sizeof(sys_devices), "%s/devices", sysfs);
    snprintf(knob, sizeof(knob), "%s/frequency", sys_devices);
    assert(mkdir(proc, 0700) == 0);
    assert(mkdir(sysfs, 0700) == 0);
    assert(mkdir(sys_devices, 0700) == 0);
    write_file(proc_stat, "cpu0 1 2 3 4 5 6 7 8\n");
    write_file(knob, "300");

    uint64_t now = 424242;
    assert(runtime_backend_configure(proc, sysfs, fake_clock, &now) == 0);
    assert(runtime_backend_monotonic_ms() == now);

    FILE *stat_file = runtime_backend_fopen("/proc/stat", "r");
    assert(stat_file != NULL);
    char line[80];
    assert(fgets(line, sizeof(line), stat_file) != NULL);
    fclose(stat_file);
    assert(strncmp(line, "cpu0 ", 5) == 0);

    Config config = {0};
    SysfsWriter *writer = sysfs_writer_new(&config);
    assert(writer != NULL);
    assert(sysfs_writer_write_raw(writer, "/sys/devices/frequency", "900") ==
           0);
    char *value = sysfs_reader_read("/sys/devices/frequency");
    assert(value != NULL && strcmp(value, "900") == 0);
    free(value);
    sysfs_writer_free(writer);

    runtime_backend_reset();
    unlink(knob);
    unlink(proc_stat);
    rmdir(sys_devices);
    rmdir(sysfs);
    rmdir(proc);
    rmdir(root);
    puts("runtime_backend: procfs, sysfs and clock injection passed");
    return 0;
}
