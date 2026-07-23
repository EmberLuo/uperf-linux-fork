#define _POSIX_C_SOURCE 200809L
#include "frequency_state.h"
#include "golden_trace.h"

#include <assert.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static void join_path(char *out, size_t size, const char *left,
                      const char *right) {
    int length = snprintf(out, size, "%s/%s", left, right);
    assert(length > 0 && (size_t)length < size);
}

static void make_dir(const char *path) {
    assert(mkdir(path, 0700) == 0);
}

static void write_file(const char *path, const char *value) {
    FILE *file = fopen(path, "w");
    assert(file != NULL);
    assert(fputs(value, file) >= 0);
    assert(fclose(file) == 0);
}

static long long read_value(const char *path) {
    FILE *file = fopen(path, "r");
    assert(file != NULL);
    long long value = 0;
    assert(fscanf(file, "%lld", &value) == 1);
    fclose(file);
    return value;
}

static void write_policy(const char *cpufreq, const char *name,
                         const char *cpus, const char *minimum,
                         const char *maximum, const char *opps) {
    char policy[256], path[320];
    join_path(policy, sizeof(policy), cpufreq, name);
    make_dir(policy);
    join_path(path, sizeof(path), policy, "related_cpus");
    write_file(path, cpus);
    join_path(path, sizeof(path), policy, "cpuinfo_min_freq");
    write_file(path, minimum);
    join_path(path, sizeof(path), policy, "cpuinfo_max_freq");
    write_file(path, maximum);
    join_path(path, sizeof(path), policy, "scaling_min_freq");
    write_file(path, minimum);
    join_path(path, sizeof(path), policy, "scaling_max_freq");
    write_file(path, maximum);
    join_path(path, sizeof(path), policy, "scaling_cur_freq");
    write_file(path, minimum);
    join_path(path, sizeof(path), policy, "scaling_available_frequencies");
    write_file(path, opps);
}

static void remove_policy(const char *cpufreq, const char *name) {
    static const char *files[] = {
        "related_cpus", "cpuinfo_min_freq", "cpuinfo_max_freq",
        "scaling_min_freq", "scaling_max_freq", "scaling_cur_freq",
        "scaling_available_frequencies",
    };
    char policy[256], path[320];
    join_path(policy, sizeof(policy), cpufreq, name);
    for (size_t i = 0; i < sizeof(files) / sizeof(files[0]); i++) {
        join_path(path, sizeof(path), policy, files[i]);
        assert(unlink(path) == 0);
    }
    assert(rmdir(policy) == 0);
}

static void write_proc_stat(const char *proc_root) {
    char path[256];
    join_path(path, sizeof(path), proc_root, "stat");
    FILE *file = fopen(path, "w");
    assert(file != NULL);
    long cpus = sysconf(_SC_NPROCESSORS_ONLN);
    assert(cpus > 0);
    for (long cpu = 0; cpu < cpus; cpu++)
        assert(fprintf(file, "cpu%ld 10 0 10 100 0 0 0 0 0 0\n", cpu) > 0);
    assert(fclose(file) == 0);
}

static void write_config(const char *path) {
    FILE *file = fopen(path, "w");
    assert(file != NULL);
    const char *json =
        "{\n"
        " \"meta\":{\"name\":\"daemon-recovery-test\",\"author\":\"tests\"},\n"
        " \"modules\":{\n"
        "  \"switcher\":{\"perapp\":\"/tmp/uperf-no-perapp\"},\n"
        "  \"input\":{\"enable\":false},\n"
        "  \"thermal\":{\"enabled\":false},\n"
        "  \"heavyload\":{\"enable\":true,\"sampleTimeMs\":1,"
            "\"heavyLoadPct\":60,\"idleLoadPct\":20,\"burstSlackMs\":0},\n"
        "  \"cpu\":{\"enable\":true,\"powerModel\":[\n"
        "   {\"efficiency\":300,\"nr\":1,\"cpumask\":\"prime\","
            "\"typicalFreq\":1000,\"sweetFreq\":900,\"freeFreq\":799},\n"
        "   {\"efficiency\":250,\"nr\":4,\"cpumask\":\"performance\","
            "\"typicalFreq\":1000,\"sweetFreq\":900,\"freeFreq\":699},\n"
        "   {\"efficiency\":180,\"nr\":3,\"cpumask\":\"efficiency\","
            "\"typicalFreq\":900,\"sweetFreq\":800,\"freeFreq\":499}\n"
        "  ]},\n"
        "  \"sysfs\":{\"knob\":{"
            "\"gpuMinFreq\":\"/sys/class/devfreq/gpu/min_freq\","
            "\"gpuMaxFreq\":\"/sys/class/devfreq/gpu/max_freq\"}},\n"
        "  \"sched\":{\"enable\":false,\"cpumask\":{"
            "\"all\":[0,1,2,3,4,5,6,7],\"prime\":[7],"
            "\"performance\":[3,4,5,6],\"efficiency\":[0,1,2]}},\n"
        "  \"cgroup\":{\"enable\":false}\n"
        " },\n"
        " \"initials\":{\"cpu\":{\"baseSampleTime\":0.01,"
            "\"margin\":0,\"burst\":0,\"limitEfficiency\":false}},\n"
        " \"presets\":{\"balance\":{\"*\":{\"cpu.margin\":0}},"
            "\"powersave\":{\"*\":{\"cpu.margin\":0}},"
            "\"performance\":{\"*\":{\"cpu.margin\":0}}}\n"
        "}\n";
    assert(fputs(json, file) >= 0);
    assert(fclose(file) == 0);
}

