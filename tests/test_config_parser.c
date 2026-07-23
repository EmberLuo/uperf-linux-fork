#define _POSIX_C_SOURCE 200809L
#include "../src/include/config.h"
#include "../src/include/log.h"
#include "../src/include/state_machine.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <assert.h>
#include <json-c/json.h>

static int tests_run = 0;
static int tests_passed = 0;
static int tests_failed = 0;

static struct json_object *load_json_for_mutation(void);
static int write_mutated_json(struct json_object *root, const char *suffix,
                              char *path, size_t path_size);

#define TEST(name) static void name(void)
#define RUN_TEST(name) do { \
    tests_run++; \
    printf("  TEST: %s ... ", #name); \
    name(); \
} while(0)
#define ASSERT_EQ(a, b, msg) do { \
    if ((a) != (b)) { \
        printf("FAIL (%s: expected %d, got %d)\n", msg, (b), (a)); \
        tests_failed++; return; \
    } \
} while(0)
#define ASSERT_OK(ret, msg) do { \
    if ((ret) != 0) { \
        printf("FAIL (%s: expected 0, got %d)\n", msg, (ret)); \
        tests_failed++; return; \
    } \
} while(0)
#define ASSERT_FAIL(ret, msg) do { \
    if ((ret) == 0) { \
        printf("FAIL (%s: expected non-zero, got 0)\n", msg); \
        tests_failed++; return; \
    } \
} while(0)
#define ASSERT_GT(a, b, msg) do { \
    double _actual = (double)(a); \
    double _limit = (double)(b); \
    if (_actual <= _limit) { \
        printf("FAIL (%s: expected >%.3f, got %.3f)\n", msg, _limit, _actual); \
        tests_failed++; return; \
    } \
} while(0)
#define ASSERT_NEAR(a, b, delta, msg) do { \
    double _actual = (double)(a); \
    double _expected = (double)(b); \
    double _delta = (double)(delta); \
    double _diff = _actual - _expected; \
    if (_diff < 0) _diff = -_diff; \
    if (_diff > _delta) { \
        printf("FAIL (%s: expected %.3f +/- %.3f, got %.3f)\n", \
               msg, _expected, _delta, _actual); \
        tests_failed++; return; \
    } \
} while(0)
#define ASSERT_PASS(msg) do { \
    printf("PASS\n"); tests_passed++; \
} while(0)

/* Test: config_load with a valid JSON file */
TEST(test_load_valid_config) {
    Config cfg;
    int ret = config_load(&cfg, "config/sm8550.json");
    ASSERT_OK(ret, "config_load valid file");
    ASSERT_EQ(cfg.cpu.nr_clusters, 3, "cluster count");
    ASSERT_PASS("valid config loaded");
}

/* Test: config_load with nonexistent file */
TEST(test_load_missing_file) {
    Config cfg;
    int ret = config_load(&cfg, "/nonexistent/path.json");
    ASSERT_FAIL(ret, "config_load missing file");
    ASSERT_PASS("correctly rejected missing file");
}

/* Test: config_load with invalid JSON */
TEST(test_load_invalid_json) {
    /* Write invalid JSON to a temp file */
    FILE *fp = fopen("/tmp/test_invalid.json", "w");
    if (fp) {
        fprintf(fp, "{ this is not valid json !!!");
        fclose(fp);
    }
    Config cfg;
    int ret = config_load(&cfg, "/tmp/test_invalid.json");
    ASSERT_FAIL(ret, "config_load invalid json");
    ASSERT_PASS("correctly rejected invalid JSON");
    if (fp) unlink("/tmp/test_invalid.json");
}

/* Test: config_validate with valid config */
TEST(test_validate_valid) {
    Config cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.cpu.nr_clusters = 3;
    for (int i = 0; i < cfg.cpu.nr_clusters; i++) {
        cfg.cpu.power_model[i].nr_cores = 1;
        cfg.cpu.power_model[i].efficiency = 350;
        cfg.cpu.power_model[i].typical_freq_mhz = 2400;
        cfg.cpu.power_model[i].sweet_freq_mhz = 1800;
        cfg.cpu.power_model[i].free_freq_mhz = 400;
    }
    cfg.presets[MODE_BALANCE].name[0] = 'b';

    int ret = config_validate(&cfg);
    ASSERT_OK(ret, "config_validate valid");
    ASSERT_PASS("valid config passes validation");
}

