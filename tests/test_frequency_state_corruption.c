#define _POSIX_C_SOURCE 200809L
#include "frequency_recovery.h"
#include "frequency_state.h"
#include "runtime_backend.h"
#include "sysfs_writer.h"

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* P0.4: a corrupted or truncated frequency-state file must never be applied.
 * The loader is expected to reject the whole file (no partial entries), and
 * recovery must surface an error while leaving the snapshot on disk so a later
 * retry can act on a fixed file. */

static char g_root[] = "/tmp/uperf-state-corrupt-XXXXXX";
static char g_state[256];

static void write_raw(const char *contents) {
    FILE *fp = fopen(g_state, "w");
    assert(fp != NULL);
    if (contents[0] != '\0')
        assert(fputs(contents, fp) >= 0);
    assert(fclose(fp) == 0);
}

/* Load must fail and yield zero entries. */
static void expect_load_rejected(const char *label, const char *contents) {
    write_raw(contents);
    FrequencyStateEntry entries[FREQUENCY_STATE_MAX_ENTRIES] = {0};
    size_t count = 123; /* poisoned: loader must reset to 0 */
    int rc = frequency_state_load(g_state, entries,
                                  FREQUENCY_STATE_MAX_ENTRIES, &count);
    printf("  %-22s rc=%d count=%zu -> ", label, rc, count);
    assert(rc == FREQUENCY_STATE_ERROR);
    assert(count == 0);
    puts("rejected (no entries)");
}

/* A well-formed file must still load, proving the rejections above are not
 * simply "reject everything". */
static void expect_load_ok(const char *label, const char *contents,
                           size_t want) {
    write_raw(contents);
    FrequencyStateEntry entries[FREQUENCY_STATE_MAX_ENTRIES] = {0};
    size_t count = 0;
    int rc = frequency_state_load(g_state, entries,
                                  FREQUENCY_STATE_MAX_ENTRIES, &count);
    printf("  %-22s rc=%d count=%zu -> ", label, rc, count);
    assert(rc == FREQUENCY_STATE_OK);
    assert(count == want);
    puts("loaded");
}

int main(void) {
    assert(mkdtemp(g_root) != NULL);
    snprintf(g_state, sizeof(g_state), "%s/frequency-state", g_root);

    /* A minimal sysfs root so recovery can attempt (and fail on) a read. */
    char proc[128], sysfs[128], policy[192], min_file[256], max_file[256];
    snprintf(proc, sizeof(proc), "%s/proc", g_root);
    snprintf(sysfs, sizeof(sysfs), "%s/sys", g_root);
    snprintf(policy, sizeof(policy), "%s/policy0", sysfs);
    snprintf(min_file, sizeof(min_file), "%s/min", policy);
    snprintf(max_file, sizeof(max_file), "%s/max", policy);
    assert(mkdir(proc, 0700) == 0);
    assert(mkdir(sysfs, 0700) == 0);
    assert(mkdir(policy, 0700) == 0);
    write_raw("");            /* placeholder, replaced per-case */
    FILE *mf = fopen(min_file, "w"); assert(mf && fputs("500", mf) >= 0 && fclose(mf) == 0);
    FILE *xf = fopen(max_file, "w"); assert(xf && fputs("700", xf) >= 0 && fclose(xf) == 0);
    assert(runtime_backend_configure(proc, sysfs, NULL, NULL) == 0);

    puts("=== frequency_state corruption ===");

    /* Structurally malformed content. */
    expect_load_rejected("garbage",        "this is not a valid state file\n");
    expect_load_rejected("truncated_row",  "/sys/policy0/min\t/sys/policy0/max\t300\n");
    expect_load_rejected("too_many_fields","/sys/policy0/min\t/sys/policy0/max\t300\t900\textra\n");
    expect_load_rejected("non_numeric",    "/sys/policy0/min\t/sys/policy0/max\tlow\thigh\n");
    expect_load_rejected("negative_value", "/sys/policy0/min\t/sys/policy0/max\t-1\t900\n");
    expect_load_rejected("min_gt_max",     "/sys/policy0/min\t/sys/policy0/max\t900\t300\n");
    expect_load_rejected("relative_path",  "policy0/min\tpolicy0/max\t300\t900\n");
    expect_load_rejected("empty_file",     "");
    expect_load_rejected("only_newlines",  "\n\n\n");

    /* A trailing corrupt row poisons the whole file: the first row must NOT be
     * partially applied. */
    expect_load_rejected("good_then_bad",
        "/sys/policy0/min\t/sys/policy0/max\t300\t900\n"
        "/sys/policy1/min\t/sys/policy1/max\tbad\tbad\n");

    /* Duplicate paths are rejected (ambiguous restore target). */
    expect_load_rejected("duplicate_paths",
        "/sys/policy0/min\t/sys/policy0/max\t300\t900\n"
        "/sys/policy0/min\t/sys/policy0/max\t400\t800\n");

    /* Overlong value field (>= FREQUENCY_STATE_VALUE_LEN). */
    expect_load_rejected("overlong_value",
        "/sys/policy0/min\t/sys/policy0/max\t3000000000000000000000000000000000\t900\n");

    /* Sanity: a valid file still loads. */
    expect_load_ok("valid_single",
        "/sys/policy0/min\t/sys/policy0/max\t300\t900\n", 1);
    expect_load_ok("valid_double",
        "/sys/policy0/min\t/sys/policy0/max\t300\t900\n"
        "/sys/policy1/min\t/sys/policy1/max\t400\t800\n", 2);

    /* End-to-end: recovery over a corrupt snapshot must error AND preserve the
     * file for retry, and must not modify hardware. */
    puts("=== recovery over corrupt snapshot ===");
    write_raw("/sys/policy0/min\t/sys/policy0/max\tNOPE\t900\n");
    Config config = {0};
    SysfsWriter *writer = sysfs_writer_new(&config);
    assert(writer != NULL);
    size_t restored = 999;
    int rc = frequency_recovery_restore(g_state, writer, &restored);
    printf("  recovery rc=%d restored=%zu\n", rc, restored);
    assert(rc == FREQUENCY_RECOVERY_ERROR);
    assert(restored == 0);
    assert(access(g_state, F_OK) == 0);   /* snapshot preserved for retry */

    /* Hardware untouched by the failed recovery. */
    char *min_now = sysfs_reader_read("/sys/policy0/min");
    char *max_now = sysfs_reader_read("/sys/policy0/max");
    assert(min_now && strcmp(min_now, "500") == 0);
    assert(max_now && strcmp(max_now, "700") == 0);
    free(min_now);
    free(max_now);
    sysfs_writer_free(writer);

    runtime_backend_reset();
    unlink(g_state);
    unlink(min_file);
    unlink(max_file);
    rmdir(policy);
    rmdir(sysfs);
    rmdir(proc);
    rmdir(g_root);
    puts("PASS");
    return 0;
}
