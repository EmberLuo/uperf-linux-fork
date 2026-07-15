#include "config.h"
#include "log.h"
#include "state_machine.h"

#include <json-c/json.h>
#include <json-c/json_util.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <sys/stat.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>

/* ----------------------------------------------------------------
 * JSON parsing helpers (fixed for json-c 0.19 API)
 * ---------------------------------------------------------------- */

static int parse_int_field(struct json_object *obj, const char *field, int *out) {
    struct json_object *jo;
    if (!json_object_object_get_ex(obj, field, &jo))
        return -1;
    *out = (int)json_object_get_int(jo);
    return 0;
}

static int parse_double_field(struct json_object *obj, const char *field, double *out) {
    struct json_object *jo;
    if (!json_object_object_get_ex(obj, field, &jo))
        return -1;
    *out = json_object_get_double(jo);
    return 0;
}

/* Helper: parse a double field into a float variable */
static int parse_float_field(struct json_object *obj, const char *field, float *out) {
    double tmp;
    int rc = parse_double_field(obj, field, &tmp);
    if (rc == 0) *out = (float)tmp;
    return rc;
}

/* Parse a string field safely */
static int parse_string_field(struct json_object *obj, const char *field, const char **out) {
    struct json_object *jo;
    if (!json_object_object_get_ex(obj, field, &jo))
        return -1;
    *out = json_object_get_string(jo);
    return 0;
}

static bool parse_bool_field(struct json_object *obj, const char *field, bool def) {
    struct json_object *jo;
    if (!json_object_object_get_ex(obj, field, &jo))
        return def;
    return json_object_get_boolean(jo);
}

/* Map SceneState to StatePresets member pointer */
static ActionParams *get_state_action(StatePresets *sp, SceneState scene) {
    switch (scene) {
        case SCENE_IDLE:      return &sp->idle;
        case SCENE_TOUCH:     return &sp->touch;
        case SCENE_TRIGGER:   return &sp->trigger;
        case SCENE_GESTURE:   return &sp->gesture;
        case SCENE_JUNK:      return &sp->junk;
        case SCENE_SWITCH:    return &sp->switch_;
        case SCENE_BOOST:     return &sp->boost;
        default:              return &sp->global;
    }
}

/* ----------------------------------------------------------------
 * Power model parsing
 * ---------------------------------------------------------------- */

static int parse_power_model(struct json_object *mod_obj, Config *cfg) {
    struct json_object *arr;
    if (!json_object_object_get_ex(mod_obj, "powerModel", &arr)) {
        log_error("Missing 'powerModel' array in cpu module");
        return -1;
    }

    int nr = json_object_array_length(arr);
    if (nr <= 0 || nr > MAX_CLUSTERS) {
        log_error("Invalid powerModel array length: %d (expected 1-%d)", nr, MAX_CLUSTERS);
        return -1;
    }

    cfg->cpu.nr_clusters = nr;
    for (int i = 0; i < nr; i++) {
        struct json_object *entry = json_object_array_get_idx(arr, i);
        PowerModelEntry *pm = &cfg->cpu.power_model[i];

        memset(pm, 0, sizeof(*pm));

        if (parse_int_field(entry, "efficiency", &pm->efficiency) < 0) {
            log_error("powerModel[%d]: missing 'efficiency'", i);
            return -1;
        }
        if (parse_int_field(entry, "nr", &pm->nr_cores) < 0) {
            log_error("powerModel[%d]: missing 'nr'", i);
            return -1;
        }
        if (parse_float_field(entry, "typicalPower", &pm->typical_power_w) < 0)
            pm->typical_power_w = 1.0f;
        if (parse_float_field(entry, "typicalFreq", &pm->typical_freq_mhz) < 0)
            pm->typical_freq_mhz = 1000.0f;
        if (parse_float_field(entry, "sweetFreq", &pm->sweet_freq_mhz) < 0)
            pm->sweet_freq_mhz = pm->typical_freq_mhz * 0.7f;
        if (parse_float_field(entry, "plainFreq", &pm->plain_freq_mhz) < 0)
            pm->plain_freq_mhz = pm->typical_freq_mhz * 0.5f;
        if (parse_float_field(entry, "freeFreq", &pm->free_freq_mhz) < 0)
            pm->free_freq_mhz = pm->typical_freq_mhz * 0.25f;

        log_debug("powerModel[%d]: eff=%d nr=%d typP=%.1fW typF=%.0fMHz sweet=%.0f plain=%.0f free=%.0f",
                   i, pm->efficiency, pm->nr_cores,
                   pm->typical_power_w, pm->typical_freq_mhz,
                   pm->sweet_freq_mhz, pm->plain_freq_mhz, pm->free_freq_mhz);
    }
    return 0;
}