/* Test: config_validate with no clusters */
TEST(test_validate_no_clusters) {
    Config cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.cpu.nr_clusters = 0;

    int ret = config_validate(&cfg);
    ASSERT_FAIL(ret, "config_validate no clusters");
    ASSERT_PASS("correctly rejects no clusters");
}

/* Test: config_validate with negative efficiency */
TEST(test_validate_neg_efficiency) {
    Config cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.cpu.nr_clusters = 1;
    cfg.cpu.power_model[0].nr_cores = 1;
    cfg.cpu.power_model[0].efficiency = -100;
    cfg.cpu.power_model[0].typical_freq_mhz = 1000;
    cfg.presets[MODE_BALANCE].name[0] = 'b';

    int ret = config_validate(&cfg);
    ASSERT_FAIL(ret, "config_validate neg efficiency");
    ASSERT_PASS("correctly rejects negative efficiency");
}

/* Test: config_validate with zero frequency */
TEST(test_validate_zero_freq) {
    Config cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.cpu.nr_clusters = 1;
    cfg.cpu.power_model[0].nr_cores = 1;
    cfg.cpu.power_model[0].efficiency = 100;
    cfg.cpu.power_model[0].typical_freq_mhz = 0;
    cfg.presets[MODE_BALANCE].name[0] = 'b';

    int ret = config_validate(&cfg);
    ASSERT_FAIL(ret, "config_validate zero freq");
    ASSERT_PASS("correctly rejects zero frequency");
}

/* Test: config_validate with no presets */
TEST(test_validate_no_presets) {
    Config cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.cpu.nr_clusters = 1;
    cfg.cpu.power_model[0].nr_cores = 1;
    cfg.cpu.power_model[0].efficiency = 100;
    cfg.cpu.power_model[0].typical_freq_mhz = 1000;
    /* No presets defined */

    int ret = config_validate(&cfg);
    ASSERT_FAIL(ret, "config_validate no presets");
    ASSERT_PASS("correctly rejects no presets");
}

/* Test: config_validate with zero cores */
TEST(test_validate_zero_cores) {
    Config cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.cpu.nr_clusters = 1;
    cfg.cpu.power_model[0].nr_cores = 1;
    cfg.cpu.power_model[0].efficiency = 100;
    cfg.cpu.power_model[0].typical_freq_mhz = 1000;
    cfg.cpu.power_model[0].nr_cores = 0;
    cfg.presets[MODE_BALANCE].name[0] = 'b';

    int ret = config_validate(&cfg);
    ASSERT_FAIL(ret, "config_validate zero cores");
    ASSERT_PASS("correctly rejects zero cores");
}

/* Test: config_load parses power model correctly */
TEST(test_load_power_model) {
    Config cfg;
    int ret = config_load(&cfg, "config/sm8550.json");
    ASSERT_OK(ret, "config_load for PM test");
    ASSERT_EQ(cfg.cpu.power_model[0].efficiency, 350, "PM[0] efficiency");
    ASSERT_EQ(cfg.cpu.power_model[0].nr_cores, 1, "PM[0] nr_cores");
    ASSERT_EQ((int)cfg.cpu.power_model[0].cpu_mask, 0x80, "PM[0] CPU mask");
    ASSERT_NEAR(cfg.cpu.power_model[0].sweet_freq_mhz, 2218.0, 1.0, "PM[0] sweetFreq");
    ASSERT_EQ(cfg.cpu.power_model[1].nr_cores, 4, "PM[1] nr_cores");
    ASSERT_EQ((int)cfg.cpu.power_model[1].cpu_mask, 0x78, "PM[1] CPU mask");
    ASSERT_EQ(cfg.cpu.power_model[2].nr_cores, 3, "PM[2] nr_cores");
    ASSERT_EQ((int)cfg.cpu.power_model[2].cpu_mask, 0x07, "PM[2] CPU mask");
    ASSERT_PASS("power model parsed correctly");
}

/* Test: config_load parses sysfs knobs */
TEST(test_load_sysfs_knobs) {
    Config cfg;
    int ret = config_load(&cfg, "config/sm8550.json");
    ASSERT_OK(ret, "config_load for knobs test");
    ASSERT_GT(cfg.sysfs.nr_knobs, 0, "knobs count > 0");
    /* Check first knob has a name and path */
    ASSERT_GT(strlen(cfg.sysfs.knobs[0].name), 0, "knob[0] has name");
    ASSERT_GT(strlen(cfg.sysfs.knobs[0].path), 0, "knob[0] has path");
    ASSERT_PASS("sysfs knobs parsed correctly");
}

