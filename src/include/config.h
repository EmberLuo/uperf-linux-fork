#ifndef UPERF_CONFIG_H
#define UPERF_CONFIG_H

#include <stdint.h>
#include <stdbool.h>
#include "power_model.h"

/* Config schema version the daemon understands.  A config may declare
 * meta.schemaVersion; the daemon accepts an absent version (0, legacy) or any
 * value <= this, and rejects a newer schema it cannot interpret.  Bump this
 * whenever an incompatible schema change lands. */
#define CONFIG_SCHEMA_VERSION 1

#define MAX_CLUSTERS     4
#define MAX_CPUS         64
#define MAX_CPU_MASK_GROUPS 8
#define MAX_AFFINITY_PROFILES 16
#define MAX_PRIORITY_PROFILES 16
#define MAX_THREAD_RULES 16
#define MAX_CGROUP_CLASSES 8
#define MAX_KNOBS        64
#define MAX_RULES        32
#define MAX_POWER_MODES  8
#define MAX_GAMES        128
#define MAX_PATH_LEN     256
#define MAX_NAME_LEN     64

/* A sysfs path consumed by the runtime. */
typedef struct {
    char        name[MAX_NAME_LEN];
    char        path[MAX_PATH_LEN];
} KnobDef;

/* Power mode preset name */
typedef enum {
    MODE_BALANCE = 0,
    MODE_POWERSAVE,
    MODE_PERFORMANCE,
    MODE_FAST,
    MODE_NUM
} PowerMode;

/* Presence bits for scalar tuning values in a preset.  A separate mask is
 * required because zero and false are valid explicit overrides.
 *
 * Only the tuning fields actually consumed by the runtime are represented:
 * margin/burst feed the frequency demand calculation (frequency_controller.c),
 * limit_efficiency caps a cluster at its sweet frequency, and base_sample_time
 * drives the main-loop poll interval (main.c).  Upstream Uperf additionally
 * defined latencyTime/slowLimitPower/fastLimitPower/fastLimitCapacity/
 * fastLimitRecoverScale/guideCap/baseSlackTime/predictThd, but none of those
 * were ever read by any decision path here, so they have been removed rather
 * than carried forward as dead configuration. */
typedef enum {
    ACTION_TUNE_MARGIN                   = 1u << 0,
    ACTION_TUNE_BURST                    = 1u << 1,
    ACTION_TUNE_LIMIT_EFFICIENCY         = 1u << 2,
    ACTION_TUNE_BASE_SAMPLE_TIME         = 1u << 3,
} ActionTuningField;

/* CPU frequency limits and tuning params for an action.
 *
 * cpu_freq_min/max are consumed by frequency_controller_compute_limits() as
 * explicit per-cluster clamps.  They are not yet wired to any JSON key (no
 * parse site sets has_cpu_freq_*), so today they only ever take effect in
 * unit tests; the plumbing is retained because it is live decision code. */
typedef struct {
    uint32_t     tuning_present;
    bool         has_cpu_freq_max;
    int          cpu_freq_max[MAX_CLUSTERS];  /* Hz */
    bool         has_cpu_freq_min;
    int          cpu_freq_min[MAX_CLUSTERS];
    /* Global tuning params consumed by the runtime */
    float        margin;
    float        burst;
    bool         limit_efficiency;
    float        base_sample_time;
} ActionParams;

/* State-specific action overrides */
typedef struct {
    ActionParams global;      /* Applied to all states */
    ActionParams idle;
    ActionParams touch;
    ActionParams trigger;
    ActionParams gesture;
    ActionParams junk;
    ActionParams switch_;
    ActionParams boost;
} StatePresets;

/* A full power-mode preset (balance/powersave/performance) */
typedef struct {
    char                        name[MAX_NAME_LEN];
    StatePresets                presets;
} PowerModePreset;

/* Switcher / hint configuration.
 *
 * The upstream switchInode file (an external process writes a power-mode name
 * that Uperf polls) has no runtime reader here — power mode is driven by D-Bus
 * and per-app rules instead — so the field is not carried. */
typedef struct {
    char perapp_file[MAX_PATH_LEN];
    float hint_duration[8];  /* idle, touch, trigger, gesture, junk, switch, boost, _ */
} SwitcherConfig;

/* Input module configuration */
typedef struct {
    bool    enable;
    float   swipe_thd;
    float   gesture_thd_x;
    float   gesture_thd_y;
    float   gesture_delay_time;
    int     screen_width;   /* Physical screen width in pixels (0 = auto-detect) */
    int     screen_height;  /* Physical screen height in pixels (0 = auto-detect) */
} InputConfig;

/* Scheduling context used by the original Uperf affinity/priority profiles. */
typedef enum {
    SCHED_CTX_BG = 0,
    SCHED_CTX_FG,
    SCHED_CTX_IDLE,
    SCHED_CTX_TOUCH,
    SCHED_CTX_BOOST,
    SCHED_CTX_NUM
} SchedContext;

/* CPU mask group */
typedef struct {
    char    name[MAX_NAME_LEN];
    int     cpus[MAX_CPUS];
    int     nr_cpus;
    uint64_t mask;
} CpuMaskGroup;

