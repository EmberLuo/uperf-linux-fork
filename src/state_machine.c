#define _GNU_SOURCE
#include "state_machine.h"
#include "log.h"
#include "runtime_backend.h"

#include <string.h>
#include <stdlib.h>
#include <math.h>

/* Helper: monotonic time in milliseconds */
static uint64_t now_ms(void) {
    return runtime_backend_monotonic_ms();
}

/* Internal state machine implementation */
struct StateMachine {
    SceneState    current_scene;
    PowerMode     current_mode;
    uint64_t      enter_time_ms;  /* monotonic ms when we entered current scene */

    /* Per-mode, per-scene action parameters (populated from presets at init) */
    ActionParams actions[MODE_NUM][SCENE_NUM_STATES];

    /* Config snapshot. Config currently contains no owning pointers. */
    Config cfg;

    /* Hint durations (seconds) */
    float hint_duration[SCENE_NUM_STATES];

    /* Burst slack / debounce */
    float request_burst_slack_ms;
    uint64_t last_boost_exit_ms;
    bool heavy_load_active;

    /* Load tracking for boost transitions */
    float current_load;
    float load_history[16];
    int   load_hist_idx;

    /* Thermal reduction (0.0 = none, 1.0 = max reduction) */
    float thermal_reduction;
};

static const ActionParams *get_state_preset(const StatePresets *presets,
                                             SceneState scene) {
    switch (scene) {
        case SCENE_IDLE: return &presets->idle;
        case SCENE_TOUCH: return &presets->touch;
        case SCENE_TRIGGER: return &presets->trigger;
        case SCENE_GESTURE: return &presets->gesture;
        case SCENE_JUNK: return &presets->junk;
        case SCENE_SWITCH: return &presets->switch_;
        case SCENE_BOOST: return &presets->boost;
        default: return NULL;
    }
}

static void merge_action_params(ActionParams *dst, const ActionParams *src) {
    uint32_t mask = src->tuning_present;
#define MERGE_TUNING(bit, member) \
    do { if (mask & (bit)) dst->member = src->member; } while (0)
    MERGE_TUNING(ACTION_TUNE_MARGIN, margin);
    MERGE_TUNING(ACTION_TUNE_BURST, burst);
    MERGE_TUNING(ACTION_TUNE_LIMIT_EFFICIENCY, limit_efficiency);
    MERGE_TUNING(ACTION_TUNE_BASE_SAMPLE_TIME, base_sample_time);
#undef MERGE_TUNING
    dst->tuning_present |= mask;

    /* Explicit per-cluster frequency clamps (consumed by
     * frequency_controller_compute_limits). */
    if (src->has_cpu_freq_max) {
        dst->has_cpu_freq_max = true;
        memcpy(dst->cpu_freq_max, src->cpu_freq_max,
               sizeof(dst->cpu_freq_max));
    }
    if (src->has_cpu_freq_min) {
        dst->has_cpu_freq_min = true;
        memcpy(dst->cpu_freq_min, src->cpu_freq_min,
               sizeof(dst->cpu_freq_min));
    }
}

static ActionParams initial_action(const Config *cfg) {
    ActionParams action = {0};
    action.margin = cfg->initial_margin;
    action.burst = cfg->initial_burst;
    action.limit_efficiency = cfg->initial_limit_efficiency;
    action.base_sample_time = cfg->initial_base_sample_time;
    return action;
}

/* Get the merged ActionParams for the current (mode, scene) pair. */
static const ActionParams *get_action_params(const StateMachine *sm,
                                              SceneState scene) {
    if (!sm || sm->current_mode < 0 || sm->current_mode >= MODE_NUM ||
        scene < 0 || scene >= SCENE_NUM_STATES)
        return NULL;
    return &sm->actions[sm->current_mode][scene];
}

StateMachine *state_machine_new(const Config *cfg) {
    StateMachine *sm = calloc(1, sizeof(*sm));
    if (!sm) return NULL;

    sm->cfg = *cfg;
    sm->current_scene = SCENE_IDLE;
    sm->current_mode = MODE_BALANCE;
    sm->enter_time_ms = now_ms();
    sm->heavy_load_active = false;
    sm->last_boost_exit_ms = 0;
    sm->request_burst_slack_ms = 3000.0f;
    sm->load_hist_idx = 0;
    memset(sm->load_history, 0, sizeof(sm->load_history));
    sm->thermal_reduction = 0.0f;

    /* Copy hint durations from config */
    for (int i = 0; i < SCENE_NUM_STATES; i++) {
        sm->hint_duration[i] = cfg->switcher.hint_duration[i];
    }

    /* Initialize actions from presets */
    for (int m = 0; m < MODE_NUM; m++) {
        for (int s = 0; s < SCENE_NUM_STATES; s++) {
            ActionParams action = initial_action(cfg);
            const StatePresets *presets = &cfg->presets[m].presets;
            merge_action_params(&action, &presets->global);
            const ActionParams *scene = get_state_preset(presets,
                                                         (SceneState)s);
            if (scene) merge_action_params(&action, scene);
            sm->actions[m][s] = action;
        }
    }

    log_info("StateMachine created: mode=%d scene=%d",
             sm->current_mode, sm->current_scene);
    return sm;
}

void state_machine_free(StateMachine *sm) {
    if (!sm) return;
    log_debug("StateMachine destroyed (was in scene %d)", sm->current_scene);
    free(sm);
}

