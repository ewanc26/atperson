/* Safe ledger compaction and checkpointing (issue #7).
 *
 * Phases (separate processes, mirroring the ledger/replay test layout):
 * - compact: outcome identity across compaction (withdrawn/skipped/
 *   pending included), dedup unchanged, payload retention rules, reopen,
 *   interrupted compaction recovery, rebuild equivalence.
 */
#include "atperson/core.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <direct.h>
#define atp_mkdir(path) _mkdir(path)
#define atp_rmdir(path) _rmdir(path)
#else
#include <sys/stat.h>
#include <unistd.h>
#define atp_mkdir(path) mkdir(path, 0755)
#define atp_rmdir(path) rmdir(path)
#endif

#define CHECK(condition)                                                                           \
    do {                                                                                           \
        if (!(condition)) {                                                                        \
            fprintf(stderr, "CHECK failed: %s (%s:%d)\n", #condition, __FILE__, __LINE__);         \
            return 1;                                                                              \
        }                                                                                          \
    } while (0)

static void remove_dir_files(const char *dir) {
    char path[1024];
    snprintf(path, sizeof(path), "%s/ledger.bin", dir);
    remove(path);
    snprintf(path, sizeof(path), "%s/ledger.bin.off", dir);
    remove(path);
    snprintf(path, sizeof(path), "%s/ledger.bin.tmp", dir);
    remove(path);
    snprintf(path, sizeof(path), "%s/ledger.bin.off.tmp", dir);
    remove(path);
    atp_rmdir(dir);
}

static atp_ledger *open_ledger(const char *dir) {
    char path[1024];
    snprintf(path, sizeof(path), "%s/ledger.bin", dir);
    atp_status status = ATP_OK;
    atp_ledger *ledger = atp_ledger_open(path, &status);
    if (!ledger || status != ATP_OK) {
        fprintf(stderr, "atp_ledger_open failed (status %d)\n", (int)status);
        exit(1);
    }
    return ledger;
}

static uint64_t append_observation(atp_ledger *ledger, const char *source, const char *author,
                                   const char *text, atp_ledger_outcome outcome) {
    uint64_t id = 0u;
    atp_status status = ATP_OK;
    const size_t text_len = strlen(text);
    const atp_ledger_result result =
        atp_ledger_append(ledger, source, author, 100u,
                          atp_ledger_digest(text, text_len), ATPERSON_SCHEMA_VERSION,
                          outcome, text, text_len, &id, &status);
    CHECK(result == ATP_LEDGER_NEW);
    CHECK(status == ATP_OK);
    return id;
}

/* Build the fixture ledger: 2 learned, 1 skipped, 1 pending, 1 failed,
 * 1 learned-then-withdrawn, 1 learned-then-withdrawn-with-payload.
 * Patches accumulate on entries 2 (skipped) and 6/7 (withdrawn). */
static int build_fixture(atp_ledger *ledger, uint64_t *ids) {
    ids[0] = append_observation(ledger, "at://c/1", "did:plc:a", "wolf moon",
                                ATP_LEDGER_OUTCOME_LEARNED);
    ids[1] = append_observation(ledger, "at://c/2", "did:plc:a", "silver fire",
                                ATP_LEDGER_OUTCOME_LEARNED);
    ids[2] = append_observation(ledger, "at://c/3", "did:plc:b", "quiet moor",
                                ATP_LEDGER_OUTCOME_LEARNED);
    ids[3] = append_observation(ledger, "at://c/4", "did:plc:b", "an image",
                                ATP_LEDGER_OUTCOME_PENDING);
    ids[4] = append_observation(ledger, "at://c/5", "did:plc:c", "still water",
                                ATP_LEDGER_OUTCOME_PENDING);
    ids[5] = append_observation(ledger, "at://c/6", "did:plc:c", "fading light",
                                ATP_LEDGER_OUTCOME_FAILED);
    ids[6] = append_observation(ledger, "at://c/7", "did:plc:a", "withdrawn prose",
                                ATP_LEDGER_OUTCOME_LEARNED);
    ids[7] = append_observation(ledger, "at://c/8", "did:plc:d", "withdrawn verse",
                                ATP_LEDGER_OUTCOME_LEARNED);
    /* Entry 4 closes from PENDING to SKIPPED: one patch, the normal
     * completion path sync uses. */
    CHECK(atp_ledger_set_outcome(ledger, ids[3], ATP_LEDGER_OUTCOME_SKIPPED) == ATP_OK);
    /* Entries 7 and 8 are withdrawn: two patches. */
    CHECK(atp_ledger_withdraw(ledger, ids[6]) == ATP_OK);
    CHECK(atp_ledger_withdraw(ledger, ids[7]) == ATP_OK);
    /* Withdraw idempotency: no new patch. */
    CHECK(atp_ledger_withdraw(ledger, ids[6]) == ATP_OK);
    return 0;
}

static int snapshot_outcomes(atp_ledger *ledger, uint64_t *outcomes, size_t count) {
    const uint64_t entries = atp_ledger_count(ledger);
    if (entries != (uint64_t)count) {
        return 1;
    }
    for (size_t i = 0u; i < count; ++i) {
        atp_ledger_entry entry = {0};
        if (atp_ledger_entry_at(ledger, i, &entry) != ATP_OK) {
            return 1;
        }
        outcomes[i] = (uint64_t)entry.outcome;
    }
    return 0;
}

static int run_compact(const char *dir) {
    remove_dir_files(dir);
    CHECK(atp_mkdir(dir) == 0);

    atp_ledger *ledger = open_ledger(dir);
    uint64_t ids[8] = {0};
    CHECK(build_fixture(ledger, ids) == 0);

    /* --- Outcomes and ids before compaction. --- */
    uint64_t before[8] = {0};
    CHECK(snapshot_outcomes(ledger, before, 8u) == 0);
    CHECK(before[0] == (uint64_t)ATP_LEDGER_OUTCOME_LEARNED);
    CHECK(before[3] == (uint64_t)ATP_LEDGER_OUTCOME_SKIPPED);
    CHECK(before[4] == (uint64_t)ATP_LEDGER_OUTCOME_PENDING);
    CHECK(before[5] == (uint64_t)ATP_LEDGER_OUTCOME_FAILED);
    CHECK(before[6] == (uint64_t)ATP_LEDGER_OUTCOME_WITHDRAWN);
    CHECK(before[7] == (uint64_t)ATP_LEDGER_OUTCOME_WITHDRAWN);

    /* --- Payloads before: all eight retained. --- */
    for (size_t i = 0u; i < 8u; ++i) {
        size_t len = 0u;
        CHECK(atp_ledger_entry_payload(ledger, ids[i], NULL, 0u, &len) == ATP_OK);
        CHECK(len > 0u);
    }

    /* --- Compaction report. --- */
    atp_compact_report report = {0};
    CHECK(atp_ledger_compact(ledger, &report) == ATP_OK);
    CHECK(report.entries == 8u);
    CHECK(report.patches_flattened == 3u);
    CHECK(report.payloads_dropped == 2u);
    CHECK(report.bytes_before > report.bytes_after);
    CHECK(report.bytes_after > 12u);

    /* --- Outcomes and ids identical after compaction. --- */
    uint64_t after[8] = {0};
    CHECK(snapshot_outcomes(ledger, after, 8u) == 0);
    for (size_t i = 0u; i < 8u; ++i) {
        CHECK(after[i] == before[i]);
        atp_ledger_entry entry = {0};
        CHECK(atp_ledger_entry_at(ledger, i, &entry) == ATP_OK);
        CHECK(entry.id == ids[i]); /* ids stable across generations */
    }

    /* --- Payload retention rules. --- */
    for (size_t i = 0u; i < 6u; ++i) {
        size_t len = 0u;
        CHECK(atp_ledger_entry_payload(ledger, ids[i], NULL, 0u, &len) == ATP_OK);
        CHECK(len > 0u); /* learned/skipped/pending/failed keep bytes */
    }
    for (size_t i = 6u; i < 8u; ++i) {
        size_t len = 1u;
        CHECK(atp_ledger_entry_payload(ledger, ids[i], NULL, 0u, &len) == ATP_OK);
        CHECK(len == 0u); /* withdrawn bytes dropped */
    }
    /* The retained payload still verifies against its digest. */
    char buffer[64] = {0};
    size_t len = 0u;
    CHECK(atp_ledger_entry_payload(ledger, ids[0], buffer, sizeof(buffer), &len) == ATP_OK);
    CHECK(len == 9u && memcmp(buffer, "wolf moon", 9u) == 0);

    /* --- Dedup unchanged: the same (source, digest) keys still hit. --- */
    uint64_t id = 0u;
    atp_status status = ATP_OK;
    CHECK(atp_ledger_append(ledger, "at://c/1", "did:plc:a", 100u,
                            atp_ledger_digest("wolf moon", 9u), ATPERSON_SCHEMA_VERSION,
                            ATP_LEDGER_OUTCOME_LEARNED, "wolf moon", 9u, &id,
                            &status) == ATP_LEDGER_EXISTS_COMMITTED);
    CHECK(id == ids[0]);
    /* Withdrawn content is not re-learned: the tombstone still dedups. */
    CHECK(atp_ledger_append(ledger, "at://c/7", "did:plc:a", 100u,
                            atp_ledger_digest("withdrawn prose", 15u), ATPERSON_SCHEMA_VERSION,
                            ATP_LEDGER_OUTCOME_LEARNED, "withdrawn prose", 15u, &id,
                            &status) == ATP_LEDGER_EXISTS_COMMITTED);
    CHECK(id == ids[6]);
    /* New content appends normally after compaction. */
    CHECK(atp_ledger_append(ledger, "at://c/9", "did:plc:a", 100u,
                            atp_ledger_digest("new moon", 8u), ATPERSON_SCHEMA_VERSION,
                            ATP_LEDGER_OUTCOME_LEARNED, "new moon", 8u, &id,
                            &status) == ATP_LEDGER_NEW);
    CHECK(id == 9u);
    atp_ledger_destroy(ledger);

    /* --- Reopen: the compacted generation loads with the same state. --- */
    ledger = open_ledger(dir);
    uint64_t reopened[9] = {0};
    CHECK(snapshot_outcomes(ledger, reopened, 9u) == 0);
    for (size_t i = 0u; i < 8u; ++i) {
        CHECK(reopened[i] == before[i]);
    }
    CHECK(reopened[8] == (uint64_t)ATP_LEDGER_OUTCOME_LEARNED);
    atp_ledger_destroy(ledger);

    /* --- Interrupted compaction, crash before the swap: a staging file
     * is present, the original log is untouched. Open heals. --- */
    remove_dir_files(dir);
    CHECK(atp_mkdir(dir) == 0);
    ledger = open_ledger(dir);
    CHECK(build_fixture(ledger, ids) == 0);
    atp_ledger_destroy(ledger);

    /* Simulate the crash window: a complete staging file exists but the
     * rename never happened. Open discards the staging file and continues
     * from the original generation. */
    {
        char tmp_path[1024];
        snprintf(tmp_path, sizeof(tmp_path), "%s/ledger.bin.tmp", dir);
        FILE *tmp = fopen(tmp_path, "wb");
        CHECK(tmp != NULL);
        fputs("garbage from a torn compaction", tmp);
        fclose(tmp);
    }
    ledger = open_ledger(dir);
    uint64_t healed[8] = {0};
    CHECK(snapshot_outcomes(ledger, healed, 8u) == 0);
    for (size_t i = 0u; i < 8u; ++i) {
        CHECK(healed[i] == before[i]);
    }
    /* And the healed ledger still compacts cleanly. */
    atp_compact_report healed_report = {0};
    CHECK(atp_ledger_compact(ledger, &healed_report) == ATP_OK);
    CHECK(healed_report.entries == 8u);
    atp_ledger_destroy(ledger);

    /* --- Interrupted compaction, crash between marker removal and
     * rename: no marker, original log present. Open self-heals the
     * marker from the log's longest valid prefix. --- */
    remove_dir_files(dir);
    CHECK(atp_mkdir(dir) == 0);
    ledger = open_ledger(dir);
    CHECK(build_fixture(ledger, ids) == 0);
    atp_ledger_destroy(ledger);

    {
        char off_path[1024];
        snprintf(off_path, sizeof(off_path), "%s/ledger.bin.off", dir);
        CHECK(remove(off_path) == 0);
    }
    ledger = open_ledger(dir);
    uint64_t markerless[8] = {0};
    CHECK(snapshot_outcomes(ledger, markerless, 8u) == 0);
    for (size_t i = 0u; i < 8u; ++i) {
        CHECK(markerless[i] == before[i]);
    }
    atp_ledger_destroy(ledger);

    /* --- Empty ledger compaction: valid no-op generation. --- */
    remove_dir_files(dir);
    CHECK(atp_mkdir(dir) == 0);
    ledger = open_ledger(dir);
    atp_compact_report empty_report = {0};
    CHECK(atp_ledger_compact(ledger, &empty_report) == ATP_OK);
    CHECK(empty_report.entries == 0u);
    CHECK(empty_report.patches_flattened == 0u);
    CHECK(empty_report.payloads_dropped == 0u);
    CHECK(atp_ledger_count(ledger) == 0u);
    atp_ledger_destroy(ledger);
    /* Reopen the compacted empty ledger and append normally. */
    ledger = open_ledger(dir);
    const uint64_t first = append_observation(ledger, "at://c/1", "did:plc:a", "first",
                                              ATP_LEDGER_OUTCOME_LEARNED);
    CHECK(first == 1u);
    atp_ledger_destroy(ledger);

    remove_dir_files(dir);
    printf("compact: ok\n");
    return 0;
}

/* Rebuild equivalence: replaying the fixture ledger before and after
 * compaction produces byte-identical snapshots. Withdrawn and pending
 * entries are excluded identically; the learned payload bytes are the
 * same bytes, so the rebuilt graph must be the same graph. */
static int run_rebuild_equivalence(const char *dir) {
    remove_dir_files(dir);
    CHECK(atp_mkdir(dir) == 0);

    atp_ledger *ledger = open_ledger(dir);
    uint64_t ids[8] = {0};
    CHECK(build_fixture(ledger, ids) == 0);

    atp_graph_config config = atp_graph_default_config();
    atp_graph *first = atp_graph_create(&config);
    atp_replay_report report = {0};
    CHECK(atp_replay_ledger(ledger, first, &report) == ATP_OK);
    CHECK(report.replayed == 3u); /* learned entries only */

    char before_path[1024];
    char after_path[1024];
    snprintf(before_path, sizeof(before_path), "%s/before.bin", dir);
    snprintf(after_path, sizeof(after_path), "%s/after.bin", dir);
    CHECK(atp_graph_save(first, before_path) == ATP_OK);
    atp_graph_destroy(first);

    atp_compact_report compact_report = {0};
    CHECK(atp_ledger_compact(ledger, &compact_report) == ATP_OK);
    CHECK(compact_report.payloads_dropped == 2u);

    atp_graph *second = atp_graph_create(&config);
    report = (atp_replay_report){0};
    CHECK(atp_replay_ledger(ledger, second, &report) == ATP_OK);
    CHECK(report.replayed == 3u);
    CHECK(atp_graph_save(second, after_path) == ATP_OK);
    atp_graph_destroy(second);
    atp_ledger_destroy(ledger);

    /* Byte-identical snapshots: the compacted ledger reproduces the same
     * learned state from the same bytes. */
    FILE *a = fopen(before_path, "rb");
    FILE *b = fopen(after_path, "rb");
    CHECK(a != NULL && b != NULL);
    int equal = 1;
    for (;;) {
        unsigned char ca;
        unsigned char cb;
        const size_t ra = fread(&ca, 1u, 1u, a);
        const size_t rb = fread(&cb, 1u, 1u, b);
        if (ra != rb || (ra == 0u && feof(a) != feof(b))) {
            equal = 0;
            break;
        }
        if (ra == 0u) {
            break;
        }
        if (ca != cb) {
            equal = 0;
            break;
        }
    }
    fclose(a);
    fclose(b);
    CHECK(equal);

    remove(before_path);
    remove(after_path);
    remove_dir_files(dir);
    printf("rebuild-equivalence: ok\n");
    return 0;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s <compact|rebuild-equivalence> [dir]\n", argv[0]);
        return 2;
    }
    const char *dir = argc > 2 ? argv[2] : "/tmp/atperson-compact";
    if (strcmp(argv[1], "compact") == 0) {
        return run_compact(dir);
    }
    if (strcmp(argv[1], "rebuild-equivalence") == 0) {
        return run_rebuild_equivalence(dir);
    }
    fprintf(stderr, "unknown phase %s\n", argv[1]);
    return 2;
}
