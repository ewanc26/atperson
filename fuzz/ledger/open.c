/* Fuzz target: ledger open/recovery and record decoding (issue #11).
 *
 * The ledger is the durable authority for learned experience; its
 * recovery path replays an append-only log of records with lengths,
 * digests, and outcome patches. This target writes the fuzzer input to a
 * temp file and drives atp_ledger_open over it: malformed records,
 * truncated payloads, impossible lengths, and unexpected patches must be
 * rejected or safely recovered — never crash, leak, or corrupt the
 * in-memory index.
 *
 * A successfully opened ledger is exercised: entries are enumerated and
 * payloads read, so recovery-trusted values flow through the query and
 * payload-verification paths too. */

#include "atperson/core.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int LLVMFuzzerTestOneInput(const unsigned char *data, size_t size) {
    char path[] = "/tmp/atperson-fuzz-ledger-XXXXXX";
    int fd = mkstemp(path);
    if (fd < 0) {
        return 0;
    }
    FILE *file = fdopen(fd, "wb");
    if (!file) {
        close(fd);
        remove(path);
        return 0;
    }
    fwrite(data, 1u, size, file);
    fclose(file);

    atp_status status = ATP_OK;
    atp_ledger *ledger = atp_ledger_open(path, &status);
    remove(path);
    if (!ledger) {
        return 0;
    }

    /* Recovery-trusted state must be safe to enumerate. Payload reads
     * re-verify against the entry digest, so corrupted payload bytes
     * surface here rather than downstream. */
    const uint64_t count = atp_ledger_count(ledger);
    for (uint64_t i = 0u; i < count; ++i) {
        atp_ledger_entry entry;
        if (atp_ledger_entry_at(ledger, i, &entry) == ATP_OK) {
            size_t payload_len = 0u;
            (void)atp_ledger_entry_payload(ledger, entry.id, NULL, 0u, &payload_len);
            if (payload_len > 0u && payload_len <= 1024u * 1024u) {
                unsigned char *buffer = malloc(payload_len);
                if (buffer) {
                    (void)atp_ledger_entry_payload(ledger, entry.id, buffer, payload_len,
                                                   &payload_len);
                    free(buffer);
                }
            }
        }
    }

    atp_ledger_destroy(ledger);
    return 0;
}