/* A named affinity profile maps each scheduling context to a CPU mask.
 * has_mask=false means that the scheduler must leave/restore affinity. */
typedef struct {
    char name[MAX_NAME_LEN];
    uint64_t masks[SCHED_CTX_NUM];
    bool has_mask[SCHED_CTX_NUM];
} AffinityProfile;

/* Priority encoding is compatible with Uperf:
 *   0       leave unchanged
 *  -1/-2/-3 SCHED_OTHER/BATCH/IDLE
 *   1..98   SCHED_FIFO priority
 *   100..139 SCHED_OTHER with nice=(value-120) */
typedef struct {
    char name[MAX_NAME_LEN];
    int values[SCHED_CTX_NUM];
} PriorityProfile;

/* A process rule contains ordered, first-match thread rules. */
typedef struct {
    char regex[MAX_PATH_LEN];
    char affinity_name[MAX_NAME_LEN];
    char priority_name[MAX_NAME_LEN];
    int affinity_index;
    int priority_index;
} SchedThreadRule;

typedef struct {
    char name[MAX_NAME_LEN];
    char regex[MAX_PATH_LEN];
    bool pinned;
    bool match_game;
    char cgroup_class[MAX_NAME_LEN];
    SchedThreadRule thread_rules[MAX_THREAD_RULES];
    int nr_thread_rules;
} SchedRule;

/* Sched module configuration */
typedef struct {
    bool enable;
    CpuMaskGroup cpumasks[MAX_CPU_MASK_GROUPS];
    int nr_cpumasks;
    AffinityProfile affinity_profiles[MAX_AFFINITY_PROFILES];
    int nr_affinity_profiles;
    PriorityProfile priority_profiles[MAX_PRIORITY_PROFILES];
    int nr_priority_profiles;
    SchedRule rules[MAX_RULES];
    int nr_rules;
} SchedConfig;

/* Process-group resource class. cpu_mask=0 leaves AllowedCPUs unchanged. */
typedef struct {
    char name[MAX_NAME_LEN];
    char cpumask_name[MAX_NAME_LEN];
    uint64_t cpu_mask;
    int cpu_weight;
    int uclamp_min;
    int uclamp_max;
} CgroupClass;

typedef struct {
    bool enable;
    char backend[MAX_NAME_LEN];
    CgroupClass classes[MAX_CGROUP_CLASSES];
    int nr_classes;
} CgroupConfig;

/* CPU module configuration */
typedef struct {
    bool enable;
    PowerModelEntry power_model[MAX_CLUSTERS];
    int nr_clusters;
} CpuConfig;

/* Sysfs paths used for GPU frequency target discovery. CPU cpufreq policies
 * are discovered directly from /sys/devices/system/cpu/cpufreq. */
typedef struct {
    KnobDef knobs[MAX_KNOBS];
    int nr_knobs;
} SysfsConfig;

/* Thermal thresholds configured under modules.thermal. */
typedef struct {
    bool enable;
    int warn_temp;
    int throttle_temp;
    int critical_temp;
    int recovery_temp;
} ThermalConfig;

/* Heavy-load / boost detector parameters, configured under modules.heavyload.
 * sample_time_ms is the MINIMUM sampling interval: the daemon's main loop runs
 * at 10-250ms cadence, so this is a floor, not a guaranteed period. */
typedef struct {
    bool  enable;
    float sample_time_ms;   /* [1, 1000]   minimum /proc/stat sample interval */
    float heavy_load_pct;   /* (idle, 100] load% that triggers boost */
    float idle_load_pct;    /* [0, heavy)  load% that ends boost */
    float burst_slack_ms;   /* [0, 60000]  cooldown before re-entering boost */
} HeavyLoadConfig;

/* Full config — the union of all module configs + presets */
typedef struct {
    char meta_name[MAX_NAME_LEN];
    char meta_author[MAX_NAME_LEN];
    int  meta_schema_version;   /* meta.schemaVersion; 0 if absent (legacy) */

    SwitcherConfig  switcher;
    InputConfig     input;
    CpuConfig       cpu;
    SysfsConfig     sysfs;
    SchedConfig     sched;
    CgroupConfig    cgroup;
    ThermalConfig   thermal;
    HeavyLoadConfig heavyload;

    PowerModePreset presets[MODE_NUM];

    /* Initial defaults (applied at startup before any state) */
    float initial_margin;
    float initial_burst;
    bool  initial_limit_efficiency;
    float initial_base_sample_time;
} Config;

/* Load config from JSON file. Returns 0 on success, -1 on error. */
int config_load(Config *cfg, const char *path);

/* Validate config after loading. Returns 0 if valid, -1 otherwise. */
int config_validate(const Config *cfg);

/* Check that all referenced sysfs paths exist and are writable.
 * Returns 0 if all paths OK, -1 otherwise (logs warnings for missing ones). */
int config_check_paths(const Config *cfg);

/* Free any resources held by config (none currently, but for future-proofing). */
void config_free(Config *cfg);

#endif /* UPERF_CONFIG_H */