/* ----------------------------------------------------------------
 * Preset / ActionParams parsing
 * ---------------------------------------------------------------- */

static int parse_action_params(struct json_object *obj, ActionParams *ap) {
    memset(ap, 0, sizeof(*ap));

    json_object_object_foreach(obj, k, v) {
        if (strncmp(k, "cpu.", 4) != 0)
            continue;
        const char *sub = k + 4;
        double d = json_object_get_double(v);
        if (strcmp(sub, "latencyTime") == 0)
            ap->latency_time = (float)d;
        else if (strcmp(sub, "slowLimitPower") == 0)
            ap->slow_limit_power = (float)d;
        else if (strcmp(sub, "fastLimitPower") == 0)
            ap->fast_limit_power = (float)d;
        else if (strcmp(sub, "fastLimitCapacity") == 0)
            ap->fast_limit_capacity = (float)d;
        else if (strcmp(sub, "fastLimitRecoverScale") == 0)
            ap->fast_limit_recover_scale = (float)d;
        else if (strcmp(sub, "margin") == 0)
            ap->margin = (float)d;
        else if (strcmp(sub, "burst") == 0)
            ap->burst = (float)d;
        else if (strcmp(sub, "guideCap") == 0)
            ap->guide_cap = (bool)d;
        else if (strcmp(sub, "limitEfficiency") == 0)
            ap->limit_efficiency = (bool)d;
        else if (strcmp(sub, "baseSampleTime") == 0)
            ap->base_sample_time = (float)d;
        else if (strcmp(sub, "baseSlackTime") == 0)
            ap->base_slack_time = (float)d;
        else if (strcmp(sub, "predictThd") == 0)
            ap->predict_thd = (float)d;
    }
    return 0;
}

/* ----------------------------------------------------------------
 * Sysfs knobs parsing
 * ---------------------------------------------------------------- */

static int parse_sysfs_knobs(struct json_object *mod_obj, Config *cfg) {
    struct json_object *knobs_obj;
    if (!json_object_object_get_ex(mod_obj, "knob", &knobs_obj)) {
        log_warn("sysfs module: no 'knob' object found");
        return 0;  /* Optional */
    }

    int idx = 0;
    json_object_object_foreach(knobs_obj, k, v) {
        if (idx >= MAX_KNOBS) {
            log_warn("sysfs: too many knobs (max %d), truncating", MAX_KNOBS);
            break;
        }
        KnobDef *knob = &cfg->sysfs.knobs[idx];
        strncpy(knob->name, k, MAX_NAME_LEN - 1);
        knob->name[MAX_NAME_LEN - 1] = '\0';
        strncpy(knob->path, json_object_get_string(v), MAX_PATH_LEN - 1);
        knob->path[MAX_PATH_LEN - 1] = '\0';
        knob->enabled = true;
        idx++;
        log_debug("sysfs knob[%d]: %s = %s", idx - 1, knob->name, knob->path);
    }
    cfg->sysfs.nr_knobs = idx;
    return 0;
}

/* ----------------------------------------------------------------
 * Sched rules parsing
 * ---------------------------------------------------------------- */

static AffinityClass parse_affinity_class(const char *s) {
    if (!s) return AC_AUTO;
    if (strcmp(s, "bg") == 0)   return AC_BG;
    if (strcmp(s, "norm") == 0) return AC_NORM;
    if (strcmp(s, "coop") == 0) return AC_COOP;
    if (strcmp(s, "ui") == 0)   return AC_UI;
    if (strcmp(s, "rtusr") == 0) return AC_RTUSR;
    if (strcmp(s, "all") == 0)  return AC_NORM;
    return AC_AUTO;
}