TEST(test_removed_and_unknown_keys_warn) {
    struct json_object *root = load_json_for_mutation();
    if (!root) { printf("FAIL (cannot load fixture)\n"); tests_failed++; return; }
    struct json_object *modules, *sysfs, *presets, *balance, *global;
    json_object_object_get_ex(root, "modules", &modules);
    json_object_object_get_ex(modules, "sysfs", &sysfs);
    json_object_object_add(sysfs, "enable", json_object_new_boolean(1));
    json_object_object_get_ex(root, "presets", &presets);
    json_object_object_get_ex(presets, "balance", &balance);
    json_object_object_get_ex(balance, "*", &global);
    json_object_object_add(global, "cpu.margni", json_object_new_double(0.5));
    char config_path[160];
    write_mutated_json(root, "warn-keys", config_path, sizeof(config_path));
    json_object_put(root);

    char log_path[] = "/tmp/uperf-config-warning-XXXXXX";
    int log_fd = mkstemp(log_path);
    int saved_stderr = dup(STDERR_FILENO);
    if (log_fd < 0 || saved_stderr < 0 || dup2(log_fd, STDERR_FILENO) < 0) {
        if (log_fd >= 0) close(log_fd);
        if (saved_stderr >= 0) close(saved_stderr);
        unlink(config_path);
        unlink(log_path);
        printf("FAIL (cannot capture warnings)\n"); tests_failed++; return;
    }
    Config cfg;
    int ret = config_load(&cfg, config_path);
    fflush(stderr);
    assert(dup2(saved_stderr, STDERR_FILENO) >= 0);
    close(saved_stderr);
    lseek(log_fd, 0, SEEK_SET);
    char output[8192] = {0};
    ssize_t length = read(log_fd, output, sizeof(output) - 1);
    close(log_fd);
    unlink(log_path);
    unlink(config_path);
    ASSERT_OK(ret, "deprecated keys remain non-fatal");
    if (length < 0 || !strstr(output, "modules.sysfs.enable") ||
        !strstr(output, "cpu.margni")) {
        printf("FAIL (missing deprecated/unknown key warning)\n");
        tests_failed++; return;
    }
    ASSERT_PASS("deprecated and unknown keys are visible");
}

TEST(test_official_config_has_no_sysfs_enable) {
    struct json_object *root = load_json_for_mutation();
    if (!root) { printf("FAIL (cannot load fixture)\n"); tests_failed++; return; }
    struct json_object *modules, *sysfs, *knobs, *input, *unused = NULL;
    json_object_object_get_ex(root, "modules", &modules);
    json_object_object_get_ex(modules, "sysfs", &sysfs);
    int present = json_object_object_get_ex(sysfs, "enable", &unused);
    int has_knobs = json_object_object_get_ex(sysfs, "knob", &knobs);
    int has_gpu_min = has_knobs &&
        json_object_object_get_ex(knobs, "gpuMinFreq", &unused);
    int has_gpu_max = has_knobs &&
        json_object_object_get_ex(knobs, "gpuMaxFreq", &unused);
    int has_dead_cpu_knob = has_knobs &&
        json_object_object_get_ex(knobs, "cpufreqGovernor", &unused);
    int has_heavyload = json_object_object_get_ex(
        modules, "heavyload", &unused);
    json_object_object_get_ex(modules, "input", &input);
    int has_dead_hold = json_object_object_get_ex(
        input, "holdEnterTime", &unused);
    json_object_put(root);
    ASSERT_EQ(present, 0, "dead sysfs.enable removed from shipped config");
    ASSERT_EQ(has_gpu_min, 1, "runtime-consumed GPU minimum path present");
    ASSERT_EQ(has_gpu_max, 1, "runtime-consumed GPU maximum path present");
    ASSERT_EQ(has_dead_cpu_knob, 0, "unused CPU governor path removed");
    ASSERT_EQ(has_heavyload, 1, "heavy-load behavior is explicitly configured");
    ASSERT_EQ(has_dead_hold, 0, "unused holdEnterTime removed");
    ASSERT_PASS("official config contains only runtime-consumed controls");
}

