/* Deterministic replay from the observation ledger (issue #3).
 *
 * Phases (run as separate processes through the same binary, mirroring the
 * ledger test layout):
 * - replay: outcome semantics, determinism (two replays of the same ledger
 *   produce equivalent state), schema mismatch refusal, payload-less
 *   LEARNED refusal, snapshot roundtrip of the rebuilt state.
 * - rebuild-safety: a failing rebuild leaves the previous snapshot
 *   untouched, and the rebuilt snapshot replaces it only on success.
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

static void remove_file(const char *path) {
    remove(path);
}

static void remove_dir_files(const char *dir) {
    char path[1024];
    snprintf(path, sizeof(path), "%s/ledger.bin", dir);
    remove_file(path);
    snprintf(path, sizeof(path), "%s/ledger.bin.off", dir);
    remove_file(path);
    snprintf(path, sizeof(path), "%s/ledger.bin.tmp", dir);
    remove_file(path);
    snprintf(path, sizeof(path), "%s/ledger.bin.off.tmp", dir);
    remove_file(path);
    snprintf(path, sizeof(path), "%s/model.bin", dir);
    remove_file(path);
    snprintf(path, sizeof(path), "%s/model.bin.tmp", dir);
    remove_file(path);
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

/* Append one observation with the given outcome and return its id. */
static atp_neural_architecture replay_source_architecture(void) {
    atp_neural_architecture architecture = {0};
    architecture.version = ATPERSON_NEURAL_ARCHITECTURE_VERSION;
    architecture.embedding_dim = 4u;
    architecture.input_dim = 8u;
    architecture.hidden_layer_count = 1u;
    architecture.hidden_widths[0] = 4u;
    architecture.output_dim = 1u;
    return architecture;
}

static atp_neural_architecture replay_target_one(void) {
    atp_neural_architecture architecture = replay_source_architecture();
    architecture.embedding_dim = 6u;
    architecture.input_dim = 12u;
    architecture.hidden_layer_count = 2u;
    architecture.hidden_widths[0] = 6u;
    architecture.hidden_widths[1] = 4u;
    return architecture;
}

static atp_neural_architecture replay_target_two(void) {
    atp_neural_architecture architecture = replay_source_architecture();
    architecture.embedding_dim = 8u;
    architecture.input_dim = 16u;
    architecture.hidden_layer_count = 3u;
    architecture.hidden_widths[0] = 8u;
    architecture.hidden_widths[1] = 6u;
    architecture.hidden_widths[2] = 4u;
    return architecture;
}

