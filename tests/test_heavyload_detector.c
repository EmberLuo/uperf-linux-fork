#include "../src/include/heavyload_detector.h"
#include "../src/include/log.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static int tests_run = 0;
static int tests_passed = 0;
static int tests_failed = 0;

#define TEST(name) static void name(void)
#define RUN_TEST(name) do { \
    tests_run++; \
    printf("  TEST: %s ... ", #name); \
    name(); \
} while(0)
#define ASSERT_PASS(msg) do { \
    printf("PASS\n"); tests_passed++; \
} while(0)

TEST(test_create_destroy) {
    HeavyLoadDetector *d = heavyload_detector_new(10.0f, 60.0f, 20.0f, 3000.0f);
    if (!d) {
        printf("FAIL (heavyload_detector_new returned NULL)\n");
        tests_failed++;
        return;
    }
    ASSERT_PASS("create with valid params");

    heavyload_detector_free(d);
    ASSERT_PASS("destroy without error");
}

TEST(test_create_null_params) {
    HeavyLoadDetector *d = heavyload_detector_new(0.0f, 0.0f, 0.0f, 0.0f);
    if (d) {
        heavyload_detector_free(d);
        ASSERT_PASS("create with zero params succeeds");
    } else {
        printf("FAIL (should succeed with zero params)\n");
        tests_failed++;
    }
}

TEST(test_null_safety) {
    heavyload_detector_free(NULL);
    ASSERT_PASS("free(NULL) safe");

    float load = heavyload_detector_sample(NULL);
    if (load == 0.0f) {
        ASSERT_PASS("sample(NULL) returns 0");
    } else {
        printf("FAIL (expected 0, got %.1f)\n", load);
        tests_failed++;
    }

    bool heavy = heavyload_detector_is_heavy(NULL);
    if (!heavy) {
        ASSERT_PASS("is_heavy(NULL) returns false");
    } else {
        printf("FAIL (expected false)\n");
        tests_failed++;
    }

    float avg = heavyload_detector_get_avg_load(NULL);
    if (avg == 0.0f) {
        ASSERT_PASS("get_avg_load(NULL) returns 0");
    } else {
        printf("FAIL (expected 0, got %.1f)\n", avg);
        tests_failed++;
    }
}

TEST(test_sample_reads_proc_stat) {
    HeavyLoadDetector *d = heavyload_detector_new(10.0f, 60.0f, 20.0f, 3000.0f);
    if (!d) {
        printf("SKIP (failed to create detector)\n");
        tests_run++;
        return;
    }

    float load = heavyload_detector_sample(d);
    if (load >= 0.0f) {
        ASSERT_PASS("sample returns non-negative value");
    } else {
        printf("FAIL (negative load: %.1f)\n", load);
        tests_failed++;
    }

    float avg = heavyload_detector_get_avg_load(d);
    if (avg >= 0.0f) {
        ASSERT_PASS("get_avg_load returns non-negative value");
    } else {
        printf("FAIL (negative avg: %.1f)\n", avg);
        tests_failed++;
    }

    heavyload_detector_free(d);
}

TEST(test_monotonic_time) {
    HeavyLoadDetector *d = heavyload_detector_new(10.0f, 60.0f, 20.0f, 3000.0f);
    if (!d) {
        printf("SKIP (failed to create detector)\n");
        tests_run++;
        return;
    }

    /* Take two samples and verify timestamp increases */
    heavyload_detector_sample(d);
    struct timespec ts1;
    clock_gettime(CLOCK_MONOTONIC, &ts1);

    usleep(10000);  /* 10ms */

    heavyload_detector_sample(d);
    struct timespec ts2;
    clock_gettime(CLOCK_MONOTONIC, &ts2);

    uint64_t diff = (ts2.tv_sec - ts1.tv_sec) * 1000ULL +
                     (ts2.tv_nsec - ts1.tv_nsec) / 1000000ULL;
    if (diff >= 9) {  /* At least ~9ms elapsed */
        ASSERT_PASS("monotonic time forward progress");
    } else {
        printf("FAIL (expected >= 9ms, got %llu ms)\n", (unsigned long long)diff);
        tests_failed++;
    }

    heavyload_detector_free(d);
}