static int parse_sched_rules(struct json_object *mod_obj, Config *cfg) {
    struct json_object *rules_arr;
    if (!json_object_object_get_ex(mod_obj, "rules", &rules_arr))
        return 0;  /* Optional */

    int nr = json_object_array_length(rules_arr);
    if (nr > MAX_RULES) nr = MAX_RULES;

    cfg->sched.nr_rules = nr;
    for (int i = 0; i < nr; i++) {
        struct json_object *rule_obj = json_object_array_get_idx(rules_arr, i);
        SchedRule *rule = &cfg->sched.rules[i];
        memset(rule, 0, sizeof(*rule));

        const char *name_str = NULL;
        parse_string_field(rule_obj, "name", &name_str);
        if (name_str)
            strncpy(rule->name, name_str, MAX_NAME_LEN - 1);

        const char *regex_str = NULL;
        parse_string_field(rule_obj, "regex", &regex_str);
        if (regex_str)
            strncpy(rule->regex, regex_str, MAX_PATH_LEN - 1);

        rule->pinned = parse_bool_field(rule_obj, "pinned", false);

        /* Parse nested rules array for affinity/priority */
        struct json_object *nested;
        if (json_object_object_get_ex(rule_obj, "rules", &nested)) {
            int nn = json_object_array_length(nested);
            if (nn > 0) {
                struct json_object *first = json_object_array_get_idx(nested, 0);
                const char *ac_str = NULL;
                parse_string_field(first, "ac", &ac_str);
                rule->affinity_class = parse_affinity_class(ac_str);

                const char *pc_str = NULL;
                parse_string_field(first, "pc", &pc_str);
                if (pc_str) {
                    if (strcmp(pc_str, "bg") == 0)   rule->prio_profile.fg = -3;
                    else if (strcmp(pc_str, "norm") == 0) rule->prio_profile.fg = -1;
                    else if (strcmp(pc_str, "coop") == 0) rule->prio_profile.fg = 124;
                    else if (strcmp(pc_str, "ui") == 0)   rule->prio_profile.fg = 120;
                    else if (strcmp(pc_str, "rtusr") == 0) rule->prio_profile.fg = 98;
                    else                                   rule->prio_profile.fg = 0;
                }
            }
        }

        log_debug("sched rule[%d]: %s regex=%s pinned=%d ac=%d prio_fg=%d",
                   i, rule->name, rule->regex, rule->pinned,
                   rule->affinity_class, rule->prio_profile.fg);
    }
    return 0;
}

/* ----------------------------------------------------------------
 * Presets (balance/powersave/performance) parsing
 * ---------------------------------------------------------------- */

static PowerMode mode_from_string(const char *s) {
    if (!s) return MODE_BALANCE;
    if (strcmp(s, "balance") == 0)       return MODE_BALANCE;
    if (strcmp(s, "powersave") == 0)     return MODE_POWERSAVE;
    if (strcmp(s, "performance") == 0)   return MODE_PERFORMANCE;
    if (strcmp(s, "fast") == 0)          return MODE_FAST;
    return MODE_BALANCE;
}

static SceneState scene_from_string(const char *s) {
    if (!s) return SCENE_IDLE;
    if (strcmp(s, "idle") == 0)      return SCENE_IDLE;
    if (strcmp(s, "touch") == 0)     return SCENE_TOUCH;
    if (strcmp(s, "trigger") == 0)   return SCENE_TRIGGER;
    if (strcmp(s, "gesture") == 0)   return SCENE_GESTURE;
    if (strcmp(s, "junk") == 0)      return SCENE_JUNK;
    if (strcmp(s, "switch") == 0)    return SCENE_SWITCH;
    if (strcmp(s, "boost") == 0)     return SCENE_BOOST;
    return SCENE_IDLE;
}

static int parse_presets(struct json_object *cfg_obj, Config *out) {
    struct json_object *presets_obj;
    if (!json_object_object_get_ex(cfg_obj, "presets", &presets_obj)) {
        log_error("Missing 'presets' object in config");
        return -1;
    }

    /* Iterate each power mode preset */
    json_object_object_foreach(presets_obj, mode_name, mode_obj) {
        PowerMode mode = mode_from_string(mode_name);
        if (mode >= MODE_NUM) {
            log_warn("Unknown preset mode: %s, skipping", mode_name);
            continue;
        }

        PowerModePreset *preset = &out->presets[mode];
        strncpy(preset->name, mode_name, MAX_NAME_LEN - 1);

        /* Each mode has state sub-objects: idle, touch, trigger, etc. */
        json_object_object_foreach(mode_obj, state_name, state_obj) {
            /* Parse global defaults (*) */
            if (strcmp(state_name, "*") == 0) {
                parse_action_params(state_obj, &preset->presets.global);
                continue;
            }

            /* Map state name to enum and get ActionParams pointer */
            SceneState scene = scene_from_string(state_name);
            parse_action_params(state_obj, get_state_action(&preset->presets, scene));
        }
    }
    return 0;
}

