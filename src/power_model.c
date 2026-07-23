#include "power_model.h"
#include "log.h"

/* ----------------------------------------------------------------
 * Power model: a utilization-to-frequency mapping, NOT a real power
 * budget scheduler.
 *
 * This module maps a relative performance demand to a target
 * frequency using a linear performance-vs-frequency approximation.
 * It does not estimate wattage and does not enforce a power budget;
 * the historical "power estimation" and "total power" helpers were
 * never wired into the frequency algorithm and have been removed so
 * the model states honestly what it does.  See git history if the
 * old piecewise power curve is needed for reference.
 * ---------------------------------------------------------------- */

float power_model_perf_at_freq(const PowerModelEntry *pm, float freq_mhz) {
    if (freq_mhz <= 0) return 0.0f;
    /* Performance is proportional to frequency, scaled by cluster efficiency */
    float norm = freq_mhz / pm->typical_freq_mhz;
    return pm->efficiency * norm;
}

float power_model_select_freq(const PowerModelEntry *pm,
                               float demand, float margin) {
    /* demand: required relative performance (0.0-1.0)
     * margin: additional headroom (0.0 = none, 1.0 = double)
     *
     * Target performance = demand * (1 + margin) * max_cluster_perf
     * Find the lowest frequency that meets this target.
     */
    float target_perf = demand * (1.0f + margin) * pm->efficiency;

    /* Binary search over frequency range */
    float lo = pm->free_freq_mhz;
    float hi = pm->typical_freq_mhz;
    float mid;

    for (int iter = 0; iter < 50; iter++) {
        mid = (lo + hi) / 2.0f;
        float perf = power_model_perf_at_freq(pm, mid);

        if (perf >= target_perf) {
            hi = mid;  /* Can go lower */
        } else {
            lo = mid;  /* Need higher frequency */
        }

        if (hi - lo < 1.0f)  /* Precision: 1 MHz */
            break;
    }

    return hi;
}
