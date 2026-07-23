#include "../src/include/frequency_controller.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

static int failures;

#define CHECK(condition, message) do { \
    if (!(condition)) { \
        fprintf(stderr, "FAIL: %s\n", message); \
        failures++; \
    } \
} while (0)

static PowerModelEntry model(void) {
    PowerModelEntry value = {
        .efficiency = 300,
        .nr_cores = 3,
        .typical_freq_mhz = 3000.0f,
        .sweet_freq_mhz = 2200.0f,
        .free_freq_mhz = 600.0f,
    };
    return value;
}

static void test_cluster_demand(void) {
    float loads[] = {5.0f, 20.0f, 75.0f, 40.0f};
    CHECK(fabsf(frequency_controller_cluster_demand(loads, 4, 0xau) -
                40.0f) < 0.01f,
          "cluster demand uses only CPUs in the policy mask");
    CHECK(fabsf(frequency_controller_cluster_demand(loads, 4, 0x5u) -
                75.0f) < 0.01f,
          "cluster demand returns the busiest policy CPU");
}

static void test_limits(void) {
    PowerModelEntry pm = model();
    ActionParams params;
    memset(&params, 0, sizeof(params));
    params.margin = 0.2f;
    int64_t minimum, maximum;

    frequency_controller_compute_limits(&pm, &params, 0, 0.0f, 0.0f,
                                        300000000, 3000000000,
                                        &minimum, &maximum);
    CHECK(minimum >= 590000000 && minimum <= 610000000,
          "idle demand selects the model free frequency");
    CHECK(maximum == 3000000000, "normal mode preserves hardware maximum");

    params.limit_efficiency = true;
    frequency_controller_compute_limits(&pm, &params, 0, 100.0f, 0.0f,
                                        300000000, 3000000000,
                                        &minimum, &maximum);
    CHECK(maximum == 2200000000,
          "efficiency mode caps the policy at the sweet frequency");
    CHECK(minimum == maximum, "minimum never exceeds an efficiency cap");

    params.limit_efficiency = false;
    frequency_controller_compute_limits(&pm, &params, 0, 50.0f, 1.0f,
                                        300000000, 3000000000,
                                        &minimum, &maximum);
    CHECK(minimum == 300000000 && maximum == 300000000,
          "full thermal reduction clamps both limits to hardware minimum");

    memset(&params, 0, sizeof(params));
    params.burst = 0.5f;
    frequency_controller_compute_limits(&pm, &params, 0, 10.0f, 0.0f,
                                        300000000, 3000000000,
                                        &minimum, &maximum);
    CHECK(minimum >= 1790000000,
          "burst raises the requested floor above raw utilization");
}

/* Real SM8550 prime-cluster OPP table (kHz -> Hz), from
 * scaling_available_frequencies on the target device. */
static OppTable prime_opp(void) {
    const int64_t khz[] = {
        595200, 729600, 864000, 998400, 1132800, 1248000, 1363200, 1478400,
        1593600, 1708800, 1843200, 1977600, 2092800, 2227200, 2342400,
        2476800, 2592000, 2726400, 2841600, 2956800
    };
    OppTable opp = {0};
    opp.count = (int)(sizeof(khz) / sizeof(khz[0]));
    for (int i = 0; i < opp.count; i++) opp.freq_hz[i] = khz[i] * 1000;
    return opp;
}