/* Test: config_load parses sched rules */
TEST(test_load_sched_rules) {
    Config cfg;
    int ret = config_load(&cfg, "config/sm8550.json");
    ASSERT_OK(ret, "config_load for rules test");
    ASSERT_GT(cfg.sched.nr_rules, 0, "rules count > 0");
    ASSERT_EQ(cfg.sched.nr_cpumasks, 4, "cpumask group count");
    ASSERT_EQ(cfg.sched.enable, 1, "sched enabled");
    ASSERT_EQ(cfg.sched.nr_affinity_profiles, 7, "affinity profile count");
    ASSERT_EQ(cfg.sched.nr_priority_profiles, 6, "priority profile count");
    ASSERT_EQ((int)cfg.sched.cpumasks[0].mask, 0xff, "all CPU mask");
    /* First rule should have a name and regex */
    ASSERT_GT(strlen(cfg.sched.rules[0].name), 0, "rule[0] has name");
    ASSERT_GT(strlen(cfg.sched.rules[0].regex), 0, "rule[0] has regex");
    ASSERT_EQ(cfg.sched.rules[0].nr_thread_rules, 3,
              "display thread rule count");
    ASSERT_EQ(cfg.sched.rules[1].match_game, 1, "game selector parsed");
    ASSERT_EQ(cfg.sched.rules[1].nr_thread_rules, 5,
              "game thread rule count");
    ASSERT_EQ(cfg.sched.rules[1].thread_rules[0].affinity_index, 6,
              "gtmain affinity reference resolved");
    ASSERT_EQ(cfg.cgroup.enable, 1, "cgroup enabled");
    ASSERT_EQ(cfg.cgroup.nr_classes, 3, "cgroup class count");
    ASSERT_EQ(cfg.cgroup.classes[0].cpu_weight, 200, "game CPU weight");
    ASSERT_EQ((int)cfg.cgroup.classes[2].cpu_mask, 0x07,
              "background CPU mask");
    ASSERT_PASS("sched rules parsed correctly");
}

static struct json_object *load_json_for_mutation(void) {
    struct json_object *root = json_object_from_file("config/sm8550.json");
    return root;
}

static int write_mutated_json(struct json_object *root, const char *suffix,
                              char *path, size_t path_size) {
    snprintf(path, path_size, "/tmp/uperf-config-%d-%s.json", getpid(),
             suffix);
    return json_object_to_file_ext(path, root, JSON_C_TO_STRING_PRETTY);
}

TEST(test_reject_invalid_sched_priority) {
    struct json_object *root = load_json_for_mutation();
    if (!root) { printf("FAIL (cannot load fixture)\n"); tests_failed++; return; }
    struct json_object *modules, *sched, *prio, *rtusr;
    json_object_object_get_ex(root, "modules", &modules);
    json_object_object_get_ex(modules, "sched", &sched);
    json_object_object_get_ex(sched, "prio", &prio);
    json_object_object_get_ex(prio, "rtusr", &rtusr);
    json_object_object_add(rtusr, "bg", json_object_new_int(99));
    char path[160];
    write_mutated_json(root, "bad-prio", path, sizeof(path));
    json_object_put(root);
    Config cfg;
    int ret = config_load(&cfg, path);
    unlink(path);
    ASSERT_FAIL(ret, "priority 99 must be rejected");
    ASSERT_PASS("invalid priority rejected");
}

TEST(test_reject_unknown_affinity_cpumask) {
    struct json_object *root = load_json_for_mutation();
    if (!root) { printf("FAIL (cannot load fixture)\n"); tests_failed++; return; }
    struct json_object *modules, *sched, *affinity, *ui;
    json_object_object_get_ex(root, "modules", &modules);
    json_object_object_get_ex(modules, "sched", &sched);
    json_object_object_get_ex(sched, "affinity", &affinity);
    json_object_object_get_ex(affinity, "ui", &ui);
    json_object_object_add(ui, "touch", json_object_new_string("missing"));
    char path[160];
    write_mutated_json(root, "bad-mask", path, sizeof(path));
    json_object_put(root);
    Config cfg;
    int ret = config_load(&cfg, path);
    unlink(path);
    ASSERT_FAIL(ret, "unknown affinity cpumask must be rejected");
    ASSERT_PASS("unknown affinity cpumask rejected");
}