static pid_t spawn_daemon(const char *config, const char *proc_root,
                          const char *sysfs_root, const char *frequency_state,
                          const char *scheduler_state,
                          const char *cgroup_state, int recovery_only) {
    pid_t pid = fork();
    assert(pid >= 0);
    if (pid != 0) return pid;
    assert(setenv("UPERF_TEST_PROC_ROOT", proc_root, 1) == 0);
    assert(setenv("UPERF_TEST_SYSFS_ROOT", sysfs_root, 1) == 0);
    assert(setenv("UPERF_TEST_FREQUENCY_STATE", frequency_state, 1) == 0);
    assert(setenv("UPERF_TEST_SCHEDULER_STATE", scheduler_state, 1) == 0);
    assert(setenv("UPERF_TEST_CGROUP_STATE", cgroup_state, 1) == 0);
    assert(setenv("DBUS_SYSTEM_BUS_ADDRESS",
                  "unix:path=/tmp/uperf-test-no-system-bus", 1) == 0);
    if (recovery_only)
        assert(setenv("UPERF_TEST_RECOVERY_ONLY", "1", 1) == 0);
    else
        unsetenv("UPERF_TEST_RECOVERY_ONLY");
    execl(TEST_DAEMON_PATH, TEST_DAEMON_PATH, "-c", config, "-l", "4",
          (char *)NULL);
    _exit(127);
}

static void wait_for_frequency(const char *path, long long expected,
                               pid_t daemon) {
    struct timespec delay = {.tv_sec = 0, .tv_nsec = 10000000};
    for (int attempt = 0; attempt < 500; attempt++) {
        if (read_value(path) == expected) return;
        int status = 0;
        assert(waitpid(daemon, &status, WNOHANG) == 0);
        nanosleep(&delay, NULL);
    }
    assert(!"daemon did not apply the expected fake-sysfs frequency");
}

