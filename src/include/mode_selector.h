#ifndef UPERF_MODE_SELECTOR_H
#define UPERF_MODE_SELECTOR_H

#include <stdint.h>
#include <sys/types.h>
#include <stddef.h>

#include "config.h"

/* Identity of a process that survives PID reuse: a PID is only "the same
 * process" if its start time (/proc/[pid]/stat field 22) also matches. */
typedef struct {
    pid_t    pid;
    uint64_t start_time;
} ProcessIdentity;

/* One detected game and its resolved per-app mode.  This is the ONLY input the
 * selector consumes — the caller resolves per-app rules (by comm, cmdline,
 * package, whatever) up front and hands over a flat table.  That keeps the
 * selector a pure, deterministic function with no scanner/regex dependency,
 * which makes it directly reusable as a Rust golden-test oracle. */
typedef struct {
    ProcessIdentity process;
    PowerMode       mode;   /* per-app resolved mode; MODE_BALANCE = "no override" */
} ModeCandidate;

/* Compute the effective global power mode.
 *
 * Semantics (foreground-first, chosen by the project owner):
 *   1. If `active` identifies one of the candidates exactly (pid AND start_time
 *      match), that foreground game wins:
 *         - its mode, or `requested` when its mode is MODE_BALANCE
 *           ("balance" means "no per-app override, use the user's baseline").
 *   2. Otherwise (no active id, active id matches no candidate, or the
 *      foreground process is not a detected game) fall back to a global scan
 *      with a deterministic priority — independent of scan order:
 *         performance/fast  >  powersave  >  requested
 *      i.e. any candidate asking for performance forces performance; else any
 *      asking for powersave forces powersave; else the user's `requested`
 *      baseline (no override present).
 *
 * MODE_FAST is normalized to MODE_PERFORMANCE in the result.  A NULL/empty
 * candidate list yields `requested`.  Pure function: no I/O, deterministic. */
PowerMode mode_selector_select(const ModeCandidate *candidates, size_t count,
                               ProcessIdentity active, PowerMode requested);

#endif /* UPERF_MODE_SELECTOR_H */
