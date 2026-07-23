#include "mode_selector.h"
#include "golden_trace.h"

#include <stdio.h>
#include <string.h>

static int tests_run = 0;
static int tests_passed = 0;
static int tests_failed = 0;

#define TEST(name) static void name(void)
#define RUN_TEST(name) do { \
    tests_run++; \
    printf("  TEST: %s ... ", #name); \
    name(); \
} while(0)
#define ASSERT_EQ(a, b, msg) do { \
    if ((int)(a) != (int)(b)) { \
        printf("FAIL (%s: expected %d, got %d)\n", msg, (int)(b), (int)(a)); \
        tests_failed++; return; \
    } \
} while(0)
#define ASSERT_PASS(msg) do { \
    printf("PASS\n"); tests_passed++; (void)(msg); \
} while(0)

static ProcessIdentity id(pid_t pid, uint64_t start) {
    ProcessIdentity p = { .pid = pid, .start_time = start };
    return p;
}

/* No candidates -> the caller's baseline flows through untouched. */
TEST(test_null_and_empty_return_requested) {
    ASSERT_EQ(mode_selector_select(NULL, 0, id(0, 0), MODE_PERFORMANCE),
              MODE_PERFORMANCE, "NULL list returns requested");
    ModeCandidate none[1];
    ASSERT_EQ(mode_selector_select(none, 0, id(1234, 5), MODE_POWERSAVE),
              MODE_POWERSAVE, "count 0 returns requested");
    ASSERT_PASS("empty selection preserves baseline");
}

/* requested=performance with only balance candidates -> baseline wins because
 * balance means "no override". */
TEST(test_requested_performance_all_balance) {
    ModeCandidate c[] = {
        { .process = { 10, 100 }, .mode = MODE_BALANCE },
        { .process = { 11, 101 }, .mode = MODE_BALANCE },
    };
    ASSERT_EQ(mode_selector_select(c, 2, id(0, 0), MODE_PERFORMANCE),
              MODE_PERFORMANCE, "all-balance keeps requested performance");
    ASSERT_PASS("balance candidates do not override baseline");
}

/* Global fallback priority: any performance beats any powersave, regardless of
 * array order. */
TEST(test_global_fallback_priority) {
    ModeCandidate c[] = {
        { .process = { 10, 100 }, .mode = MODE_POWERSAVE },
        { .process = { 11, 101 }, .mode = MODE_PERFORMANCE },
    };
    ASSERT_EQ(mode_selector_select(c, 2, id(0, 0), MODE_BALANCE),
              MODE_PERFORMANCE, "performance beats powersave (perf last)");
    ModeCandidate c2[] = {
        { .process = { 11, 101 }, .mode = MODE_PERFORMANCE },
        { .process = { 10, 100 }, .mode = MODE_POWERSAVE },
    };
    ASSERT_EQ(mode_selector_select(c2, 2, id(0, 0), MODE_BALANCE),
              MODE_PERFORMANCE, "performance beats powersave (perf first)");
    ASSERT_PASS("global fallback is order-independent");
}

/* Powersave-only candidates -> powersave wins over a balance baseline. */
TEST(test_global_powersave_coexists) {
    ModeCandidate c[] = {
        { .process = { 10, 100 }, .mode = MODE_POWERSAVE },
        { .process = { 11, 101 }, .mode = MODE_BALANCE },
    };
    ASSERT_EQ(mode_selector_select(c, 2, id(0, 0), MODE_BALANCE),
              MODE_POWERSAVE, "powersave beats balance baseline");
    ASSERT_PASS("powersave and balance coexist");
}

/* Foreground exact-identity match wins outright, even against a performance
 * candidate elsewhere. */
TEST(test_foreground_override_wins) {
    ModeCandidate c[] = {
        { .process = { 10, 100 }, .mode = MODE_PERFORMANCE },
        { .process = { 11, 101 }, .mode = MODE_POWERSAVE },
    };
    ASSERT_EQ(mode_selector_select(c, 2, id(11, 101), MODE_BALANCE),
              MODE_POWERSAVE, "foreground powersave beats bg performance");
    ASSERT_PASS("foreground override takes precedence");
}

