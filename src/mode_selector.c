#include "mode_selector.h"

/* MODE_FAST is an alias for MODE_PERFORMANCE everywhere in the runtime; collapse
 * it here so the selector never emits MODE_FAST. */
static PowerMode normalize(PowerMode mode) {
    if (mode == MODE_FAST) return MODE_PERFORMANCE;
    if (mode < 0 || mode >= MODE_NUM) return MODE_BALANCE;
    return mode;
}

static bool identity_matches(ProcessIdentity a, ProcessIdentity b) {
    return a.pid > 0 && a.pid == b.pid && a.start_time == b.start_time;
}

PowerMode mode_selector_select(const ModeCandidate *candidates, size_t count,
                               ProcessIdentity active, PowerMode requested) {
    requested = normalize(requested);
    if (!candidates || count == 0) return requested;

    /* 1. Foreground-first: an exact identity match wins outright. */
    if (active.pid > 0) {
        for (size_t i = 0; i < count; i++) {
            if (identity_matches(active, candidates[i].process)) {
                PowerMode fg = normalize(candidates[i].mode);
                /* balance == "no per-app override" -> user's baseline. */
                return fg == MODE_BALANCE ? requested : fg;
            }
        }
    }

    /* 2. Global fallback with deterministic priority (scan-order independent):
     *    performance > powersave > requested. */
    bool any_performance = false;
    bool any_powersave = false;
    for (size_t i = 0; i < count; i++) {
        PowerMode m = normalize(candidates[i].mode);
        if (m == MODE_PERFORMANCE) any_performance = true;
        else if (m == MODE_POWERSAVE) any_powersave = true;
    }
    if (any_performance) return MODE_PERFORMANCE;
    if (any_powersave) return MODE_POWERSAVE;
    return requested;
}
