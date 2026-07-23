#define _POSIX_C_SOURCE 200809L
#include "../src/include/sysfs_writer.h"
#include "../src/include/log.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>

/* Fault-injection recovery tests for the sysfs writer.
 *
 * The writer takes absolute paths, so instead of an injectable root we drive it
 * against real kernel/filesystem objects that fail deterministically:
 *   - /dev/full           open() succeeds, write() fails with ENOSPC
 *   - a missing path       open() fails with ENOENT
 *   - a read-only 0400 file  open(O_WRONLY) fails with EACCES (when not root)
 *
 * The property under test is RECOVERY: a failed write must be reported (for the
 * immediate path) and must NOT poison the writer — later good writes still land
 * and the queue is left empty. */

static int tests_run = 0;
static int tests_passed = 0;
static int tests_failed = 0;

#define RUN_TEST(name) do { \
    tests_run++; \
    printf("  TEST: %s ... ", #name); \
    name(); \
} while(0)
#define FAILT(msg) do { printf("FAIL (%s)\n", msg); tests_failed++; return; } while(0)
#define PASST(msg) do { printf("PASS\n"); tests_passed++; (void)(msg); } while(0)

static SysfsWriter *make_writer(void) {
    Config cfg;
    memset(&cfg, 0, sizeof(cfg));
    return sysfs_writer_new(&cfg);
}

static ssize_t short_write(int fd, const void *buffer, size_t length,
                           void *context) {
    (void)context;
    if (length == 0) return 0;
    return write(fd, buffer, length - 1);
}

/* Immediate write to /dev/full must report ENOSPC as failure, not success. */
static void test_immediate_enospc_reported(void) {
    if (access("/dev/full", W_OK) != 0) { printf("SKIP (/dev/full)\n"); return; }
    SysfsWriter *w = make_writer();
    if (!w) FAILT("writer creation");
    int ret = sysfs_writer_write_raw(w, "/dev/full", "1");
    sysfs_writer_free(w);
    if (ret == 0) FAILT("write to /dev/full should fail (ENOSPC)");
    PASST("ENOSPC surfaced from immediate write");
}

/* Immediate write to a missing path must report ENOENT as failure. */
static void test_immediate_enoent_reported(void) {
    SysfsWriter *w = make_writer();
    if (!w) FAILT("writer creation");
    int ret = sysfs_writer_write_raw(w, "/sys/uperf/does/not/exist", "1");
    sysfs_writer_free(w);
    if (ret == 0) FAILT("write to missing path should fail (ENOENT)");
    PASST("ENOENT surfaced from immediate write");
}

/* Immediate write to a read-only file must report EACCES (skipped as root). */
static void test_immediate_eacces_reported(void) {
    if (geteuid() == 0) { printf("SKIP (running as root)\n"); return; }
    char path[] = "/tmp/uperf-ro-XXXXXX";
    int fd = mkstemp(path);
    if (fd < 0) FAILT("mkstemp");
    close(fd);
    if (chmod(path, 0400) != 0) { unlink(path); FAILT("chmod 0400"); }

    SysfsWriter *w = make_writer();
    if (!w) { unlink(path); FAILT("writer creation"); }
    int ret = sysfs_writer_write_raw(w, path, "1");
    sysfs_writer_free(w);
    unlink(path);
    if (ret == 0) FAILT("write to 0400 file should fail (EACCES)");
    PASST("EACCES surfaced from immediate write");
}

/* A failing queued write must not abort the batch: a good write queued after a
 * bad one still lands, and the queue is drained. */