/* Foreground A -> B: the winner follows whichever identity is active. */
TEST(test_foreground_switch_a_to_b) {
    ModeCandidate c[] = {
        { .process = { 10, 100 }, .mode = MODE_PERFORMANCE },
        { .process = { 11, 101 }, .mode = MODE_POWERSAVE },
    };
    ASSERT_EQ(mode_selector_select(c, 2, id(10, 100), MODE_BALANCE),
              MODE_PERFORMANCE, "A active -> A's mode");
    ASSERT_EQ(mode_selector_select(c, 2, id(11, 101), MODE_BALANCE),
              MODE_POWERSAVE, "B active -> B's mode");
    ASSERT_PASS("foreground switch tracks active identity");
}

/* An active foreground game whose per-app mode is balance -> user's baseline,
 * NOT a global-scan override from other games. */
TEST(test_foreground_balance_uses_baseline) {
    ModeCandidate c[] = {
        { .process = { 10, 100 }, .mode = MODE_BALANCE },
        { .process = { 11, 101 }, .mode = MODE_PERFORMANCE },
    };
    ASSERT_EQ(mode_selector_select(c, 2, id(10, 100), MODE_POWERSAVE),
              MODE_POWERSAVE, "fg balance -> requested baseline");
    ASSERT_PASS("foreground balance defers to baseline, not other games");
}

/* Same PID, different start_time (PID reuse) must NOT match the foreground. */
TEST(test_pid_reuse_no_match) {
    ModeCandidate c[] = {
        { .process = { 10, 100 }, .mode = MODE_PERFORMANCE },
    };
    /* active pid 10 but a different start_time -> not the same process, so no
     * foreground match; falls back to global scan (performance present). */
    ASSERT_EQ(mode_selector_select(c, 1, id(10, 999), MODE_BALANCE),
              MODE_PERFORMANCE, "reused pid still counts in global scan");
    /* With the candidate as balance, no override remains -> baseline. */
    ModeCandidate c2[] = {
        { .process = { 10, 100 }, .mode = MODE_BALANCE },
    };
    ASSERT_EQ(mode_selector_select(c2, 1, id(10, 999), MODE_POWERSAVE),
              MODE_POWERSAVE, "reused pid does not trigger fg override");
    ASSERT_PASS("start_time guards against PID reuse");
}

/* Same comm but different PID is represented as a distinct identity; the active
 * one is what matters, not name collision. */
TEST(test_same_comm_different_pid) {
    ModeCandidate c[] = {
        { .process = { 20, 200 }, .mode = MODE_PERFORMANCE },
        { .process = { 21, 201 }, .mode = MODE_POWERSAVE },
    };
    ASSERT_EQ(mode_selector_select(c, 2, id(21, 201), MODE_BALANCE),
              MODE_POWERSAVE, "active instance's mode wins");
    ASSERT_PASS("distinct identities resolve independently");
}

/* MODE_FAST normalizes to MODE_PERFORMANCE in every path. */
TEST(test_fast_normalization) {
    ModeCandidate fg[] = {
        { .process = { 10, 100 }, .mode = MODE_FAST },
    };
    ASSERT_EQ(mode_selector_select(fg, 1, id(10, 100), MODE_BALANCE),
              MODE_PERFORMANCE, "fg fast -> performance");
    ModeCandidate bg[] = {
        { .process = { 10, 100 }, .mode = MODE_FAST },
    };
    ASSERT_EQ(mode_selector_select(bg, 1, id(0, 0), MODE_BALANCE),
              MODE_PERFORMANCE, "global fast -> performance");
    ASSERT_EQ(mode_selector_select(NULL, 0, id(0, 0), MODE_FAST),
              MODE_PERFORMANCE, "requested fast -> performance");
    ASSERT_PASS("FAST collapses to PERFORMANCE");
}