void state_machine_tick(StateMachine *sm) {
    uint64_t now_ms_val = now_ms();
    uint64_t elapsed = now_ms_val - sm->enter_time_ms;

    /* Check hint duration timeouts for current scene */
    float hint = sm->hint_duration[sm->current_scene];
    if (hint > 0.0f && elapsed >= (uint64_t)(hint * 1000.0f)) {
        /* Transition based on current scene */
        SceneState next = sm->current_scene;

        switch (sm->current_scene) {
            case SCENE_TOUCH:
                next = SCENE_IDLE;
                break;
            case SCENE_TRIGGER:
            case SCENE_JUNK:
                next = SCENE_TOUCH;
                break;
            case SCENE_GESTURE:
                next = SCENE_TOUCH;
                break;
            case SCENE_SWITCH:
                next = SCENE_TOUCH;
                break;
            default:
                break;
        }

        if (next != sm->current_scene) {
            log_debug("Timeout transition: %d -> %d (elapsed %.0f ms, hint %.1f s)",
                      sm->current_scene, next, (double)elapsed, hint);
            sm->current_scene = next;
            sm->enter_time_ms = now_ms_val;
        }
    }
}

SceneState state_machine_feed_event(StateMachine *sm, EventType evt) {
    SceneState old = sm->current_scene;
    SceneState next = old;

    if (evt == EVT_HEAVY_LOAD_START) {
        sm->heavy_load_active = true;
        next = SCENE_BOOST;
        goto transition;
    }

    switch (old) {
        case SCENE_IDLE:
            switch (evt) {
                case EVT_TOUCH_DOWN:
                    next = SCENE_TOUCH;
                    break;
                case EVT_WINDOW_SWITCH:
                    next = SCENE_SWITCH;
                    break;
                default:
                    break;
            }
            break;

        case SCENE_TOUCH:
            switch (evt) {
                case EVT_TOUCH_UP:
                    next = SCENE_TRIGGER;
                    break;
                case EVT_SWIPE:
                case EVT_GESTURE:
                    next = SCENE_GESTURE;
                    break;
                case EVT_JUNK:
                    next = SCENE_JUNK;
                    break;
                case EVT_WINDOW_SWITCH:
                    next = SCENE_SWITCH;
                    break;
                case EVT_TIMEOUT:
                    next = SCENE_IDLE;
                    break;
                default:
                    break;
            }
            break;

        case SCENE_TRIGGER:
        case SCENE_JUNK:
            switch (evt) {
                case EVT_TIMEOUT:
                case EVT_TOUCH_UP:
                    next = SCENE_TOUCH;
                    break;
                case EVT_WINDOW_SWITCH:
                    next = SCENE_SWITCH;
                    break;
                default:
                    break;
            }
            break;

        case SCENE_GESTURE:
            switch (evt) {
                case EVT_TIMEOUT:
                    next = SCENE_TOUCH;
                    break;
                case EVT_WINDOW_SWITCH:
                    next = SCENE_SWITCH;
                    break;
                default:
                    break;
            }
            break;

        case SCENE_SWITCH:
            if (evt == EVT_TIMEOUT)
                next = SCENE_TOUCH;
            break;

        case SCENE_BOOST:
            switch (evt) {
                case EVT_HEAVY_LOAD_END:
                    sm->heavy_load_active = false;
                    sm->last_boost_exit_ms = now_ms();
                    next = SCENE_TOUCH;
                    break;
                case EVT_TIMEOUT:
                    next = SCENE_IDLE;
                    break;
                default:
                    break;
            }
            break;

        default:
            break;
    }

transition:
    if (next != old) {
        log_debug("State transition: %d -> %d (event=%d)",
                   old, next, evt);
        sm->current_scene = next;
        sm->enter_time_ms = now_ms();
    }

    return next;
}

SceneState state_machine_get_scene(const StateMachine *sm) {
    return sm->current_scene;
}

PowerMode state_machine_get_mode(const StateMachine *sm) {
    return sm->current_mode;
}

void state_machine_set_mode(StateMachine *sm, PowerMode mode) {
    if (!sm || mode < 0 || mode >= MODE_NUM) return;
    if (mode == sm->current_mode) return;

    log_info("Power mode changed: %d -> %d", sm->current_mode, mode);
    sm->current_mode = mode;

    /* Reset to idle in the new mode */
    sm->current_scene = SCENE_IDLE;
    sm->enter_time_ms = now_ms();
    sm->heavy_load_active = false;
    sm->last_boost_exit_ms = 0;
}

void state_machine_get_actions(const StateMachine *sm, ActionParams *out) {
    SceneState scene = sm->current_scene;

    const ActionParams *src = get_action_params(sm, scene);
    if (src) {
        memcpy(out, src, sizeof(*out));
    } else {
        memset(out, 0, sizeof(*out));
    }

}

float state_machine_get_hint_duration(const StateMachine *sm, SceneState scene) {
    if (scene < 0 || scene >= SCENE_NUM_STATES)
        return 0.0f;
    return sm->hint_duration[scene];
}

bool state_machine_needs_boost(const StateMachine *sm, float current_load,
                                float heavy_load_threshold) {
    if (sm->heavy_load_active)
        return true;

    /* Check burst slack cooldown */
    if (sm->last_boost_exit_ms > 0) {
        if (now_ms() - sm->last_boost_exit_ms <
            (uint64_t)sm->request_burst_slack_ms)
            return false;
    }

    return current_load > heavy_load_threshold;
}

void state_machine_apply_thermal_reduction(StateMachine *sm, float reduction) {
    if (!sm) return;

    /* Clamp to [0.0, 1.0] */
    if (reduction < 0.0f) reduction = 0.0f;
    if (reduction > 1.0f) reduction = 1.0f;

    if (reduction != sm->thermal_reduction) {
        log_info("Thermal reduction: %.0f%% -> %.0f%%",
                 sm->thermal_reduction * 100.0f, reduction * 100.0f);
        sm->thermal_reduction = reduction;
    }
}

float state_machine_get_thermal_reduction(const StateMachine *sm) {
    return sm ? sm->thermal_reduction : 0.0f;
}
