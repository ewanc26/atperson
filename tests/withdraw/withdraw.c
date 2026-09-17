/* Source withdrawal, deletion, and unlearning semantics (issue #4).
 *
 * Phases (separate processes, mirroring the ledger/replay test layout):
 * - withdraw: withdraw-by-id/source/author, idempotency, restart
 *   persistence, withdrawal of a live graph's entries followed by rebuild
 *   excluding them, edited-source behaviour (same URI, new digest), and
 *   rebuild determinism with withdrawals present.
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

static int run_withdraw(const char *dir) {
    remove_dir_files(dir);
    CHECK(atp_mkdir(dir) == 0);

    atp_ledger *ledger = open_ledger(dir);
    const uint64_t a = append_observation(ledger, "at://w/1", "did:plc:a", "wolf moon",
                                          ATP_LEDGER_OUTCOME_LEARNED);
    const uint64_t b = append_observation(ledger, "at://w/2", "did:plc:a", "silver fire",
                                          ATP_LEDGER_OUTCOME_LEARNED);
    const uint64_t c = append_observation(ledger, "at://w/3", "did:plc:b", "quiet moor",
                                          ATP_LEDGER_OUTCOME_LEARNED);
    const uint64_t d = append_observation(ledger, "at://w/4", "did:plc:b", "an image",
                                          ATP_LEDGER_OUTCOME_SKIPPED);
    CHECK(a == 1u && b == 2u && c == 3u && d == 4u);

    /* --- Withdraw by id: single entry, siblings untouched. --- */
    CHECK(atp_ledger_withdraw(ledger, b) == ATP_OK);
    atp_ledger_entry entry = {0};
    CHECK(atp_ledger_entry_at(ledger, 1u, &entry) == ATP_OK);
    CHECK(entry.outcome == ATP_LEDGER_OUTCOME_WITHDRAWN);
    CHECK(atp_ledger_entry_at(ledger, 0u, &entry) == ATP_OK);
    CHECK(entry.outcome == ATP_LEDGER_OUTCOME_LEARNED);

    /* --- Idempotency: withdrawing again is a no-op, not an error. --- */
    CHECK(atp_ledger_withdraw(ledger, b) == ATP_OK);
    CHECK(atp_ledger_entry_at(ledger, 1u, &entry) == ATP_OK);
    CHECK(entry.outcome == ATP_LEDGER_OUTCOME_WITHDRAWN);

    /* --- A withdrawn entry cannot regress to PENDING. --- */
    CHECK(atp_ledger_set_outcome(ledger, b, ATP_LEDGER_OUTCOME_PENDING) ==
          ATP_ERR_INVALID_ARGUMENT);

    /* --- Withdraw by source: covers a deleted AT record. --- */
    CHECK(atp_ledger_withdraw_source(ledger, "at://w/3") == 1u);
    CHECK(atp_ledger_entry_at(ledger, 2u, &entry) == ATP_OK);
    CHECK(entry.outcome == ATP_LEDGER_OUTCOME_WITHDRAWN);
    /* Re-withdrawing the same source withdraws nothing new. */
    CHECK(atp_ledger_withdraw_source(ledger, "at://w/3") == 0u);
    /* A missing source withdraws nothing. */
    CHECK(atp_ledger_withdraw_source(ledger, "at://nope") == 0u);

    /* --- Withdraw by author: excludes an account entirely. --- */
    /* did:plc:a has entries 1 (LEARNED) and 2 (already WITHDRAWN). */
    CHECK(atp_ledger_withdraw_author(ledger, "did:plc:a") == 1u);
    CHECK(atp_ledger_entry_at(ledger, 0u, &entry) == ATP_OK);
    CHECK(entry.outcome == ATP_LEDGER_OUTCOME_WITHDRAWN);
    CHECK(atp_ledger_withdraw_author(ledger, "did:plc:a") == 0u);

    /* --- Withdrawal blocks re-append: the observation is history. --- */
    uint64_t id = 0u;
    atp_status status = ATP_OK;
    CHECK(atp_ledger_append(ledger, "at://w/1", "did:plc:a", 100u,
                            atp_ledger_digest("wolf moon", 9u), ATPERSON_SCHEMA_VERSION,
                            ATP_LEDGER_OUTCOME_LEARNED, "wolf moon", 9u, &id,
                            &status) == ATP_LEDGER_EXISTS_COMMITTED);

    /* --- Edited record: same URI, new content appends fresh. --- */
    const uint64_t edited = append_observation(ledger, "at://w/1", "did:plc:a", "wolf moon risen",
                                               ATP_LEDGER_OUTCOME_LEARNED);
    CHECK(edited == 5u);

    /* --- Restart: withdrawal patches survive reopen. --- */
    atp_ledger_destroy(ledger);
    ledger = open_ledger(dir);
    CHECK(atp_ledger_entry_at(ledger, 0u, &entry) == ATP_OK);
    CHECK(entry.outcome == ATP_LEDGER_OUTCOME_WITHDRAWN);
    CHECK(atp_ledger_entry_at(ledger, 1u, &entry) == ATP_OK);
    CHECK(entry.outcome == ATP_LEDGER_OUTCOME_WITHDRAWN);
    CHECK(atp_ledger_entry_at(ledger, 2u, &entry) == ATP_OK);
    CHECK(entry.outcome == ATP_LEDGER_OUTCOME_WITHDRAWN);
    CHECK(atp_ledger_entry_at(ledger, 3u, &entry) == ATP_OK);
    CHECK(entry.outcome == ATP_LEDGER_OUTCOME_SKIPPED);
    CHECK(atp_ledger_entry_at(ledger, 4u, &entry) == ATP_OK);
    CHECK(entry.outcome == ATP_LEDGER_OUTCOME_LEARNED);

    /* --- Rebuild excludes withdrawn contributions entirely. --- */
    atp_graph_config config = atp_graph_default_config();
    atp_graph *rebuilt = atp_graph_create(&config);
    atp_replay_report report = {0};
    CHECK(atp_replay_ledger(ledger, rebuilt, &report) == ATP_OK);
    /* Only entry 5 (the edited content) trains; entry 4 is SKIPPED-mirrored. */
    CHECK(report.replayed == 1u);
    CHECK(report.mirrored == 1u);
    CHECK(report.excluded_withdrawn == 3u);
    CHECK(report.excluded_pending == 0u);
    CHECK(report.excluded_failed == 0u);
    const atp_graph_stats stats = atp_graph_get_stats(rebuilt);
    CHECK(stats.observations == 1u);
    /* The mirror holds the edited entry plus the SKIPPED one; the three
     * withdrawn entries are excluded from the mirror too. */
    CHECK(atp_graph_ledger_count(rebuilt) == 2u);
    CHECK(atp_graph_familiarity(rebuilt, "moon") > 0.0f);
    /* The withdrawn text's distinctive token never entered the graph. */
    CHECK(atp_graph_familiarity(rebuilt, "fire") == 0.0f);

    /* --- Determinism with withdrawals: rebuild twice, compare. --- */
    atp_graph *again = atp_graph_create(&config);
    atp_replay_report second_report = {0};
    CHECK(atp_replay_ledger(ledger, again, &second_report) == ATP_OK);
    CHECK(second_report.excluded_withdrawn == report.excluded_withdrawn);
    const atp_graph_stats sa = atp_graph_get_stats(rebuilt);
    const atp_graph_stats sb = atp_graph_get_stats(again);
    CHECK(sa.node_count == sb.node_count && sa.edge_count == sb.edge_count);
    CHECK(sa.training_steps == sb.training_steps && sa.mean_loss == sb.mean_loss);
    CHECK(atp_graph_familiarity(rebuilt, "moon") == atp_graph_familiarity(again, "moon"));

    /* --- Argument validation. --- */
    CHECK(atp_ledger_withdraw(NULL, 1u) == ATP_ERR_INVALID_ARGUMENT);
    CHECK(atp_ledger_withdraw(ledger, 0u) == ATP_ERR_NOT_FOUND);
    CHECK(atp_ledger_withdraw(ledger, 999u) == ATP_ERR_NOT_FOUND);
    CHECK(atp_ledger_withdraw_source(NULL, "x") == 0u);
    CHECK(atp_ledger_withdraw_author(ledger, NULL) == 0u);

    atp_graph_destroy(again);
    atp_graph_destroy(rebuilt);
    atp_ledger_destroy(ledger);

    remove_dir_files(dir);
    return 0;
}

int main(int argc, char **argv) {
    if (argc != 3) {
        fprintf(stderr, "usage: %s withdraw <dir>\n", argv[0]);
        return 2;
    }
    if (strcmp(argv[1], "withdraw") == 0) {
        return run_withdraw(argv[2]);
    }
    fprintf(stderr, "unknown command: %s\n", argv[1]);
    return 2;
}
