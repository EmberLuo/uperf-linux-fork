#define _POSIX_C_SOURCE 200809L
#include "golden_trace.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int golden_trace_matches(const char *golden_path, const char *actual) {
    if (!golden_path || !actual) return -1;
    FILE *file = fopen(golden_path, "rb");
    if (!file) return -1;
    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        return -1;
    }
    long length = ftell(file);
    if (length < 0 || fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        return -1;
    }
    char *expected = malloc((size_t)length + 1);
    if (!expected) {
        fclose(file);
        return -1;
    }
    size_t read_length = fread(expected, 1, (size_t)length, file);
    int failed = ferror(file);
    fclose(file);
    expected[read_length] = '\0';
    int result = !failed && read_length == (size_t)length &&
        strcmp(expected, actual) == 0 ? 0 : -1;
    free(expected);
    return result;
}
