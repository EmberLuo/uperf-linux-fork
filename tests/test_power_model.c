#include "../src/include/power_model.h"
#include "../src/include/log.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

static int tests_run = 0;
static int tests_passed = 0;
static int tests_failed = 0;

#define TEST(name) static void name(void)
#define RUN_TEST(name) do { \
    tests_run++; \
    printf("  TEST: %s ... ", #name); \
    name(); \
} while(0)
#define ASSERT_NEAR(a, b, eps, msg) do { \
    double diff = fabs((double)(a) - (double)(b)); \
    if (diff > (eps)) { \
        printf("FAIL (%s: expected ~%.2f, got %.2f, diff %.4f)\n", \
               msg, (double)(b), (double)(a), diff); \
        tests_failed++; return; \
    } \
} while(0)
#define ASSERT_GT(a, b, msg) do { \
    if ((a) <= (b)) { \
        printf("FAIL (%s: expected %.2f > %.2f)\n", msg, (double)(a), (double)(b)); \
        tests_failed++; return; \
    } \
} while(0)
#define ASSERT_LT(a, b, msg) do { \
    if ((a) >= (b)) { \
        printf("FAIL (%s: expected %.2f < %.2f)\n", msg, (double)(a), (double)(b)); \
        tests_failed++; return; \
    } \
} while(0)
#define ASSERT_PASS(msg) do { \
    printf("PASS\n"); tests_passed++; \
} while(0)

/* Create a test power model entry.
 * The power model is a utilization-to-frequency mapping; wattage is not
 * modelled, so only efficiency and the frequency endpoints are set. */
static PowerModelEntry make_pm(int eff, int nr, float typ_f,
                                float sweet_f, float free_f) {
    PowerModelEntry pm = {0};
    pm.efficiency = eff;
    pm.nr_cores = nr;
    pm.typical_freq_mhz = typ_f;
    pm.sweet_freq_mhz = sweet_f;
    pm.free_freq_mhz = free_f;
    return pm;
}

/* === Performance tests === */

TEST(test_perf_at_zero_freq) {
    PowerModelEntry pm = make_pm(280, 2, 2200, 1600, 500);
    float perf = power_model_perf_at_freq(&pm, 0.0f);
    ASSERT_NEAR(perf, 0.0f, 0.01, "zero perf at zero freq");
    ASSERT_PASS("zero frequency yields zero performance");
}

TEST(test_perf_linear_with_freq) {
    PowerModelEntry pm = make_pm(280, 2, 2200, 1600, 500);
    float perf_half = power_model_perf_at_freq(&pm, 1100.0f);
    float perf_full = power_model_perf_at_freq(&pm, 2200.0f);
    ASSERT_GT(perf_full, perf_half, "full freq > half freq perf");
    ASSERT_NEAR(perf_full / perf_half, 2.0f, 0.3f, "perf ratio ≈ 2:1");
    ASSERT_PASS("performance scales roughly linearly with frequency");
}

TEST(test_perf_at_typical_is_efficiency) {
    PowerModelEntry pm = make_pm(280, 2, 2200, 1600, 500);
    float perf = power_model_perf_at_freq(&pm, pm.typical_freq_mhz);
    ASSERT_NEAR(perf, (float)pm.efficiency, 10.0f, "perf at typical ≈ efficiency");
    ASSERT_PASS("performance at typical frequency equals efficiency score");
}

TEST(test_different_clusters_different_perf) {
    PowerModelEntry big = make_pm(700, 1, 3000, 2400, 800);
    PowerModelEntry little = make_pm(180, 5, 1200, 800, 300);

    float perf_big = power_model_perf_at_freq(&big, 2000.0f);
    float perf_lit = power_model_perf_at_freq(&little, 1000.0f);
    ASSERT_GT(perf_big, perf_lit, "big cluster > little cluster");
    ASSERT_PASS("different clusters have different performance profiles");
}

/* === Frequency selection tests === */

TEST(test_select_freq_meets_demand) {
    PowerModelEntry pm = make_pm(280, 2, 2200, 1600, 500);
    float freq = power_model_select_freq(&pm, 0.5f, 0.0f);
    ASSERT_GT(freq, 0.0f, "non-zero freq for positive demand");
    ASSERT_LT(freq, pm.typical_freq_mhz * 2.0f, "freq within bounds");
    ASSERT_PASS("select_freq returns reasonable frequency for 50% demand");
}

TEST(test_select_freq_with_margin) {
    PowerModelEntry pm = make_pm(280, 2, 2200, 1600, 500);
    float freq_low = power_model_select_freq(&pm, 0.3f, 0.0f);
    float freq_high = power_model_select_freq(&pm, 0.3f, 0.5f);
    ASSERT_GT(freq_high, freq_low, "margin increases frequency");
    ASSERT_PASS("margin parameter increases selected frequency");
}

TEST(test_select_freq_high_demand) {
    PowerModelEntry pm = make_pm(280, 2, 2200, 1600, 500);
    float freq = power_model_select_freq(&pm, 0.9f, 0.0f);
    ASSERT_GT(freq, 1000.0f, "high demand → high frequency");
    ASSERT_PASS("high demand selects high frequency");
}

TEST(test_select_freq_zero_demand) {
    PowerModelEntry pm = make_pm(280, 2, 2200, 1600, 500);
    float freq = power_model_select_freq(&pm, 0.0f, 0.0f);
    ASSERT_NEAR(freq, pm.free_freq_mhz, 100.0f, "zero demand → near free freq");
    ASSERT_PASS("zero demand selects minimum frequency");
}

int main(void) {
    printf("=== power_model tests ===\n");
    log_init(UPERF_LOG_WARN, 0, NULL);

    RUN_TEST(test_perf_at_zero_freq);
    RUN_TEST(test_perf_linear_with_freq);
    RUN_TEST(test_perf_at_typical_is_efficiency);
    RUN_TEST(test_different_clusters_different_perf);
    RUN_TEST(test_select_freq_meets_demand);
    RUN_TEST(test_select_freq_with_margin);
    RUN_TEST(test_select_freq_high_demand);
    RUN_TEST(test_select_freq_zero_demand);

    printf("\nResults: %d/%d passed (%d failed)\n",
           tests_passed, tests_run, tests_failed);
    log_shutdown();
    return tests_failed > 0 ? 1 : 0;
}