/* An out-of-range mode from a lookup is treated as balance, never propagated. */
TEST(test_illegal_mode_from_lookup) {
    ModeCandidate fg[] = {
        { .process = { 10, 100 }, .mode = (PowerMode)999 },
    };
    /* fg illegal -> normalized to balance -> user baseline. */
    ASSERT_EQ(mode_selector_select(fg, 1, id(10, 100), MODE_POWERSAVE),
              MODE_POWERSAVE, "illegal fg mode -> baseline");
    ModeCandidate bg[] = {
        { .process = { 10, 100 }, .mode = (PowerMode)-5 },
    };
    /* bg illegal -> normalized to balance -> no override -> baseline. */
    ASSERT_EQ(mode_selector_select(bg, 1, id(0, 0), MODE_POWERSAVE),
              MODE_POWERSAVE, "illegal bg mode -> baseline");
    /* An illegal requested baseline itself normalizes to balance. */
    ASSERT_EQ(mode_selector_select(NULL, 0, id(0, 0), (PowerMode)42),
              MODE_BALANCE, "illegal requested -> balance");
    ASSERT_PASS("illegal modes are sanitized");
}

/* A zero/negative active pid disables foreground matching entirely. */
TEST(test_no_active_pid) {
    ModeCandidate c[] = {
        { .process = { 0, 0 }, .mode = MODE_PERFORMANCE },
    };
    /* active pid 0 must not match candidate pid 0. */
    ASSERT_EQ(mode_selector_select(c, 1, id(0, 0), MODE_BALANCE),
              MODE_PERFORMANCE, "no active -> global scan only");
    ASSERT_PASS("pid<=0 never matches a candidate");
}

static const char *mode_name(PowerMode mode) {
    switch (mode) {
        case MODE_PERFORMANCE: return "performance";
        case MODE_POWERSAVE: return "powersave";
        default: return "balance";
    }
}

TEST(test_golden_trace) {
    ModeCandidate candidates[] = {
        { .process = { 10, 100 }, .mode = MODE_PERFORMANCE },
        { .process = { 11, 101 }, .mode = MODE_POWERSAVE },
        { .process = { 12, 102 }, .mode = MODE_BALANCE },
    };
    char trace[256];
    snprintf(trace, sizeof(trace),
             "no_gui=%s\n"
             "foreground_performance=%s\n"
             "foreground_powersave=%s\n"
             "foreground_balance=%s\n"
             "unknown_foreground=%s\n",
             mode_name(mode_selector_select(candidates, 3, id(0, 0),
                                            MODE_POWERSAVE)),
             mode_name(mode_selector_select(candidates, 3, id(10, 100),
                                            MODE_POWERSAVE)),
             mode_name(mode_selector_select(candidates, 3, id(11, 101),
                                            MODE_BALANCE)),
             mode_name(mode_selector_select(candidates, 3, id(12, 102),
                                            MODE_POWERSAVE)),
             mode_name(mode_selector_select(candidates, 3, id(99, 999),
                                            MODE_POWERSAVE)));
    if (golden_trace_matches(
            TEST_SOURCE_DIR "/tests/golden/mode_selector.trace", trace) != 0) {
        printf("FAIL (mode selector golden trace mismatch)\n");
        tests_failed++;
        return;
    }
    ASSERT_PASS("mode selector trace is stable for Rust replay");
}

int main(void) {
    printf("=== Mode Selector Tests ===\n");

    RUN_TEST(test_null_and_empty_return_requested);
    RUN_TEST(test_requested_performance_all_balance);
    RUN_TEST(test_global_fallback_priority);
    RUN_TEST(test_global_powersave_coexists);
    RUN_TEST(test_foreground_override_wins);
    RUN_TEST(test_foreground_switch_a_to_b);
    RUN_TEST(test_foreground_balance_uses_baseline);
    RUN_TEST(test_pid_reuse_no_match);
    RUN_TEST(test_same_comm_different_pid);
    RUN_TEST(test_fast_normalization);
    RUN_TEST(test_illegal_mode_from_lookup);
    RUN_TEST(test_no_active_pid);
    RUN_TEST(test_golden_trace);

    printf("\nResults: %d/%d passed, %d failed\n",
           tests_passed, tests_run, tests_failed);

    return tests_failed > 0 ? 1 : 0;
}
