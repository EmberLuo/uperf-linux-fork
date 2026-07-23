#ifndef UPERF_POWER_MODEL_H
#define UPERF_POWER_MODEL_H

#include <stdint.h>

/* Power model entry for one CPU cluster.
 * Values are calibrated empirically per SoC.
 * Efficiency is normalized to Cortex-A53@1GHz = 100.
 *
 * NOTE: this describes a utilization-to-frequency mapping, not a power
 * budget.  Wattage is not modelled.  Only efficiency, the frequency
 * endpoints, and sweet_freq_mhz (efficiency cap) are consumed. */
typedef struct {
    int    efficiency;       /* Relative single-core performance score */
    int    nr_cores;         /* Number of cores in this cluster */
    uint64_t cpu_mask;       /* Explicit CPUs belonging to this model */
    char   cpumask_name[32]; /* Optional modules.sched.cpumask reference */
    float  typical_freq_mhz; /* Normal operating (max) frequency (MHz) */
    float  sweet_freq_mhz;   /* Efficiency-cap frequency (MHz) */
    float  free_freq_mhz;    /* Minimum efficient frequency (MHz) */
} PowerModelEntry;

/* Compute performance at a given frequency.
 * Returns relative performance score (normalized to efficiency at typical_freq). */
float power_model_perf_at_freq(const PowerModelEntry *pm, float freq_mhz);

/* Select optimal frequency for a given performance demand.
 * demand: required relative performance (0.0-1.0, where 1.0 = max cluster perf).
 * margin: additional headroom factor (0.0 = none, 1.0 = double).
 * Returns optimal frequency in MHz. */
float power_model_select_freq(const PowerModelEntry *pm,
                              float demand, float margin);

#endif /* UPERF_POWER_MODEL_H */