int main(void) {
    char root[] = "/tmp/uperf-daemon-recovery-XXXXXX";
    assert(mkdtemp(root) != NULL);
    char proc[192], sysfs[192], devices[256], system[256], cpu[256];
    char cpufreq[256], class_dir[256], devfreq[256], gpu[256];
    char config[256], frequency_state[256], scheduler_state[256];
    char cgroup_state[256], proc_stat[256], path[320];
    join_path(proc, sizeof(proc), root, "proc");
    join_path(sysfs, sizeof(sysfs), root, "sys");
    make_dir(proc);
    make_dir(sysfs);
    join_path(devices, sizeof(devices), sysfs, "devices"); make_dir(devices);
    join_path(system, sizeof(system), devices, "system"); make_dir(system);
    join_path(cpu, sizeof(cpu), system, "cpu"); make_dir(cpu);
    join_path(cpufreq, sizeof(cpufreq), cpu, "cpufreq"); make_dir(cpufreq);
    write_policy(cpufreq, "policy0", "0 1 2\n", "300000", "900000",
                 "300000 500000 900000\n");
    write_policy(cpufreq, "policy3", "3 4 5 6\n", "400000", "1000000",
                 "400000 700000 1000000\n");
    write_policy(cpufreq, "policy7", "7\n", "600000", "1000000",
                 "600000 800000 1000000\n");
    join_path(class_dir, sizeof(class_dir), sysfs, "class"); make_dir(class_dir);
    join_path(devfreq, sizeof(devfreq), class_dir, "devfreq"); make_dir(devfreq);
    join_path(gpu, sizeof(gpu), devfreq, "gpu"); make_dir(gpu);
    join_path(path, sizeof(path), gpu, "min_freq"); write_file(path, "200000000");
    join_path(path, sizeof(path), gpu, "max_freq"); write_file(path, "400000000");
    join_path(path, sizeof(path), gpu, "available_frequencies");
    write_file(path, "200000000 400000000\n");
    write_proc_stat(proc);
    join_path(proc_stat, sizeof(proc_stat), proc, "stat");
    join_path(config, sizeof(config), root, "config.json"); write_config(config);
    join_path(frequency_state, sizeof(frequency_state), root, "frequency-state");
    join_path(scheduler_state, sizeof(scheduler_state), root, "scheduler-state");
    join_path(cgroup_state, sizeof(cgroup_state), root, "cgroup-state");

    pid_t first = spawn_daemon(config, proc, sysfs, frequency_state,
                               scheduler_state, cgroup_state, 0);
    char policy0_min[320], policy3_min[320], policy7_min[320];
    join_path(path, sizeof(path), cpufreq, "policy0");
    join_path(policy0_min, sizeof(policy0_min), path, "scaling_min_freq");
    join_path(path, sizeof(path), cpufreq, "policy3");
    join_path(policy3_min, sizeof(policy3_min), path, "scaling_min_freq");
    join_path(path, sizeof(path), cpufreq, "policy7");
    join_path(policy7_min, sizeof(policy7_min), path, "scaling_min_freq");
    wait_for_frequency(policy0_min, 500000, first);
    wait_for_frequency(policy3_min, 700000, first);
    wait_for_frequency(policy7_min, 800000, first);

    FrequencyStateEntry entries[FREQUENCY_STATE_MAX_ENTRIES] = {0};
    size_t entry_count = 0;
    assert(frequency_state_load(frequency_state, entries,
                                FREQUENCY_STATE_MAX_ENTRIES, &entry_count) ==
           FREQUENCY_STATE_OK);
    assert(entry_count == 4);
    assert(kill(first, SIGKILL) == 0);
    int status = 0;
    assert(waitpid(first, &status, 0) == first);
    assert(WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL);

    long long killed0 = read_value(policy0_min);
    long long killed3 = read_value(policy3_min);
    long long killed7 = read_value(policy7_min);
    pid_t restart = spawn_daemon(config, proc, sysfs, frequency_state,
                                 scheduler_state, cgroup_state, 1);
    assert(waitpid(restart, &status, 0) == restart);
    assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);

    char gpu_min[320], gpu_max[320];
    join_path(gpu_min, sizeof(gpu_min), gpu, "min_freq");
    join_path(gpu_max, sizeof(gpu_max), gpu, "max_freq");
    char trace[320];
    snprintf(trace, sizeof(trace),
             "after_sigkill=%lld,%lld,%lld\n"
             "after_restart=%lld,%lld,%lld\n"
             "gpu_after_restart=%lld,%lld\n"
             "snapshot=cleared\n",
             killed0, killed3, killed7,
             read_value(policy0_min), read_value(policy3_min),
             read_value(policy7_min), read_value(gpu_min), read_value(gpu_max));
    assert(golden_trace_matches(
        TEST_SOURCE_DIR "/tests/golden/daemon_recovery.trace", trace) == 0);
    assert(access(frequency_state, F_OK) != 0 && errno == ENOENT);

    remove_policy(cpufreq, "policy0");
    remove_policy(cpufreq, "policy3");
    remove_policy(cpufreq, "policy7");
    join_path(path, sizeof(path), gpu, "min_freq"); assert(unlink(path) == 0);
    join_path(path, sizeof(path), gpu, "max_freq"); assert(unlink(path) == 0);
    join_path(path, sizeof(path), gpu, "available_frequencies");
    assert(unlink(path) == 0);
    assert(rmdir(gpu) == 0); assert(rmdir(devfreq) == 0);
    assert(rmdir(class_dir) == 0); assert(rmdir(cpufreq) == 0);
    assert(rmdir(cpu) == 0); assert(rmdir(system) == 0);
    assert(rmdir(devices) == 0); assert(rmdir(sysfs) == 0);
    assert(unlink(proc_stat) == 0); assert(rmdir(proc) == 0);
    assert(unlink(config) == 0); assert(rmdir(root) == 0);
    puts(trace);
    return 0;
}