static void test_snap(void) {
    OppTable opp = prime_opp();

    /* A window strictly between OPPs snaps outward-safe: max DOWN, min UP. */
    int64_t lo = 1000000000LL;  /* 1.000 GHz, between 998.4M and 1132.8M */
    int64_t hi = 2000000000LL;  /* 2.000 GHz, between 1977.6M and 2092.8M */
    OppSnapStatus status = frequency_controller_snap_limits(&opp, &lo, &hi);
    CHECK(status == OPP_SNAP_APPLIED, "normal OPP window needs no relaxation");
    CHECK(lo == 1132800000LL, "min snaps up to next OPP >= floor");
    CHECK(hi == 1977600000LL, "max snaps down to highest OPP <= ceiling");

    /* Exact OPP values are preserved. */
    lo = 595200000LL;
    hi = 2956800000LL;
    status = frequency_controller_snap_limits(&opp, &lo, &hi);
    CHECK(lo == 595200000LL && hi == 2956800000LL,
          "exact OPP endpoints are preserved");

    /* Ceiling below the lowest OPP falls back to the lowest OPP. */
    lo = 100000000LL;
    hi = 100000000LL;
    status = frequency_controller_snap_limits(&opp, &lo, &hi);
    CHECK(hi == 595200000LL, "ceiling below range clamps to lowest OPP");
    CHECK(lo == 595200000LL, "floor never exceeds the snapped ceiling");
    CHECK((status & OPP_SNAP_CEILING_RAISED) != 0,
          "unattainable low ceiling is reported");

    /* Floor above the highest OPP falls back to the highest OPP. */
    lo = 9000000000LL;
    hi = 9000000000LL;
    status = frequency_controller_snap_limits(&opp, &lo, &hi);
    CHECK(lo == 2956800000LL, "floor above range clamps to highest OPP");
    CHECK(hi == 2956800000LL, "ceiling above range clamps to highest OPP");
    CHECK((status & OPP_SNAP_FLOOR_RELAXED) != 0,
          "unattainable high floor is reported");

    /* No OPP lies inside this narrow range.  The thermal ceiling wins over
     * the demand floor, and the relaxation is observable to the caller. */
    lo = 1010000000LL;
    hi = 1100000000LL;
    status = frequency_controller_snap_limits(&opp, &lo, &hi);
    CHECK(lo == 998400000LL && hi == 998400000LL,
          "empty OPP window chooses highest OPP below ceiling");
    CHECK((status & OPP_SNAP_FLOOR_RELAXED) != 0,
          "empty OPP window reports relaxed floor");

    /* Empty/NULL table leaves values untouched (graceful fallback). */
    OppTable empty = {0};
    lo = 1234567;
    hi = 7654321;
    status = frequency_controller_snap_limits(&empty, &lo, &hi);
    CHECK(lo == 1234567 && hi == 7654321, "empty table is a no-op");
    CHECK(status == OPP_SNAP_NONE, "empty table reports no snapping");
    status = frequency_controller_snap_limits(NULL, &lo, &hi);
    CHECK(lo == 1234567 && hi == 7654321, "NULL table is a no-op");
    CHECK(status == OPP_SNAP_NONE, "NULL table reports no snapping");
}

static void test_three_cluster_and_gpu_tables(void) {
    const int64_t points[][5] = {
        {307200000, 614400000, 1036800000, 1363200000, 1785600000},
        {499200000, 940800000, 1555200000, 2150400000, 2803200000},
        {595200000, 1132800000, 1843200000, 2476800000, 2956800000},
        {180000000, 305000000, 430000000, 545000000, 680000000},
    };
    for (int table = 0; table < 4; table++) {
        OppTable opp = {.count = 5};
        memcpy(opp.freq_hz, points[table], sizeof(points[table]));
        int64_t lo = points[table][1] + 1;
        int64_t hi = points[table][4] - 1;
        OppSnapStatus status = frequency_controller_snap_limits(
            &opp, &lo, &hi);
        CHECK(status == OPP_SNAP_APPLIED,
              "three CPU clusters and GPU use the same deterministic snapping");
        CHECK(lo == points[table][2] && hi == points[table][3],
              "each policy snaps using its own OPP table and units");
    }
}

int main(void) {
    test_cluster_demand();
    test_limits();
    test_snap();
    test_three_cluster_and_gpu_tables();
    if (failures == 0) puts("frequency_controller: all tests passed");
    return failures == 0 ? 0 : 1;
}