TEST(test_reject_invalid_sched_regex) {
    struct json_object *root = load_json_for_mutation();
    if (!root) { printf("FAIL (cannot load fixture)\n"); tests_failed++; return; }
    struct json_object *modules, *sched, *rules, *first;
    json_object_object_get_ex(root, "modules", &modules);
    json_object_object_get_ex(modules, "sched", &sched);
    json_object_object_get_ex(sched, "rules", &rules);
    first = json_object_array_get_idx(rules, 0);
    json_object_object_add(first, "regex", json_object_new_string("["));
    char path[160];
    write_mutated_json(root, "bad-regex", path, sizeof(path));
    json_object_put(root);
    Config cfg;
    int ret = config_load(&cfg, path);
    unlink(path);
    ASSERT_FAIL(ret, "invalid process regex must be rejected");
    ASSERT_PASS("invalid regex rejected");
}

TEST(test_reject_invalid_cgroup_class) {
    struct json_object *root = load_json_for_mutation();
    if (!root) { printf("FAIL (cannot load fixture)\n"); tests_failed++; return; }
    struct json_object *modules, *cgroup, *classes, *game;
    json_object_object_get_ex(root, "modules", &modules);
    json_object_object_get_ex(modules, "cgroup", &cgroup);
    json_object_object_get_ex(cgroup, "classes", &classes);
    json_object_object_get_ex(classes, "game", &game);
    json_object_object_add(game, "cpuWeight", json_object_new_int(0));
    char path[160];
    write_mutated_json(root, "bad-cgroup", path, sizeof(path));
    json_object_put(root);
    Config cfg;
    int ret = config_load(&cfg, path);
    unlink(path);
    ASSERT_FAIL(ret, "invalid cgroup weight must be rejected");
    ASSERT_PASS("invalid cgroup class rejected");
}

/* Attach a modules.heavyload object with the given key/value (float), load the
 * mutated config, and return config_load's status.  key==NULL adds an empty
 * heavyload block (defaults applied). */
static int load_with_heavyload_float(const char *suffix, const char *key,
                                     double value) {
    struct json_object *root = load_json_for_mutation();
    if (!root) return -999;
    struct json_object *modules;
    if (!json_object_object_get_ex(root, "modules", &modules)) {
        json_object_put(root);
        return -998;
    }
    struct json_object *heavy = json_object_new_object();
    if (key)
        json_object_object_add(heavy, key, json_object_new_double(value));
    json_object_object_add(modules, "heavyload", heavy);
    char path[160];
    write_mutated_json(root, suffix, path, sizeof(path));
    json_object_put(root);
    Config cfg;
    int ret = config_load(&cfg, path);
    unlink(path);
    /* Range checks live in config_validate; run both like the daemon does. */
    if (ret == 0) ret = config_validate(&cfg);
    return ret;
}

/* Omitting the block entirely must apply the historical defaults and pass. */
TEST(test_heavyload_missing_uses_defaults) {
    struct json_object *root = load_json_for_mutation();
    if (!root) { printf("FAIL (cannot load fixture)\n"); tests_failed++; return; }
    struct json_object *modules;
    json_object_object_get_ex(root, "modules", &modules);
    json_object_object_del(modules, "heavyload");
    char path[160];
    write_mutated_json(root, "hl-missing", path, sizeof(path));
    json_object_put(root);
    Config cfg;
    int ret = config_load(&cfg, path);
    unlink(path);
    ASSERT_OK(ret, "config_load default fixture");
    ASSERT_EQ(cfg.heavyload.enable, true, "heavyload enabled by default");
    ASSERT_NEAR(cfg.heavyload.sample_time_ms, 10.0, 0.001, "default sampleTime");
    ASSERT_NEAR(cfg.heavyload.heavy_load_pct, 60.0, 0.001, "default heavyPct");
    ASSERT_NEAR(cfg.heavyload.idle_load_pct, 20.0, 0.001, "default idlePct");
    ASSERT_NEAR(cfg.heavyload.burst_slack_ms, 3000.0, 0.001, "default burst");
    ASSERT_PASS("missing heavyload block yields defaults");
}

/* An empty heavyload block also keeps defaults and validates. */
TEST(test_heavyload_empty_block_ok) {
    int ret = load_with_heavyload_float("hl-empty", NULL, 0);
    ASSERT_OK(ret, "empty heavyload block should load");
    ASSERT_PASS("empty heavyload block uses defaults");
}

