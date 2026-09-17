/*
 * Ledger durable commit primitive.
 *
 * Appends one already-framed record, fsyncs the log, then publishes the
 * commit marker. The previous marker remains authoritative on failure.
 */

#include "internal.h"

#include <stdio.h>

atp_status atp_ledger_commit_record(atp_ledger *ledger, const unsigned char *record,
                                    size_t record_len) {
    if (fseek(ledger->log, 0, SEEK_END) != 0) {
        return ATP_ERR_IO;
    }
    if (fwrite(record, 1u, record_len, ledger->log) != record_len) {
        return ATP_ERR_IO;
    }
    if (!atp_fsync(ledger->log)) {
        return ATP_ERR_IO;
    }
    ledger->committed_offset += record_len;
    if (!atp_write_off(ledger)) {
        return ATP_ERR_IO;
    }
    return ATP_OK;
}
