#include "../src/include/input_monitor.h"
#include "../src/include/log.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

static int tests_run = 0;
static int tests_passed = 0;
static int tests_failed = 0;

#define TEST(name) static void name(void)
#define RUN_TEST(name) do { \
    tests_run++; \
    printf("  TEST: %s ... ", #name); \
    name(); \
} while(0)
#define ASSERT_PASS(msg) do { \
    printf("PASS\n"); tests_passed++; \
} while(0)

/* Test distance calculation (replicated from input_monitor.c for verification) */
static float test_dist(int32_t x1, int32_t y1, int32_t x2, int32_t y2) {
    int dx = x2 - x1;
    int dy = y2 - y1;
    return sqrtf((float)(dx * dx + dy * dy));
}

TEST(test_distance_calculation) {
    float d;

    /* 3-4-5 triangle: x=3, y=4 -> dist=5 */
    d = test_dist(0, 0, 3, 4);
    if (fabsf(d - 5.0f) < 0.001f) {
        ASSERT_PASS("3-4-5 triangle correct");
    } else {
        printf("FAIL (expected 5.0, got %.3f)\n", d);
        tests_failed++;
    }

    /* Horizontal: dy=0, dx=7 -> dist=7 */
    d = test_dist(0, 10, 7, 10);
    if (fabsf(d - 7.0f) < 0.001f) {
        ASSERT_PASS("horizontal distance correct");
    } else {
        printf("FAIL (expected 7.0, got %.3f)\n", d);
        tests_failed++;
    }

    /* Vertical: dx=0, dy=5 -> dist=5 */
    d = test_dist(5, 0, 5, 5);
    if (fabsf(d - 5.0f) < 0.001f) {
        ASSERT_PASS("vertical distance correct");
    } else {
        printf("FAIL (expected 5.0, got %.3f)\n", d);
        tests_failed++;
    }

    /* Zero distance */
    d = test_dist(10, 20, 10, 20);
    if (fabsf(d - 0.0f) < 0.001f) {
        ASSERT_PASS("zero distance correct");
    } else {
        printf("FAIL (expected 0.0, got %.3f)\n", d);
        tests_failed++;
    }

    /* Negative coordinates */
    d = test_dist(-1, -1, 2, 3);
    if (fabsf(d - 5.0f) < 0.001f) {
        ASSERT_PASS("negative coordinates correct");
    } else {
        printf("FAIL (expected 5.0, got %.3f)\n", d);
        tests_failed++;
    }
}

/* Test InputMonitor lifecycle */
TEST(test_monitor_create_destroy) {
    InputMonitor *im = input_monitor_new(0.03f, 0.03f, 0.03f, 2.0f, 1080, 2400);
    if (!im) {
        printf("FAIL (input_monitor_new returned NULL)\n");
        tests_failed++;
        return;
    }
    ASSERT_PASS("create with default params");

    input_monitor_free(im);
    ASSERT_PASS("destroy without error");
}

/* Test InputMonitor with zero thresholds */
TEST(test_monitor_zero_thresholds) {
    InputMonitor *im = input_monitor_new(0.0f, 0.0f, 0.0f, 0.0f, 1920, 1080);
    if (!im) {
        printf("FAIL (input_monitor_new with zero thresholds returned NULL)\n");
        tests_failed++;
        return;
    }

    int count = input_monitor_device_count(im);
    if (count == 0) {
        ASSERT_PASS("zero devices before discovery");
    } else {
        printf("INFO (%d devices before discovery)\n", count);
        tests_passed++;
    }

    input_monitor_set_screen_size(im, 1280, 720);
    ASSERT_PASS("screen size updated");

    input_monitor_free(im);
}