/* idleLoadPct >= heavyLoadPct is contradictory and must be rejected. */
TEST(test_heavyload_idle_ge_heavy_rejected) {
    /* default heavyLoadPct is 60; set idle to 60 -> not strictly less. */
    int ret = load_with_heavyload_float("hl-idle-ge", "idleLoadPct", 60.0);
    ASSERT_FAIL(ret, "idle >= heavy must be rejected");
    ASSERT_PASS("idle >= heavy rejected");
}

/* Negative idleLoadPct is out of range. */
TEST(test_heavyload_negative_idle_rejected) {
    int ret = load_with_heavyload_float("hl-neg-idle", "idleLoadPct", -1.0);
    ASSERT_FAIL(ret, "negative idle must be rejected");
    ASSERT_PASS("negative idle rejected");
}

/* heavyLoadPct above 100 is out of range. */
TEST(test_heavyload_heavy_over_100_rejected) {
    int ret = load_with_heavyload_float("hl-over-100", "heavyLoadPct", 150.0);
    ASSERT_FAIL(ret, "heavy > 100 must be rejected");
    ASSERT_PASS("heavy > 100 rejected");
}

/* sampleTimeMs below the [1,1000] floor is rejected. */
TEST(test_heavyload_sample_time_too_small_rejected) {
    int ret = load_with_heavyload_float("hl-st-small", "sampleTimeMs", 0.0);
    ASSERT_FAIL(ret, "sampleTimeMs 0 must be rejected");
    ASSERT_PASS("sampleTimeMs below floor rejected");
}

/* sampleTimeMs above the ceiling is rejected. */
TEST(test_heavyload_sample_time_too_large_rejected) {
    int ret = load_with_heavyload_float("hl-st-large", "sampleTimeMs", 5000.0);
    ASSERT_FAIL(ret, "sampleTimeMs 5000 must be rejected");
    ASSERT_PASS("sampleTimeMs above ceiling rejected");
}

/* burstSlackMs above 60000 is rejected. */
TEST(test_heavyload_burst_slack_too_large_rejected) {
    int ret = load_with_heavyload_float("hl-burst", "burstSlackMs", 60001.0);
    ASSERT_FAIL(ret, "burstSlackMs > 60000 must be rejected");
    ASSERT_PASS("burstSlackMs above ceiling rejected");
}

/* A wrong JSON type (string where a float is expected) must not crash and the
 * value stays at its default, so the config still validates. */
TEST(test_heavyload_wrong_type_ignored) {
    struct json_object *root = load_json_for_mutation();
    if (!root) { printf("FAIL (cannot load fixture)\n"); tests_failed++; return; }
    struct json_object *modules;
    json_object_object_get_ex(root, "modules", &modules);
    struct json_object *heavy = json_object_new_object();
    json_object_object_add(heavy, "sampleTimeMs",
                           json_object_new_string("fast"));
    json_object_object_add(modules, "heavyload", heavy);
    char path[160];
    write_mutated_json(root, "hl-wrong-type", path, sizeof(path));
    json_object_put(root);
    Config cfg;
    int ret = config_load(&cfg, path);
    unlink(path);
    ASSERT_OK(ret, "wrong-type field should fall back to default");
    ASSERT_NEAR(cfg.heavyload.sample_time_ms, 10.0, 0.001,
                "sampleTime stays default on type mismatch");
    ASSERT_PASS("wrong JSON type ignored, default retained");
}

/* A disabled detector skips validation, so out-of-range values are tolerated. */
TEST(test_heavyload_disabled_skips_validation) {
    struct json_object *root = load_json_for_mutation();
    if (!root) { printf("FAIL (cannot load fixture)\n"); tests_failed++; return; }
    struct json_object *modules;
    json_object_object_get_ex(root, "modules", &modules);
    struct json_object *heavy = json_object_new_object();
    json_object_object_add(heavy, "enable", json_object_new_boolean(0));
    json_object_object_add(heavy, "idleLoadPct", json_object_new_double(90.0));
    json_object_object_add(heavy, "heavyLoadPct", json_object_new_double(10.0));
    json_object_object_add(modules, "heavyload", heavy);
    char path[160];
    write_mutated_json(root, "hl-disabled", path, sizeof(path));
    json_object_put(root);
    Config cfg;
    int ret = config_load(&cfg, path);
    unlink(path);
    ASSERT_OK(ret, "disabled detector config should load");
    ASSERT_EQ(cfg.heavyload.enable, false, "detector disabled");
    ret = config_validate(&cfg);
    ASSERT_OK(ret, "disabled detector should skip range checks");
    ASSERT_PASS("disabled heavyload skips validation");
}