/* ----------------------------------------------------------------
 * Initials parsing
 * ---------------------------------------------------------------- */

static int parse_initials(struct json_object *cfg_obj, Config *out) {
    struct json_object *initials_obj;
    if (!json_object_object_get_ex(cfg_obj, "initials", &initials_obj))
        return 0;  /* Optional */

    struct json_object *cpu_obj;
    if (json_object_object_get_ex(initials_obj, "cpu", &cpu_obj)) {
        out->initial_latency_time       = 0.2f;
        out->initial_slow_limit_power   = 3.0f;
        out->initial_fast_limit_power   = 6.0f;
        out->initial_fast_limit_capacity = 10.0f;
        out->initial_fast_limit_recover_scale = 0.3f;
        out->initial_margin             = 0.25f;
        out->initial_burst              = 0.0f;
        out->initial_guide_cap          = false;
        out->initial_limit_efficiency   = false;
        out->initial_base_sample_time   = 0.01f;
        out->initial_base_slack_time    = 0.01f;
        out->initial_predict_thd        = 0.3f;

        json_object_object_foreach(cpu_obj, k, v) {
            double d = json_object_get_double(v);
            if (strcmp(k, "latencyTime") == 0)       out->initial_latency_time = (float)d;
            else if (strcmp(k, "slowLimitPower") == 0) out->initial_slow_limit_power = (float)d;
            else if (strcmp(k, "fastLimitPower") == 0) out->initial_fast_limit_power = (float)d;
            else if (strcmp(k, "fastLimitCapacity") == 0) out->initial_fast_limit_capacity = (float)d;
            else if (strcmp(k, "fastLimitRecoverScale") == 0) out->initial_fast_limit_recover_scale = (float)d;
            else if (strcmp(k, "margin") == 0) out->initial_margin = (float)d;
            else if (strcmp(k, "burst") == 0) out->initial_burst = (float)d;
            else if (strcmp(k, "guideCap") == 0) out->initial_guide_cap = (bool)d;
            else if (strcmp(k, "limitEfficiency") == 0) out->initial_limit_efficiency = (bool)d;
            else if (strcmp(k, "baseSampleTime") == 0) out->initial_base_sample_time = (float)d;
            else if (strcmp(k, "baseSlackTime") == 0) out->initial_base_slack_time = (float)d;
            else if (strcmp(k, "predictThd") == 0) out->initial_predict_thd = (float)d;
        }
    }
    return 0;
}

/* ----------------------------------------------------------------
 * Meta parsing
 * ---------------------------------------------------------------- */

static int parse_meta(struct json_object *cfg_obj, Config *out) {
    struct json_object *meta_obj;
    if (!json_object_object_get_ex(cfg_obj, "meta", &meta_obj))
        return 0;  /* Optional */

    struct json_object *name_jo = NULL;
    if (json_object_object_get_ex(meta_obj, "name", &name_jo)) {
        const char *name = json_object_get_string(name_jo);
        if (name)
            strncpy(out->meta_name, name, MAX_NAME_LEN - 1);
    }

    struct json_object *author_jo = NULL;
    if (json_object_object_get_ex(meta_obj, "author", &author_jo)) {
        const char *author = json_object_get_string(author_jo);
        if (author)
            strncpy(out->meta_author, author, MAX_NAME_LEN - 1);
    }

    return 0;
}

/* ----------------------------------------------------------------
 * Switcher / Input module parsing
 * ---------------------------------------------------------------- */

