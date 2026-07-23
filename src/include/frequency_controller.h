#ifndef UPERF_FREQUENCY_CONTROLLER_H
#define UPERF_FREQUENCY_CONTROLLER_H

#include <stddef.h>
#include <stdint.h>

#include "config.h"

/* Maximum number of operating performance points per cpufreq policy.  The
 * SM8550 clusters expose at most 20 OPPs (prime/performance); 32 leaves
 * headroom for other SoCs without dynamic allocation. */
#define MAX_OPP_POINTS 32

/* The discrete set of frequencies a cpufreq policy can actually run at, read
 * from scaling_available_frequencies.  Frequencies are in Hz.  Order is not
 * assumed; snapping scans the whole set. */
typedef struct {
    int64_t freq_hz[MAX_OPP_POINTS];
    int     count;
} OppTable;

/* Bit flags returned by frequency_controller_snap_limits(). */
typedef enum {
    OPP_SNAP_NONE            = 0,
    OPP_SNAP_APPLIED         = 1u << 0,
    /* No reachable OPP at/below the ceiling also satisfied the demand floor,
     * so the floor was relaxed to the highest OPP at/below the ceiling. */
    OPP_SNAP_FLOOR_RELAXED   = 1u << 1,
    /* The requested ceiling was below the hardware's lowest OPP, so no
     * physically valid value could satisfy it and the lowest OPP was used. */
    OPP_SNAP_CEILING_RAISED  = 1u << 2,
} OppSnapStatus;

/* Return the highest utilization among CPUs belonging to one cpufreq policy. */
float frequency_controller_cluster_demand(const float *cpu_loads,
                                          size_t nr_cpus,
                                          uint64_t cpu_mask);

/* Snap a computed [min,max] policy window to real OPP points so the daemon
 * always commands frequencies the hardware can actually run:
 *   - maximum snaps DOWN to the highest OPP <= *maximum_hz (never exceed the
 *     intended ceiling); if every OPP is above it, the lowest OPP is used.
 *   - minimum snaps UP to the lowest OPP >= *minimum_hz (always satisfy the
 *     demand floor); if every OPP is below it, the highest OPP is used.
 *   - if a hardware-reachable ceiling leaves no OPP in the requested window,
 *     thermal-ceiling safety wins: both endpoints become the highest OPP
 *     at/below the ceiling and OPP_SNAP_FLOOR_RELAXED is returned. A ceiling
 *     below the lowest hardware OPP instead reports OPP_SNAP_CEILING_RAISED.
 * A NULL or empty table leaves the values unchanged (graceful fallback to
 * kernel-side rounding).  Pure function: no I/O, deterministic. */
OppSnapStatus frequency_controller_snap_limits(const OppTable *opp,
                                               int64_t *minimum_hz,
                                               int64_t *maximum_hz);

/* Convert the current preset, utilization and thermal state into safe policy
 * limits.  All frequency arguments and results use Hz. */
void frequency_controller_compute_limits(const PowerModelEntry *model,
                                         const ActionParams *params,
                                         int cluster,
                                         float demand_percent,
                                         float thermal_reduction,
                                         int64_t hardware_min_hz,
                                         int64_t hardware_max_hz,
                                         int64_t *minimum_hz,
                                         int64_t *maximum_hz);

#endif /* UPERF_FREQUENCY_CONTROLLER_H */