static void test_flush_recovers_after_bad_entry(void) {
    char good[] = "/tmp/uperf-good-XXXXXX";
    int fd = mkstemp(good);
    if (fd < 0) FAILT("mkstemp");
    close(fd);

    SysfsWriter *w = make_writer();
    if (!w) { unlink(good); FAILT("writer creation"); }

    /* bad first, good second */
    sysfs_writer_queue_raw(w, "/sys/uperf/does/not/exist", "X");
    sysfs_writer_queue_raw(w, good, "RECOVERED");
    int flush_rc = sysfs_writer_flush(w);

    /* A second, independent good write after the recovered flush. */
    char good2[] = "/tmp/uperf-good2-XXXXXX";
    int ok = 1;
    int fd2 = mkstemp(good2);
    if (fd2 >= 0) {
        close(fd2);
        sysfs_writer_queue_raw(w, good2, "AGAIN");
        if (sysfs_writer_flush(w) != 0) ok = 0;
    }
    sysfs_writer_free(w);

    char *v = sysfs_reader_read(good);
    ok = ok && v && strcmp(v, "RECOVERED") == 0;
    free(v);
    if (fd2 >= 0) {
        char *v2 = sysfs_reader_read(good2);
        ok = ok && v2 && strcmp(v2, "AGAIN") == 0;
        free(v2);
        unlink(good2);
    }
    unlink(good);
    if (flush_rc == 0) FAILT("queued failure must be returned by flush");
    if (!ok) FAILT("good writes must survive a bad queue entry");
    PASST("flush recovers past a failing entry");
}

/* Interleave /dev/full (ENOSPC) between two good writes in one batch. */
static void test_flush_recovers_after_enospc(void) {
    if (access("/dev/full", W_OK) != 0) { printf("SKIP (/dev/full)\n"); return; }
    char good[] = "/tmp/uperf-full-XXXXXX";
    int fd = mkstemp(good);
    if (fd < 0) FAILT("mkstemp");
    close(fd);

    SysfsWriter *w = make_writer();
    if (!w) { unlink(good); FAILT("writer creation"); }

    sysfs_writer_queue_raw(w, "/dev/full", "1");     /* ENOSPC on write */
    sysfs_writer_queue_raw(w, good, "AFTER_FULL");   /* must still apply */
    int flush_rc = sysfs_writer_flush(w);
    sysfs_writer_free(w);

    char *v = sysfs_reader_read(good);
    int ok = v && strcmp(v, "AFTER_FULL") == 0;
    free(v);
    unlink(good);
    if (flush_rc == 0) FAILT("ENOSPC must be returned by flush");
    if (!ok) FAILT("write after ENOSPC entry must still land");
    PASST("flush recovers past an ENOSPC entry");
}

/* Inject a deterministic short write into the real open/close path. */
static void test_short_write_reported(void) {
    char path[] = "/tmp/uperf-len-XXXXXX";
    int fd = mkstemp(path);
    if (fd < 0) FAILT("mkstemp");
    close(fd);
    Config cfg = {0};
    SysfsWriterIo io = {.write_fn = short_write, .context = NULL};
    SysfsWriter *w = sysfs_writer_new_with_io(&cfg, &io);
    if (!w) { unlink(path); FAILT("writer creation"); }
    const char *val = "0123456789abcdef0123456789abcdef";
    int ret = sysfs_writer_write_raw(w, path, val);
    sysfs_writer_free(w);
    unlink(path);
    if (ret == 0) FAILT("short write must be reported as failure");
    PASST("injected short write is reported");
}

/* NULL/empty guards must fail cleanly, never crash. */
static void test_null_guards(void) {
    SysfsWriter *w = make_writer();
    if (!w) FAILT("writer creation");
    int a = sysfs_writer_write_raw(w, NULL, "1");
    int b = sysfs_writer_write_raw(w, "/tmp/x", NULL);
    int c = sysfs_writer_write_raw(NULL, "/tmp/x", "1");
    int d = sysfs_writer_queue_raw(w, NULL, "1");
    sysfs_writer_free(w);
    if (a == 0 || b == 0 || c == 0 || d == 0)
        FAILT("NULL arguments must be rejected");
    PASST("NULL arguments rejected without crashing");
}

int main(void) {
    printf("=== sysfs writer fault-injection recovery tests ===\n");
    log_init(UPERF_LOG_FATAL, 0, NULL);

    RUN_TEST(test_immediate_enospc_reported);
    RUN_TEST(test_immediate_enoent_reported);
    RUN_TEST(test_immediate_eacces_reported);
    RUN_TEST(test_flush_recovers_after_bad_entry);
    RUN_TEST(test_flush_recovers_after_enospc);
    RUN_TEST(test_short_write_reported);
    RUN_TEST(test_null_guards);

    printf("\nResults: %d/%d passed (%d failed)\n",
           tests_passed, tests_run, tests_failed);
    log_shutdown();
    return tests_failed > 0 ? 1 : 0;
}