static int parse_switcher(struct json_object *cfg_obj, Config *out) {
    struct json_object *mod_obj;
    if (!json_object_object_get_ex(cfg_obj, "modules", &mod_obj))
        return -1;

    struct json_object *sw_obj;
    if (json_object_object_get_ex(mod_obj, "switcher", &sw_obj)) {
        struct json_object *jo;
        const char *s;
        if (json_object_object_get_ex(sw_obj, "switchInode", &jo)) {
            s = json_object_get_string(jo);
            if (s) strncpy(out->switcher.switch_inode, s, MAX_PATH_LEN - 1);
        }
        if (json_object_object_get_ex(sw_obj, "perapp", &jo)) {
            s = json_object_get_string(jo);
            if (s) strncpy(out->switcher.perapp_file, s, MAX_PATH_LEN - 1);
        }

        /* hintDuration */
        struct json_object *hd;
        if (json_object_object_get_ex(sw_obj, "hintDuration", &hd)) {
            json_object_object_foreach(hd, sn, sv) {
                SceneState scene = scene_from_string(sn);
                out->switcher.hint_duration[scene] = json_object_get_double(sv);
            }
        }
    }

    /* Input module */
    struct json_object *inp_obj;
    if (json_object_object_get_ex(mod_obj, "input", &inp_obj)) {
        out->input.enable = parse_bool_field(inp_obj, "enable", true);
        out->input.swipe_thd = 0.03f;
        out->input.gesture_thd_x = 0.03f;
        out->input.gesture_thd_y = 0.03f;
        out->input.gesture_delay_time = 2.0f;
        out->input.hold_enter_time = 1.0f;

        struct json_object *jo;
        if (json_object_object_get_ex(inp_obj, "swipeThd", &jo))
            out->input.swipe_thd = json_object_get_double(jo);
        if (json_object_object_get_ex(inp_obj, "gestureThdX", &jo))
            out->input.gesture_thd_x = json_object_get_double(jo);
        if (json_object_object_get_ex(inp_obj, "gestureThdY", &jo))
            out->input.gesture_thd_y = json_object_get_double(jo);
        if (json_object_object_get_ex(inp_obj, "gestureDelayTime", &jo))
            out->input.gesture_delay_time = json_object_get_double(jo);
        if (json_object_object_get_ex(inp_obj, "holdEnterTime", &jo))
            out->input.hold_enter_time = json_object_get_double(jo);
    }

    return 0;
}

/* ----------------------------------------------------------------
 * Main entry: config_load
 * ---------------------------------------------------------------- */

int config_load(Config *cfg, const char *path) {
    memset(cfg, 0, sizeof(*cfg));

    /* Defaults */
    cfg->switcher.hint_duration[SCENE_IDLE]      = 0.0f;
    cfg->switcher.hint_duration[SCENE_TOUCH]     = 4.0f;
    cfg->switcher.hint_duration[SCENE_TRIGGER]   = 0.03f;
    cfg->switcher.hint_duration[SCENE_GESTURE]   = 0.1f;
    cfg->switcher.hint_duration[SCENE_JUNK]      = 0.06f;
    cfg->switcher.hint_duration[SCENE_SWITCH]    = 0.4f;
    cfg->switcher.hint_duration[SCENE_BOOST]     = 0.0f;

    strncpy(cfg->switcher.switch_inode, "/run/uperf-linux/cur_powermode", MAX_PATH_LEN - 1);
    strncpy(cfg->switcher.perapp_file, "/etc/uperf-linux/perapp_powermode", MAX_PATH_LEN - 1);

    strncpy(cfg->meta_name, "unknown", MAX_NAME_LEN - 1);
    strncpy(cfg->meta_author, "unknown", MAX_NAME_LEN - 1);

    /* Parse JSON using json_object_from_file (replaces deprecated json_parse_file) */
    struct json_object *root = json_object_from_file(path);
    if (!root) {
        log_error("Failed to parse JSON config '%s': file not found or invalid JSON", path);
        return -1;
    }

    /* Resolve module objects once, then pass each parser the level it expects. */
    struct json_object *modules_obj = NULL;
    struct json_object *cpu_obj = NULL;
    struct json_object *sysfs_obj = NULL;
    struct json_object *sched_obj = NULL;

    if (!json_object_object_get_ex(root, "modules", &modules_obj)) {
        log_error("Missing 'modules' object in config");
        goto err;
    }
    if (!json_object_object_get_ex(modules_obj, "cpu", &cpu_obj)) {
        log_error("Missing 'modules.cpu' object in config");
        goto err;
    }
    if (!json_object_object_get_ex(modules_obj, "sysfs", &sysfs_obj)) {
        log_error("Missing 'modules.sysfs' object in config");
        goto err;
    }

    cfg->cpu.enable = parse_bool_field(cpu_obj, "enable", true);
    cfg->sysfs.enable = parse_bool_field(sysfs_obj, "enable", true);

    /* Walk the top-level structure. */
    if (parse_meta(root, cfg) < 0)               goto err;
    if (parse_switcher(root, cfg) < 0)           goto err;
    if (parse_power_model(cpu_obj, cfg) < 0)     goto err;
    if (parse_sysfs_knobs(sysfs_obj, cfg) < 0)   goto err;
    if (json_object_object_get_ex(modules_obj, "sched", &sched_obj)) {
        cfg->sched.enable = parse_bool_field(sched_obj, "enable", true);
        if (parse_sched_rules(sched_obj, cfg) < 0) goto err;
    }
    if (parse_presets(root, cfg) < 0)            goto err;
    if (parse_initials(root, cfg) < 0)           goto err;

    json_object_put(root);
    log_info("Config loaded: %s by %s (%d clusters, %d knobs, %d rules)",
             cfg->meta_name, cfg->meta_author,
             cfg->cpu.nr_clusters, cfg->sysfs.nr_knobs, cfg->sched.nr_rules);
    return 0;

err:
    json_object_put(root);
    log_error("Config parse failed for '%s'", path);
    return -1;
}