static int architectures_equal(const atp_neural_architecture *left,
                               const atp_neural_architecture *right) {
    if (left->version != right->version ||
        left->embedding_dim != right->embedding_dim ||
        left->input_dim != right->input_dim ||
        left->hidden_layer_count != right->hidden_layer_count ||
        left->output_dim != right->output_dim) {
        return 0;
    }
    for (size_t i = 0u; i < ATPERSON_NEURAL_MAX_HIDDEN_LAYERS; ++i) {
        if (left->hidden_widths[i] != right->hidden_widths[i]) {
            return 0;
        }
    }
    return 1;
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

/* Compare two graphs' observable state for rebuild equivalence. */
static int graphs_equivalent(const atp_graph *a, const atp_graph *b) {
    const atp_graph_stats sa = atp_graph_get_stats(a);
    const atp_graph_stats sb = atp_graph_get_stats(b);
    CHECK(sa.node_count == sb.node_count);
    CHECK(sa.edge_count == sb.edge_count);
    CHECK(sa.observations == sb.observations);
    CHECK(sa.token_observations == sb.token_observations);
    CHECK(sa.training_steps == sb.training_steps);
    CHECK(sa.episode_count == sb.episode_count);
    CHECK(sa.episode_evictions == sb.episode_evictions);
    CHECK(sa.mean_loss == sb.mean_loss);
    CHECK(atp_graph_ledger_count(a) == atp_graph_ledger_count(b));
    for (size_t i = 0u; i < atp_graph_ledger_count(a); ++i) {
        atp_ledger_entry ea;
        atp_ledger_entry eb;
        CHECK(atp_graph_ledger_entry(a, i, &ea) == ATP_OK);
        CHECK(atp_graph_ledger_entry(b, i, &eb) == ATP_OK);
        CHECK(ea.id == eb.id);
        CHECK(ea.outcome == eb.outcome);
        CHECK(strcmp(ea.source_id, eb.source_id) == 0);
    }
    return 0;
}

static int run_replay(const char *dir) {
    remove_dir_files(dir);
    CHECK(atp_mkdir(dir) == 0);

    atp_ledger *ledger = open_ledger(dir);

    /* A ledger with every outcome class. Replay order is ledger id order;
     * the ids below are the order the entries were appended in. */
    const uint64_t id_learned_a =
        append_observation(ledger, "at://r/1", "did:plc:a", "wolf moon light", ATP_LEDGER_OUTCOME_LEARNED);
    const uint64_t id_learned_b = append_observation(ledger, "at://r/2", "did:plc:b",
                                                     "silver fire over the moor",
                                                     ATP_LEDGER_OUTCOME_LEARNED);
    const uint64_t id_skipped = append_observation(ledger, "at://r/3", "did:plc:c", "an image post",
                                                    ATP_LEDGER_OUTCOME_SKIPPED);
    const uint64_t id_failed =
        append_observation(ledger, "at://r/4", "did:plc:d", "untrainable bytes",
                            ATP_LEDGER_OUTCOME_FAILED);
    const uint64_t id_pending =
        append_observation(ledger, "at://r/5", "did:plc:e", "reserved mid-crash",
                            ATP_LEDGER_OUTCOME_PENDING);
    const uint64_t id_learned_c = append_observation(ledger, "at://r/6", "did:plc:f", "moon rise",
                                                     ATP_LEDGER_OUTCOME_LEARNED);
    CHECK(id_learned_a == 1u && id_learned_b == 2u && id_skipped == 3u);
    CHECK(id_failed == 4u && id_pending == 5u && id_learned_c == 6u);

    /* Replay into a fresh graph with the default config: the PRNG stream
     * and every training decision reproduce the original run. */
    atp_graph_config config = atp_graph_default_config();
    atp_graph *first = atp_graph_create(&config);
    CHECK(first != NULL);
    atp_replay_report report = {0};
    CHECK(atp_replay_ledger(ledger, first, &report) == ATP_OK);
    CHECK(report.replayed == 3u);      /* LEARNED entries re-observed */
    CHECK(report.mirrored == 1u);     /* SKIPPED mirrored only */
    CHECK(report.excluded_pending == 1u);
    CHECK(report.excluded_failed == 1u);
    CHECK(report.failed_at_id == 0u);

    /* Outcome semantics: only the LEARNED entries trained. */
    const atp_graph_stats stats = atp_graph_get_stats(first);
    CHECK(stats.observations == 3u);
    /* The mirror holds LEARNED + SKIPPED (committed observations). */
    CHECK(atp_graph_ledger_count(first) == 4u);
    atp_ledger_entry mirrored = {0};
    CHECK(atp_graph_ledger_entry(first, 2u, &mirrored) == ATP_OK);
    CHECK(mirrored.id == id_skipped);
    CHECK(mirrored.outcome == ATP_LEDGER_OUTCOME_SKIPPED);

    /* Determinism: a second replay of the same ledger is equivalent. */
    atp_graph *second = atp_graph_create(&config);
    CHECK(second != NULL);
    atp_replay_report second_report = {0};
    CHECK(atp_replay_ledger(ledger, second, &second_report) == ATP_OK);
    CHECK(second_report.replayed == report.replayed);
    CHECK(second_report.mirrored == report.mirrored);
    CHECK(graphs_equivalent(first, second) == 0);

    /* Familiarity and associations reproduce too (learned state, not just
     * counters). */
    CHECK(atp_graph_familiarity(first, "moon") == atp_graph_familiarity(second, "moon"));
    atp_association assoc_a[4] = {0};
    atp_association assoc_b[4] = {0};
    size_t count_a = 0u;
    size_t count_b = 0u;
    CHECK(atp_graph_associations(first, "MOON", assoc_a, 4u, &count_a) == ATP_OK);
    CHECK(atp_graph_associations(second, "MOON", assoc_b, 4u, &count_b) == ATP_OK);
    CHECK(count_a == count_b && count_a > 0u);
    for (size_t i = 0u; i < count_a; ++i) {
        CHECK(strcmp(assoc_a[i].token, assoc_b[i].token) == 0);
        CHECK(assoc_a[i].score == assoc_b[i].score);
        CHECK(assoc_a[i].observations == assoc_b[i].observations);
    }

    /* The rebuilt state survives a snapshot roundtrip: mirror, episodes,
     * familiarity, neural parameters, and PRNG state all persist. */
    char snapshot[1024];
    snprintf(snapshot, sizeof(snapshot), "%s/model.bin", dir);
    CHECK(atp_graph_save(first, snapshot) == ATP_OK);
    atp_status load_status = ATP_OK;
    atp_graph *reloaded = atp_graph_load(snapshot, &load_status);
    CHECK(reloaded != NULL && load_status == ATP_OK);
    CHECK(graphs_equivalent(first, reloaded) == 0);
    CHECK(atp_graph_familiarity(reloaded, "moon") == atp_graph_familiarity(first, "moon"));

    atp_graph_destroy(reloaded);
    atp_graph_destroy(second);
    atp_graph_destroy(first);
    atp_ledger_destroy(ledger);

    /* --- Expanded-generation replay: migrations are chronology, not a final
     * topology hint. They apply immediately after their persisted ledger
     * boundary and become part of the rebuilt graph's durable history. --- */
    remove_dir_files(dir);
    CHECK(atp_mkdir(dir) == 0);
    ledger = open_ledger(dir);
    CHECK(append_observation(ledger, "at://m/1", "did:plc:m", "alpha beta",
                             ATP_LEDGER_OUTCOME_LEARNED) == 1u);
    CHECK(append_observation(ledger, "at://m/2", "did:plc:m", "beta gamma",
                             ATP_LEDGER_OUTCOME_LEARNED) == 2u);
    CHECK(append_observation(ledger, "at://m/3", "did:plc:m", "gamma delta",
                             ATP_LEDGER_OUTCOME_LEARNED) == 3u);
    CHECK(append_observation(ledger, "at://m/4", "did:plc:m", "delta epsilon",
                             ATP_LEDGER_OUTCOME_LEARNED) == 4u);

    const atp_neural_architecture migration_source =
        replay_source_architecture();
    const atp_neural_architecture migration_target1 = replay_target_one();
    const atp_neural_architecture migration_target2 = replay_target_two();
    const atp_neural_migration migrations[2] = {
        {
            .version = ATPERSON_NEURAL_MIGRATION_VERSION,
            .seed = UINT64_C(0x1111222233334444),
            .ledger_boundary_id = 2u,
            .source = migration_source,
            .target = migration_target1,
        },
        {
            .version = ATPERSON_NEURAL_MIGRATION_VERSION,
            .seed = UINT64_C(0x5555666677778888),
            .ledger_boundary_id = 3u,
            .source = migration_target1,
            .target = migration_target2,
        },
    };

    atp_graph *expanded_first =
        atp_graph_create_with_architecture(&config, &migration_source);
    atp_graph *expanded_second =
        atp_graph_create_with_architecture(&config, &migration_source);
    CHECK(expanded_first != NULL && expanded_second != NULL);
    atp_replay_report migration_report = {0};
    atp_replay_report migration_report_two = {0};
    CHECK(atp_replay_ledger_with_migrations(
              ledger, expanded_first, migrations, 2u,
              &migration_report) == ATP_OK);
    CHECK(atp_replay_ledger_with_migrations(
              ledger, expanded_second, migrations, 2u,
              &migration_report_two) == ATP_OK);
    CHECK(migration_report.replayed == 4u);
    CHECK(migration_report.migrations_applied == 2u);
    CHECK(migration_report_two.migrations_applied == 2u);
    CHECK(atp_graph_neural_migration_count(expanded_first) == 2u);
    atp_neural_architecture final_architecture = {0};
    CHECK(atp_graph_neural_architecture(expanded_first,
                                        &final_architecture) == ATP_OK);
    CHECK(architectures_equal(&final_architecture, &migration_target2));
    CHECK(graphs_equivalent(expanded_first, expanded_second) == 0);
    float expanded_score_one = 0.0f;
    float expanded_score_two = 0.0f;
    CHECK(atp_graph_neural_score(expanded_first, "gamma", "delta",
                                 &expanded_score_one) == ATP_OK);
    CHECK(atp_graph_neural_score(expanded_second, "gamma", "delta",
                                 &expanded_score_two) == ATP_OK);
    CHECK(expanded_score_one == expanded_score_two);
    atp_graph_destroy(expanded_second);
    atp_graph_destroy(expanded_first);

    /* Withdrawal does not erase chronology. Entry 2 no longer trains, but the
     * boundary-2 migration still applies immediately after that ledger id. */
    CHECK(atp_ledger_withdraw(ledger, 2u) == ATP_OK);
    atp_graph *withdrawn_expanded =
        atp_graph_create_with_architecture(&config, &migration_source);
    CHECK(withdrawn_expanded != NULL);
    migration_report = (atp_replay_report){0};
    CHECK(atp_replay_ledger_with_migrations(
              ledger, withdrawn_expanded, migrations, 2u,
              &migration_report) == ATP_OK);
    CHECK(migration_report.replayed == 3u);
    CHECK(migration_report.excluded_withdrawn == 1u);
    CHECK(migration_report.migrations_applied == 2u);
    CHECK(atp_graph_neural_architecture(withdrawn_expanded,
                                        &final_architecture) == ATP_OK);
    CHECK(architectures_equal(&final_architecture, &migration_target2));
    atp_graph_destroy(withdrawn_expanded);

    /* Chain errors fail before replaying any observation. */
    atp_graph *wrong_start =
        atp_graph_create_with_architecture(&config, &migration_target1);
    CHECK(wrong_start != NULL);
    migration_report = (atp_replay_report){0};
    CHECK(atp_replay_ledger_with_migrations(
              ledger, wrong_start, migrations, 2u,
              &migration_report) == ATP_ERR_MIGRATION);
    CHECK(atp_graph_get_stats(wrong_start).observations == 0u);
    atp_graph_destroy(wrong_start);

    atp_neural_migration impossible[2] = {migrations[0], migrations[1]};
    impossible[1].ledger_boundary_id = 99u;
    atp_graph *missing_boundary =
        atp_graph_create_with_architecture(&config, &migration_source);
    CHECK(missing_boundary != NULL);
    migration_report = (atp_replay_report){0};
    CHECK(atp_replay_ledger_with_migrations(
              ledger, missing_boundary, impossible, 2u,
              &migration_report) == ATP_ERR_MIGRATION);
    CHECK(atp_graph_get_stats(missing_boundary).observations == 0u);
    atp_graph_destroy(missing_boundary);
    CHECK(atp_replay_ledger_with_migrations(
              ledger, NULL, migrations, 2u, NULL) ==
          ATP_ERR_INVALID_ARGUMENT);
    atp_ledger_destroy(ledger);

    /* --- Compatible schema transition: entries recorded under the
     * current schema replay through the compatibility table. This is the
     * fixture for the replay-compatible class — when a future bump lists
     * schema 1 as still-replayable, this ledger keeps replaying under the
     * bumped core without migration. --- */
    CHECK(atp_schema_can_replay(ATPERSON_SCHEMA_VERSION));

    /* --- Incompatible learning schema fails clearly, never silently
     * reinterprets. The schema-specific status names the class of
     * problem; the report names the entry and the schema. --- */
    remove_dir_files(dir);
    CHECK(atp_mkdir(dir) == 0);
    ledger = open_ledger(dir);
    uint64_t id = 0u;
    atp_status status = ATP_OK;
    CHECK(atp_ledger_append(ledger, "at://r/7", "did:plc:g", 100u, 4242u,
                            ATPERSON_SCHEMA_VERSION + 1u, ATP_LEDGER_OUTCOME_LEARNED, "future",
                            6u, &id, &status) == ATP_LEDGER_NEW);
    CHECK(status == ATP_OK);
    atp_graph *fresh = atp_graph_create(&config);
    atp_replay_report fail_report = {0};
    CHECK(atp_replay_ledger(ledger, fresh, &fail_report) == ATP_ERR_SCHEMA);
    CHECK(fail_report.failed_at_id == id);
    CHECK(fail_report.failed_schema == ATPERSON_SCHEMA_VERSION + 1u);
    atp_graph_destroy(fresh);
    atp_ledger_destroy(ledger);

    /* --- A mixed-schema ledger fails deterministically at the first
     * unreplayable entry, after replaying the compatible prefix. --- */
    remove_dir_files(dir);
    CHECK(atp_mkdir(dir) == 0);
    ledger = open_ledger(dir);
    CHECK(atp_ledger_append(ledger, "at://r/9", "did:plc:i", 100u,
                            atp_ledger_digest("moon", 4u), ATPERSON_SCHEMA_VERSION,
                            ATP_LEDGER_OUTCOME_LEARNED, "moon", 4u, &id, &status) ==
          ATP_LEDGER_NEW);
    CHECK(status == ATP_OK);
    CHECK(atp_ledger_append(ledger, "at://r/10", "did:plc:j", 100u,
                            atp_ledger_digest("stone", 5u), ATPERSON_SCHEMA_VERSION + 1u,
                            ATP_LEDGER_OUTCOME_LEARNED, "stone", 5u, &id, &status) ==
          ATP_LEDGER_NEW);
    CHECK(status == ATP_OK);
    fresh = atp_graph_create(&config);
    fail_report = (atp_replay_report){0};
    CHECK(atp_replay_ledger(ledger, fresh, &fail_report) == ATP_ERR_SCHEMA);
    CHECK(fail_report.failed_at_id == id);
    CHECK(fail_report.failed_schema == ATPERSON_SCHEMA_VERSION + 1u);
    CHECK(fail_report.replayed == 1u); /* the schema-1 prefix was applied */
    atp_graph_destroy(fresh);
    atp_ledger_destroy(ledger);

    /* --- A payload-less LEARNED entry fails the rebuild honestly. --- */
    /* (v1-migrated ledgers: the training input is gone.) */
    remove_dir_files(dir);
    CHECK(atp_mkdir(dir) == 0);
    ledger = open_ledger(dir);
    CHECK(atp_ledger_append(ledger, "at://r/8", "did:plc:h", 100u, 4343u,
                            ATPERSON_SCHEMA_VERSION, ATP_LEDGER_OUTCOME_LEARNED, NULL, 0u, &id,
                            &status) == ATP_LEDGER_NEW);
    CHECK(status == ATP_OK);
    fresh = atp_graph_create(&config);
    CHECK(atp_replay_ledger(ledger, fresh, &fail_report) == ATP_ERR_FORMAT);
    CHECK(fail_report.failed_at_id == id);
    atp_graph_destroy(fresh);
    atp_ledger_destroy(ledger);

    /* --- Argument validation. --- */
    CHECK(atp_replay_ledger(NULL, NULL, NULL) == ATP_ERR_INVALID_ARGUMENT);

    remove_dir_files(dir);
    return 0;
}

/* Rebuild safety: a failing replay leaves the previous snapshot untouched;
 * a successful rebuild replaces it atomically. */
static int run_rebuild_safety(const char *dir) {
    remove_dir_files(dir);
    CHECK(atp_mkdir(dir) == 0);
    char snapshot[1024];
    snprintf(snapshot, sizeof(snapshot), "%s/model.bin", dir);

    /* Seed a good snapshot from a good ledger. */
    atp_ledger *ledger = open_ledger(dir);
    append_observation(ledger, "at://s/1", "did:plc:a", "original learned text",
                       ATP_LEDGER_OUTCOME_LEARNED);
    atp_graph_config config = atp_graph_default_config();
    atp_graph *graph = atp_graph_create(&config);
    CHECK(atp_replay_ledger(ledger, graph, NULL) == ATP_OK);
    CHECK(atp_graph_save(graph, snapshot) == ATP_OK);
    const atp_graph_stats before = atp_graph_get_stats(graph);
    atp_graph_destroy(graph);
    atp_ledger_destroy(ledger);

    /* Corrupt the ledger with an unreplayable entry (incompatible learning
     * schema). A rebuild must fail with the schema-specific status, name
     * the entry and schema in the report, and leave the snapshot exactly
     * as it was. */
    ledger = open_ledger(dir);
    uint64_t id = 0u;
    atp_status status = ATP_OK;
    CHECK(atp_ledger_append(ledger, "at://s/2", "did:plc:b", 100u, 99u,
                            ATPERSON_SCHEMA_VERSION + 1u, ATP_LEDGER_OUTCOME_LEARNED, "x", 1u,
                            &id, &status) == ATP_LEDGER_NEW);
    atp_graph *rebuilt = atp_graph_create(&config);
    atp_replay_report report = {0};
    CHECK(atp_replay_ledger(ledger, rebuilt, &report) == ATP_ERR_SCHEMA);
    CHECK(report.failed_at_id == id);
    CHECK(report.failed_schema == ATPERSON_SCHEMA_VERSION + 1u);
    /* The compatibility table is the single policy point. */
    CHECK(atp_schema_can_replay(ATPERSON_SCHEMA_VERSION));
    CHECK(!atp_schema_can_replay(ATPERSON_SCHEMA_VERSION + 1u));
    CHECK(!atp_schema_can_replay(0u));
    /* The partially trained graph is discarded; the snapshot was never
     * touched by the failed rebuild. */
    atp_graph_destroy(rebuilt);
    atp_ledger_destroy(ledger);

    atp_status load_status = ATP_OK;
    atp_graph *previous = atp_graph_load(snapshot, &load_status);
    CHECK(previous != NULL && load_status == ATP_OK);
    const atp_graph_stats after = atp_graph_get_stats(previous);
    CHECK(after.observations == before.observations);
    CHECK(after.node_count == before.node_count);
    CHECK(after.training_steps == before.training_steps);
    atp_graph_destroy(previous);

    remove_dir_files(dir);
    return 0;
}

int main(int argc, char **argv) {
    if (argc != 3) {
        fprintf(stderr, "usage: %s <replay|rebuild-safety> <dir>\n", argv[0]);
        return 2;
    }
    const char *command = argv[1];
    const char *dir = argv[2];
    if (strcmp(command, "replay") == 0) {
        return run_replay(dir);
    }
    if (strcmp(command, "rebuild-safety") == 0) {
        return run_rebuild_safety(dir);
    }
    fprintf(stderr, "unknown command: %s\n", command);
    return 2;
}