/* Test: config_load parses switcher config */
TEST(test_load_switcher) {
    Config cfg;
    int ret = config_load(&cfg, "config/sm8550.json");
    ASSERT_OK(ret, "config_load for switcher test");
    ASSERT_GT(strlen(cfg.switcher.perapp_file), 0, "perapp set");
    /* Check hint durations are set */
    ASSERT_GT(cfg.switcher.hint_duration[SCENE_TOUCH], 0, "touch hint duration");
    ASSERT_PASS("switcher config parsed correctly");
}

/* Test: config_load parses input config */
TEST(test_load_input) {
    Config cfg;
    int ret = config_load(&cfg, "config/sm8550.json");
    ASSERT_OK(ret, "config_load for input test");
    ASSERT_EQ(cfg.input.enable, 1, "input enabled");
    ASSERT_GT(cfg.input.swipe_thd, 0, "swipe threshold > 0");
    ASSERT_PASS("input config parsed correctly");
}

/* Test: config_load parses initials */
TEST(test_load_initials) {
    Config cfg;
    int ret = config_load(&cfg, "config/sm8550.json");
    ASSERT_OK(ret, "config_load for initials test");
    /* These should have defaults from parse_initials */
    ASSERT_GT(cfg.initial_base_sample_time, 0, "baseSampleTime > 0");
    ASSERT_GT(cfg.initial_margin, 0, "margin > 0");
    ASSERT_PASS("initials parsed correctly");
}

/* Test: config_load parses presets */
TEST(test_load_presets) {
    Config cfg;
    int ret = config_load(&cfg, "config/sm8550.json");
    ASSERT_OK(ret, "config_load for presets test");
    /* Check all three presets exist */
    ASSERT_GT(strlen(cfg.presets[MODE_BALANCE].name), 0, "balance preset");
    ASSERT_GT(strlen(cfg.presets[MODE_POWERSAVE].name), 0, "powersave preset");
    ASSERT_GT(strlen(cfg.presets[MODE_PERFORMANCE].name), 0, "performance preset");
    ASSERT_PASS("presets parsed correctly");
}

TEST(test_validate_cgroup_requires_sched) {
    Config cfg;
    int ret = config_load(&cfg, "config/sm8550.json");
    ASSERT_OK(ret, "load cgroup/sched fixture");
    cfg.sched.enable = false;
    ret = config_validate(&cfg);
    ASSERT_FAIL(ret, "enabled cgroup must require sched classification");
    ASSERT_PASS("cgroup without sched rejected");
}

/* Test: a config with no meta.schemaVersion loads as legacy (version 0). */
TEST(test_schema_version_absent_is_legacy) {
    struct json_object *root = load_json_for_mutation();
    if (!root) { printf("FAIL (cannot load fixture)\n"); tests_failed++; return; }
    struct json_object *meta;
    json_object_object_get_ex(root, "meta", &meta);
    json_object_object_del(meta, "schemaVersion");
    char path[160];
    write_mutated_json(root, "schema-absent", path, sizeof(path));
    json_object_put(root);
    Config cfg;
    int ret = config_load(&cfg, path);
    unlink(path);
    ASSERT_OK(ret, "absent schemaVersion accepted as legacy");
    ASSERT_EQ(cfg.meta_schema_version, 0, "absent schemaVersion parses as 0");
    ret = config_validate(&cfg);
    ASSERT_OK(ret, "legacy schema passes validation");
    ASSERT_PASS("absent schema version treated as legacy");
}

/* Test: the current schema version is accepted. */
TEST(test_schema_version_current_accepted) {
    struct json_object *root = load_json_for_mutation();
    if (!root) { printf("FAIL (cannot load fixture)\n"); tests_failed++; return; }
    struct json_object *meta;
    json_object_object_get_ex(root, "meta", &meta);
    json_object_object_add(meta, "schemaVersion",
                           json_object_new_int(CONFIG_SCHEMA_VERSION));
    char path[160];
    write_mutated_json(root, "schema-current", path, sizeof(path));
    json_object_put(root);
    Config cfg;
    int ret = config_load(&cfg, path);
    unlink(path);
    ASSERT_OK(ret, "current schemaVersion accepted");
    ASSERT_EQ(cfg.meta_schema_version, CONFIG_SCHEMA_VERSION,
              "current schemaVersion parsed");
    ret = config_validate(&cfg);
    ASSERT_OK(ret, "current schema passes validation");
    ASSERT_PASS("current schema version accepted");
}