/* ----------------------------------------------------------------
 * config_validate
 * ---------------------------------------------------------------- */

int config_validate(const Config *cfg) {
    if (!cfg) {
        log_error("Validation failed: config is NULL");
        return -1;
    }

    if (cfg->cpu.nr_clusters <= 0 || cfg->cpu.nr_clusters > MAX_CLUSTERS) {
        log_error("Validation failed: cluster count must be between 1 and %d",
                  MAX_CLUSTERS);
        return -1;
    }

    for (int i = 0; i < cfg->cpu.nr_clusters; i++) {
        const PowerModelEntry *pm = &cfg->cpu.power_model[i];
        if (pm->nr_cores <= 0) {
            log_error("Validation failed: cluster %d nr must be > 0", i);
            return -1;
        }
        if (pm->typical_freq_mhz <= 0) {
            log_error("Validation failed: cluster %d typicalFreq must be > 0", i);
            return -1;
        }
        if (pm->efficiency <= 0) {
            log_error("Validation failed: cluster %d efficiency must be > 0", i);
            return -1;
        }
        if (pm->typical_power_w <= 0.0f) {
            log_error("Validation failed: cluster %d typicalPower must be > 0", i);
            return -1;
        }
    }

    bool has_preset = false;
    for (int m = 0; m < MODE_NUM; m++) {
        if (cfg->presets[m].name[0] != '\0') {
            has_preset = true;
            break;
        }
    }
    if (!has_preset) {
        log_error("Validation failed: no power mode presets defined");
        return -1;
    }

    log_info("Config validation passed (%d clusters, %d modes)",
             cfg->cpu.nr_clusters, MODE_NUM);
    return 0;
}

/* ----------------------------------------------------------------
 * config_check_paths
 * ---------------------------------------------------------------- */

int config_check_paths(const Config *cfg) {
    int missing = 0;
    int total = 0;

    for (int i = 0; i < cfg->sysfs.nr_knobs; i++) {
        const KnobDef *knob = &cfg->sysfs.knobs[i];
        if (!knob->enabled) continue;
        total++;

        int fd = open(knob->path, O_WRONLY);
        if (fd < 0) {
            if (errno == ENOENT) {
                log_warn("Config path not found: %s (%s)", knob->path, knob->name);
                missing++;
            } else {
                log_error("Cannot write to config path: %s (%s): %s",
                          knob->path, knob->name, strerror(errno));
                return -1;
            }
        } else {
            close(fd);
        }
    }

    const char *dir = strrchr(cfg->switcher.switch_inode, '/');
    if (dir) {
        char dirpath[MAX_PATH_LEN];
        size_t dirlen = (size_t)(dir - cfg->switcher.switch_inode);
        if (dirlen >= MAX_PATH_LEN) dirlen = MAX_PATH_LEN - 1;
        strncpy(dirpath, cfg->switcher.switch_inode, dirlen);
        dirpath[dirlen] = '\0';
        struct stat st;
        if (stat(dirpath, &st) != 0 || !S_ISDIR(st.st_mode)) {
            log_warn("Switch inode directory does not exist: %s (will be created at runtime)", dirpath);
        }
    }

    if (missing > 0) {
        log_warn("%d/%d sysfs paths missing (non-fatal: they may appear at runtime)",
                 missing, total);
    } else if (total > 0) {
        log_info("All %d sysfs paths verified OK", total);
    }

    return 0;
}

void config_free(Config *cfg) {
    (void)cfg;
}
