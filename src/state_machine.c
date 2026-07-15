#define _GNU_SOURCE
#include "state_machine.h"
#include "log.h"

#include <string.h>
#include <stdlib.h>
#include <math.h>
#include <time.h>

/* Helper: monotonic time in milliseconds */
static uint64_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000ULL + ts.tv_nsec / 1000000ULL;
}

/* Internal state machine implementation */
struct StateMachine {
    SceneState    current_scene;
    PowerMode     current_mode;
    uint64_t      enter_time_ms;  /* monotonic ms when we entered current scene */

    /* Per-mode, per-scene action parameters (populated from presets at init) */
    ActionParams actions[MODE_NUM][SCENE_NUM_STATES];

    /* Config reference */
    const Config *cfg;

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

/* Map SceneState to actions array index */
static int scene_idx(SceneState s) { return (int)s; }
static int mode_idx(PowerMode m) { return (int)m; }

/* Copy scene-specific ActionParams over base, only overwriting non-zero fields */
static void action_params_overlay(ActionParams *dst, const ActionParams *src) {
    if (src->latency_time > 0.0f)               dst->latency_time = src->latency_time;
    if (src->slow_limit_power > 0.0f)           dst->slow_limit_power = src->slow_limit_power;
    if (src->fast_limit_power > 0.0f)           dst->fast_limit_power = src->fast_limit_power;
    if (src->fast_limit_capacity > 0.0f)        dst->fast_limit_capacity = src->fast_limit_capacity;
    if (src->fast_limit_recover_scale > 0.0f)   dst->fast_limit_recover_scale = src->fast_limit_recover_scale;
    if (src->margin > 0.0f)                     dst->margin = src->margin;
    if (src->burst > 0.0f)                      dst->burst = src->burst;
    if (src->guide_cap)                         dst->guide_cap = src->guide_cap;
    if (src->limit_efficiency)                  dst->limit_efficiency = src->limit_efficiency;
    if (src->base_sample_time > 0.0f)           dst->base_sample_time = src->base_sample_time;
    if (src->base_slack_time > 0.0f)            dst->base_slack_time = src->base_slack_time;
    if (src->predict_thd > 0.0f)               dst->predict_thd = src->predict_thd;
}

/* Get the ActionParams for the current (mode, scene) pair */
static ActionParams *get_action_params(StateMachine *sm, SceneState scene) {
    int m = mode_idx(sm->current_mode);
    int s = scene_idx(scene);
    return &sm->actions[m][s];
}

StateMachine *state_machine_new(const Config *cfg) {
    StateMachine *sm = calloc(1, sizeof(*sm));
    if (!sm) return NULL;

    sm->cfg = cfg;
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
            memset(&sm->actions[m][s], 0, sizeof(ActionParams));
            /* Start from global preset, then overlay scene-specific */
            memcpy(&sm->actions[m][s], &cfg->presets[m].presets.global,
                   sizeof(ActionParams));
            SceneState scene = (SceneState)s;
            switch (scene) {
                case SCENE_IDLE:    action_params_overlay(&sm->actions[m][s], &cfg->presets[m].presets.idle); break;
                case SCENE_TOUCH:   action_params_overlay(&sm->actions[m][s], &cfg->presets[m].presets.touch); break;
                case SCENE_TRIGGER: action_params_overlay(&sm->actions[m][s], &cfg->presets[m].presets.trigger); break;
                case SCENE_GESTURE: action_params_overlay(&sm->actions[m][s], &cfg->presets[m].presets.gesture); break;
                case SCENE_JUNK:    action_params_overlay(&sm->actions[m][s], &cfg->presets[m].presets.junk); break;
                case SCENE_SWITCH:  action_params_overlay(&sm->actions[m][s], &cfg->presets[m].presets.switch_); break;
                case SCENE_BOOST:   action_params_overlay(&sm->actions[m][s], &cfg->presets[m].presets.boost); break;
                default: break;
            }
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

    if (next != old) {
        log_debug("State transition: %d -> %d (event=%d)",
                   old, next, evt);
        sm->current_scene = next;
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
    if (mode == sm->current_mode) return;

    log_info("Power mode changed: %d -> %d", sm->current_mode, mode);
    sm->current_mode = mode;

    /* Reset to idle in the new mode */
    sm->current_scene = SCENE_IDLE;
    sm->heavy_load_active = false;
    sm->last_boost_exit_ms = 0;
}

void state_machine_get_actions(const StateMachine *sm, ActionParams *out) {
    SceneState scene = sm->current_scene;
    PowerMode  mode  = sm->current_mode;

    ActionParams *src = get_action_params((StateMachine *)sm, scene);
    if (src) {
        memcpy(out, src, sizeof(*out));
    } else {
        memset(out, 0, sizeof(*out));
    }

    /* Apply initial defaults as base */
    out->latency_time       = sm->cfg->initial_latency_time;
    out->slow_limit_power   = sm->cfg->initial_slow_limit_power;
    out->fast_limit_power   = sm->cfg->initial_fast_limit_power;
    out->fast_limit_capacity = sm->cfg->initial_fast_limit_capacity;
    out->fast_limit_recover_scale = sm->cfg->initial_fast_limit_recover_scale;
    out->margin             = sm->cfg->initial_margin;
    out->burst              = sm->cfg->initial_burst;
    out->guide_cap          = sm->cfg->initial_guide_cap;
    out->limit_efficiency   = sm->cfg->initial_limit_efficiency;
    out->base_sample_time   = sm->cfg->initial_base_sample_time;
    out->base_slack_time    = sm->cfg->initial_base_slack_time;
    out->predict_thd        = sm->cfg->initial_predict_thd;
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
        /* TODO: compare against current monotonic time */
        /* For now, skip cooldown check */
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
