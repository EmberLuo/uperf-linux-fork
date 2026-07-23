#ifndef UPERF_TEST_GOLDEN_TRACE_H
#define UPERF_TEST_GOLDEN_TRACE_H

/* Compare an emitted deterministic behavior trace with a checked-in golden
 * file.  Returns 0 on an exact byte-for-byte match. */
int golden_trace_matches(const char *golden_path, const char *actual);

#endif