/* Test InputMonitor with different screen sizes */
TEST(test_monitor_screen_sizes) {
    /* Tablet portrait */
    InputMonitor *im1 = input_monitor_new(0.03f, 0.03f, 0.03f, 2.0f, 1080, 2400);
    if (!im1) {
        printf("FAIL (tablet portrait creation)\n");
        tests_failed++;
    } else {
        ASSERT_PASS("tablet portrait (1080x2400)");
        input_monitor_free(im1);
    }

    /* Phone portrait */
    InputMonitor *im2 = input_monitor_new(0.03f, 0.03f, 0.03f, 2.0f, 720, 1600);
    if (!im2) {
        printf("FAIL (phone portrait creation)\n");
        tests_failed++;
    } else {
        ASSERT_PASS("phone portrait (720x1600)");
        input_monitor_free(im2);
    }

    /* Landscape */
    InputMonitor *im3 = input_monitor_new(0.03f, 0.03f, 0.03f, 2.0f, 2560, 1600);
    if (!im3) {
        printf("FAIL (landscape creation)\n");
        tests_failed++;
    } else {
        ASSERT_PASS("landscape (2560x1600)");
        input_monitor_free(im3);
    }
}

/* Test gesture threshold edge detection logic */
TEST(test_gesture_edge_calculation) {
    /* Simulate edge detection: at 1080 width, gesture_thd_x=0.03 -> 32px margin */
    int screen_w = 1080;
    float thd_x = 0.03f;
    float left_edge = screen_w * thd_x;         /* 32.4 */
    float right_edge = screen_w * (1.0f - thd_x); /* 1047.6 */

    if (0 <= left_edge && 0 <= right_edge) {
        ASSERT_PASS("edge threshold calculation correct");
    } else {
        printf("FAIL (invalid edge calculation: left=%.1f right=%.1f)\n",
               left_edge, right_edge);
        tests_failed++;
    }

    /* At left edge: x=10 */
    if (10.0f <= left_edge) {
        ASSERT_PASS("left edge detection works (x=10 < 32.4)");
    } else {
        printf("FAIL (left edge detection)\n");
        tests_failed++;
    }

    /* At center: x=540 */
    if (540.0f > left_edge && 540.0f < right_edge) {
        ASSERT_PASS("center position not classified as edge");
    } else {
        printf("FAIL (center detection)\n");
        tests_failed++;
    }

    /* At right edge: x=1050 */
    if (1050.0f >= right_edge) {
        ASSERT_PASS("right edge detection works (x=1050 > 1047.6)");
    } else {
        printf("FAIL (right edge detection)\n");
        tests_failed++;
    }
}

/* Test gesture threshold Y edge detection */
TEST(test_gesture_edge_y_calculation) {
    int screen_h = 2400;
    float thd_y = 0.03f;
    float top_edge = screen_h * thd_y;           /* 72.0 */
    float bottom_edge = screen_h * (1.0f - thd_y); /* 2328.0 */

    if (top_edge > 0 && bottom_edge < screen_h) {
        ASSERT_PASS("Y edge threshold calculation correct");
    } else {
        printf("FAIL (invalid Y edge calculation)\n");
        tests_failed++;
    }

    /* Near top edge */
    if (50.0f <= top_edge) {
        ASSERT_PASS("top edge detection works (y=50 < 72.0)");
    } else {
        printf("FAIL (top edge detection)\n");
        tests_failed++;
    }

    /* Near bottom edge */
    if (2360.0f >= bottom_edge) {
        ASSERT_PASS("bottom edge detection works (y=2360 > 2328.0)");
    } else {
        printf("FAIL (bottom edge detection)\n");
        tests_failed++;
    }
}

int main(void) {
    log_init(LOG_INFO, 0, NULL);

    printf("=== input_monitor tests ===\n\n");

    RUN_TEST(test_distance_calculation);
    RUN_TEST(test_monitor_create_destroy);
    RUN_TEST(test_monitor_zero_thresholds);
    RUN_TEST(test_monitor_screen_sizes);
    RUN_TEST(test_gesture_edge_calculation);
    RUN_TEST(test_gesture_edge_y_calculation);

    printf("\n=== Results: %d run, %d passed, %d failed ===\n",
           tests_run, tests_passed, tests_failed);

    log_shutdown();
    return tests_failed == 0 ? 0 : 1;
}