/* Test: a schema version newer than the daemon understands is rejected. */
TEST(test_schema_version_too_new_rejected) {
    struct json_object *root = load_json_for_mutation();
    if (!root) { printf("FAIL (cannot load fixture)\n"); tests_failed++; return; }
    struct json_object *meta;
    json_object_object_get_ex(root, "meta", &meta);
    json_object_object_add(meta, "schemaVersion",
                           json_object_new_int(CONFIG_SCHEMA_VERSION + 1));
    char path[160];
    write_mutated_json(root, "schema-too-new", path, sizeof(path));
    json_object_put(root);
    Config cfg;
    int ret = config_load(&cfg, path);
    unlink(path);
    ASSERT_FAIL(ret, "schema newer than daemon must be rejected");
    ASSERT_PASS("too-new schema version rejected");
}

/* Test: a non-integer schema version is rejected at parse time. */
TEST(test_schema_version_noninteger_rejected) {
    struct json_object *root = load_json_for_mutation();
    if (!root) { printf("FAIL (cannot load fixture)\n"); tests_failed++; return; }
    struct json_object *meta;
    json_object_object_get_ex(root, "meta", &meta);
    json_object_object_add(meta, "schemaVersion",
                           json_object_new_string("one"));
    char path[160];
    write_mutated_json(root, "schema-noninteger", path, sizeof(path));
    json_object_put(root);
    Config cfg;
    int ret = config_load(&cfg, path);
    unlink(path);
    ASSERT_FAIL(ret, "non-integer schemaVersion must be rejected");
    ASSERT_PASS("non-integer schema version rejected");
}

int main(void) {
    printf("=== config_parser tests ===\n");
    log_init(UPERF_LOG_WARN, 0, NULL);

    RUN_TEST(test_load_valid_config);
    RUN_TEST(test_load_missing_file);
    RUN_TEST(test_load_invalid_json);
    RUN_TEST(test_validate_valid);
    RUN_TEST(test_validate_no_clusters);
    RUN_TEST(test_validate_neg_efficiency);
    RUN_TEST(test_validate_zero_freq);
    RUN_TEST(test_validate_no_presets);
    RUN_TEST(test_validate_zero_cores);
    RUN_TEST(test_load_power_model);
    RUN_TEST(test_load_sysfs_knobs);
    RUN_TEST(test_removed_and_unknown_keys_warn);
    RUN_TEST(test_official_config_has_no_sysfs_enable);
    RUN_TEST(test_load_sched_rules);
    RUN_TEST(test_reject_invalid_sched_priority);
    RUN_TEST(test_reject_unknown_affinity_cpumask);
    RUN_TEST(test_reject_invalid_sched_regex);
    RUN_TEST(test_reject_invalid_cgroup_class);
    RUN_TEST(test_load_switcher);
    RUN_TEST(test_load_input);
    RUN_TEST(test_load_initials);
    RUN_TEST(test_load_presets);
    RUN_TEST(test_validate_cgroup_requires_sched);
    RUN_TEST(test_schema_version_absent_is_legacy);
    RUN_TEST(test_schema_version_current_accepted);
    RUN_TEST(test_schema_version_too_new_rejected);
    RUN_TEST(test_schema_version_noninteger_rejected);
    RUN_TEST(test_heavyload_missing_uses_defaults);
    RUN_TEST(test_heavyload_empty_block_ok);
    RUN_TEST(test_heavyload_idle_ge_heavy_rejected);
    RUN_TEST(test_heavyload_negative_idle_rejected);
    RUN_TEST(test_heavyload_heavy_over_100_rejected);
    RUN_TEST(test_heavyload_sample_time_too_small_rejected);
    RUN_TEST(test_heavyload_sample_time_too_large_rejected);
    RUN_TEST(test_heavyload_burst_slack_too_large_rejected);
    RUN_TEST(test_heavyload_wrong_type_ignored);
    RUN_TEST(test_heavyload_disabled_skips_validation);

    printf("\nResults: %d/%d passed (%d failed)\n",
           tests_passed, tests_run, tests_failed);
    log_shutdown();
    return tests_failed > 0 ? 1 : 0;
}
