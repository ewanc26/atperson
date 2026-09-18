#ifndef ATPERSON_TESTS_SNAPSHOT_FILES_H
#define ATPERSON_TESTS_SNAPSHOT_FILES_H

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

/*
 * File helpers shared by the snapshot test binaries: read a whole file into
 * a malloc'd buffer (caller frees) and write bytes atomically enough for
 * tests. Both abort on failure; these are test atoms, not library code.
 */
static unsigned char *read_file(const char *path, size_t *size) {
    FILE *file = fopen(path, "rb");
    assert(file != NULL);
    assert(fseek(file, 0L, SEEK_END) == 0);
    const long length = ftell(file);
    assert(length > 0);
    assert(fseek(file, 0L, SEEK_SET) == 0);
    unsigned char *data = malloc((size_t)length);
    assert(data != NULL);
    assert(fread(data, 1u, (size_t)length, file) == (size_t)length);
    fclose(file);
    *size = (size_t)length;
    return data;
}

static void write_file(const char *path, const unsigned char *data, size_t size) {
    FILE *file = fopen(path, "wb");
    assert(file != NULL);
    assert(fwrite(data, 1u, size, file) == size);
    assert(fclose(file) == 0);
}

#endif
