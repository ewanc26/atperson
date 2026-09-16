/*
 * Observation ledger durable commit marker and file primitives.
 *
 *   off header : magic(8) | version u32 LE | count u64 LE | offset u64 LE
 *                -> 28 bytes
 *
 * Crash-ordering contract: record bytes are written and fsync'd to the log
 * first, and only then is the commit marker staged in a temporary file,
 * fsync'd, and renamed into place. A crash at any point leaves the previous
 * committed prefix intact; recovery (ledger_recover.c) truncates a torn tail
 * beyond the committed offset and heals a missing marker from the valid log
 * prefix.
 */

#include "ledger_internal.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#if defined(_WIN32)
#include <io.h>
#else
#include <unistd.h>
#endif

bool atp_fsync(FILE *file) {
    if (fflush(file) != 0) {
        return false;
    }
#if defined(_WIN32)
    return _commit(_fileno(file)) == 0;
#else
    return fsync(fileno(file)) == 0;
#endif
}

bool atp_truncate_file(FILE *file, uint64_t size) {
#if defined(_WIN32)
    if (_chsize_s(_fileno(file), (__int64)size) != 0) {
        return false;
    }
#else
    if (ftruncate(fileno(file), (off_t)size) != 0) {
        return false;
    }
#endif
    return fseek(file, 0, SEEK_END) == 0;
}

bool atp_read_file(FILE *file, void *data, size_t size) {
    return fread(data, 1u, size, file) == size;
}

bool atp_write_off(atp_ledger *ledger) {
    FILE *file = fopen(ledger->off_tmp_path, "wb");
    if (!file) {
        return false;
    }
    unsigned char header[ATP_LEDGER_OFF_HEADER_SIZE];
    header[0] = ATP_LEDGER_OFF_MAGIC_0;
    header[1] = ATP_LEDGER_OFF_MAGIC_1;
    header[2] = ATP_LEDGER_OFF_MAGIC_2;
    header[3] = ATP_LEDGER_OFF_MAGIC_3;
    header[4] = ATP_LEDGER_OFF_MAGIC_4;
    header[5] = ATP_LEDGER_OFF_MAGIC_5;
    header[6] = ATP_LEDGER_OFF_MAGIC_6;
    header[7] = ATP_LEDGER_OFF_MAGIC_7;
    atp_store_u32_le(&header[8], ATPERSON_LEDGER_VERSION);
    atp_store_u64_le(&header[12], (uint64_t)ledger->count);
    atp_store_u64_le(&header[20], ledger->committed_offset);

    bool ok = fwrite(header, 1u, sizeof(header), file) == sizeof(header);
    if (!atp_fsync(file)) {
        ok = false;
    }
    if (fclose(file) != 0) {
        ok = false;
    }
    if (!ok) {
        remove(ledger->off_tmp_path);
        return false;
    }
    if (rename(ledger->off_tmp_path, ledger->off_path) != 0) {
        remove(ledger->off_tmp_path);
        return false;
    }
    return true;
}

bool atp_read_off(atp_ledger *ledger, uint64_t *out_count, uint64_t *out_offset) {
    FILE *file = fopen(ledger->off_path, "rb");
    if (!file) {
        return false;
    }
    unsigned char header[ATP_LEDGER_OFF_HEADER_SIZE];
    const bool ok = fread(header, 1u, sizeof(header), file) == sizeof(header) &&
                    header[0] == ATP_LEDGER_OFF_MAGIC_0 && header[1] == ATP_LEDGER_OFF_MAGIC_1 &&
                    header[2] == ATP_LEDGER_OFF_MAGIC_2 && header[3] == ATP_LEDGER_OFF_MAGIC_3 &&
                    header[4] == ATP_LEDGER_OFF_MAGIC_4 && header[5] == ATP_LEDGER_OFF_MAGIC_5 &&
                    header[6] == ATP_LEDGER_OFF_MAGIC_6 && header[7] == ATP_LEDGER_OFF_MAGIC_7 &&
                    atp_load_u32_le(&header[8]) == ATPERSON_LEDGER_VERSION;
    fclose(file);
    if (!ok) {
        return false;
    }
    *out_count = atp_load_u64_le(&header[12]);
    *out_offset = atp_load_u64_le(&header[20]);
    return true;
}