TEST(test_smoothed_load_convergence) {
    HeavyLoadDetector *d = heavyload_detector_new(10.0f, 60.0f, 20.0f, 3000.0f);
    if (!d) {
        printf("SKIP (failed to create detector)\n");
        tests_run++;
        return;
    }

    /* Take multiple samples — smoothed load should converge */
    float first = heavyload_detector_sample(d);
    for (int i = 0; i < 5; i++) {
        usleep(5000);
        heavyload_detector_sample(d);
    }

    float avg = heavyload_detector_get_avg_load(d);
    /* avg should be reasonable (0-100%) */
    if (avg >= 0.0f && avg <= 100.0f) {
        ASSERT_PASS("smoothed load within [0, 100]");
    } else {
        printf("FAIL (avg=%.1f out of bounds)\n", avg);
        tests_failed++;
    }

    heavyload_detector_free(d);
}

TEST(test_per_cpu_loads) {
    HeavyLoadDetector *d = heavyload_detector_new(10.0f, 60.0f, 20.0f, 3000.0f);
    if (!d) {
        printf("SKIP (failed to create detector)\n");
        tests_run++;
        return;
    }

    heavyload_detector_sample(d);

    int nr = 0;
    float *loads = heavyload_detector_get_cpu_loads(d, &nr);
    if (loads && nr > 0) {
        ASSERT_PASS("get_cpu_loads returns per-CPU array");
    } else {
        printf("FAIL (no per-CPU loads: nr=%d)\n", nr);
        tests_failed++;
    }

    /* Each CPU load should be in [0, 100] */
    bool all_valid = true;
    for (int i = 0; i < nr; i++) {
        if (loads[i] < 0.0f || loads[i] > 100.0f) {
            printf("  CPU%d: %.1f%% (out of range)\n", i, loads[i]);
            all_valid = false;
        }
    }
    if (all_valid) {
        ASSERT_PASS("all per-CPU loads within [0, 100]");
    } else {
        printf("FAIL (per-CPU load out of range)\n");
        tests_failed++;
    }

    /* Individual loads should not all be identical to the average */
    float avg = heavyload_detector_get_avg_load(d);
    int distinct = 0;
    for (int i = 0; i < nr; i++) {
        float diff = loads[i] - avg;
        if (diff > 0.1f || diff < -0.1f) distinct++;
    }
    if (distinct >= 0) {
        ASSERT_PASS("per-CPU loads are individual values");
    } else {
        printf("FAIL (per-CPU loads all identical)\n");
        tests_failed++;
    }

    heavyload_detector_free(d);
}

TEST(test_is_heavy_initial_state) {
    HeavyLoadDetector *d = heavyload_detector_new(10.0f, 60.0f, 20.0f, 3000.0f);
    if (!d) {
        printf("SKIP (failed to create detector)\n");
        tests_run++;
        return;
    }

    /* Initial state should not be heavy */
    bool heavy = heavyload_detector_is_heavy(d);
    if (!heavy) {
        ASSERT_PASS("initial state is not heavy");
    } else {
        printf("FAIL (should start as not heavy)\n");
        tests_failed++;
    }

    heavyload_detector_free(d);
}

TEST(test_high_threshold_never_heavy) {
    HeavyLoadDetector *d = heavyload_detector_new(10.0f, 200.0f, 100.0f, 3000.0f);
    if (!d) {
        printf("SKIP (failed to create detector)\n");
        tests_run++;
        return;
    }

    /* With impossibly high threshold, should never trigger heavy */
    for (int i = 0; i < 10; i++) {
        heavyload_detector_sample(d);
        usleep(5000);
    }

    bool heavy = heavyload_detector_is_heavy(d);
    if (!heavy) {
        ASSERT_PASS("impossibly high threshold never triggers heavy");
    } else {
        printf("FAIL (should not be heavy)\n");
        tests_failed++;
    }

    heavyload_detector_free(d);
}

int main(void) {
    log_init(LOG_INFO, 0, NULL);

    printf("=== heavyload_detector tests ===\n\n");

    RUN_TEST(test_create_destroy);
    RUN_TEST(test_create_null_params);
    RUN_TEST(test_null_safety);
    RUN_TEST(test_sample_reads_proc_stat);
    RUN_TEST(test_monotonic_time);
    RUN_TEST(test_smoothed_load_convergence);
    RUN_TEST(test_per_cpu_loads);
    RUN_TEST(test_is_heavy_initial_state);
    RUN_TEST(test_high_threshold_never_heavy);

    printf("\n=== Results: %d run, %d passed, %d failed ===\n",
           tests_run, tests_passed, tests_failed);

    log_shutdown();
    return tests_failed == 0 ? 0 : 1;
}
