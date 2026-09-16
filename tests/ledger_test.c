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

typedef struct toy_post {
    const char *uri;
    const char *author;
    const char *text;
    uint64_t observed_at;
    atp_ledger_outcome outcome;
} toy_post;

static uint64_t content_digest(const toy_post *post) {
    return atp_ledger_digest(post->text, strlen(post->text));
}

static int remove_ledger_files(const char *dir) {
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
    return 0;
}

static atp_ledger *open_or_fail(const char *path) {
    atp_status status = ATP_OK;
    atp_ledger *ledger = atp_ledger_open(path, &status);
    if (ledger == NULL || status != ATP_OK) {
        fprintf(stderr, "atp_ledger_open failed for %s (status %d)\n", path, (int)status);
        return NULL;
    }
    return ledger;
}

static uint64_t append_post(atp_ledger *ledger, const toy_post *post) {
    uint64_t id = 0u;
    atp_status status = ATP_OK;
    CHECK(atp_ledger_append(ledger, post->uri, post->author, post->observed_at,
                            content_digest(post), ATPERSON_SCHEMA_VERSION, post->outcome, &id,
                            &status) == ATP_LEDGER_NEW);
    CHECK(status == ATP_OK);
    return id;
}

static int check_entry_matches(const atp_ledger_entry *entry, const toy_post *post) {
    CHECK(entry->observed_at == post->observed_at);
    CHECK(entry->content_digest == content_digest(post));
    CHECK(entry->schema_version == ATPERSON_SCHEMA_VERSION);
    CHECK(entry->outcome == post->outcome);
    CHECK(strcmp(entry->source_id, post->uri) == 0);
    CHECK(strcmp(entry->author_did, post->author) == 0);
    return 0;
}

/* Round-trip persistence on a single ledger file: append, close, reopen, and
 * verify every field survived. Also exercises the format-version constant,
 * outcome patches, invalid-id handling, and key validation. */
static int run_roundtrip(const char *dir) {
    remove_ledger_files(dir);
    CHECK(atp_mkdir(dir) == 0);
    CHECK(ATPERSON_LEDGER_VERSION == 1u);

    char path[1024];
    snprintf(path, sizeof(path), "%s/ledger.bin", dir);

    atp_ledger *ledger = open_or_fail(path);

    const toy_post first = {"at://round/1", "did:plc:first", "round trip one", 1234,
                            ATP_LEDGER_OUTCOME_PENDING};
    const toy_post second = {"at://round/2", "did:plc:second", "round trip two", 5678,
                             ATP_LEDGER_OUTCOME_LEARNED};
    const uint64_t id1 = append_post(ledger, &first);
    const uint64_t id2 = append_post(ledger, &second);
    CHECK(id1 == 1u);
    CHECK(id2 == 2u);
    CHECK(atp_ledger_count(ledger) == 2u);

    /* Patch the first reservation to committed and confirm the patch path. */
    CHECK(atp_ledger_set_outcome(ledger, id1, ATP_LEDGER_OUTCOME_LEARNED) == ATP_OK);
    /* Unknown ids are not silently accepted. */
    CHECK(atp_ledger_set_outcome(ledger, 999u, ATP_LEDGER_OUTCOME_LEARNED) == ATP_ERR_NOT_FOUND);
    /* A committed entry cannot be reopened, but the retryable FAILED state
     * can be reset and then completed. */
    CHECK(atp_ledger_set_outcome(ledger, id1, ATP_LEDGER_OUTCOME_PENDING) ==
          ATP_ERR_INVALID_ARGUMENT);
    const toy_post third = {"at://round/3", "did:plc:third", "round trip three", 9012,
                            ATP_LEDGER_OUTCOME_FAILED};
    const uint64_t id3 = append_post(ledger, &third);
    CHECK(atp_ledger_set_outcome(ledger, id3, ATP_LEDGER_OUTCOME_PENDING) == ATP_OK);
    CHECK(atp_ledger_set_outcome(ledger, id3, ATP_LEDGER_OUTCOME_LEARNED) == ATP_OK);

    atp_ledger_destroy(ledger);

    /* Overlong keys are rejected before any durable write is made. */
    ledger = open_or_fail(path);
    char long_source[ATPERSON_LEDGER_SOURCE_BYTES + 8u];
    memset(long_source, 'x', sizeof(long_source) - 1u);
    long_source[sizeof(long_source) - 1u] = '\0';
    atp_status status = ATP_OK;
    uint64_t unused = 0u;
    CHECK(atp_ledger_append(ledger, long_source, "", 0u, 0u, ATPERSON_SCHEMA_VERSION,
                            ATP_LEDGER_OUTCOME_PENDING, &unused, &status) == ATP_LEDGER_NOT_FOUND);
    CHECK(status == ATP_ERR_INVALID_ARGUMENT);

    /* Reopen and verify all fields survived the round trip. */
    atp_ledger_destroy(ledger);
    ledger = open_or_fail(path);
    CHECK(atp_ledger_count(ledger) == 3u);

    atp_ledger_entry entry = {0};
    CHECK(atp_ledger_entry_at(ledger, 0u, &entry) == ATP_OK);
    CHECK(entry.id == id1);
    CHECK(entry.outcome == ATP_LEDGER_OUTCOME_LEARNED);
    toy_post first_committed = first;
    first_committed.outcome = ATP_LEDGER_OUTCOME_LEARNED;
    CHECK(check_entry_matches(&entry, &first_committed) == 0);
    CHECK(atp_ledger_entry_at(ledger, 1u, &entry) == ATP_OK);
    CHECK(check_entry_matches(&entry, &second) == 0);
    CHECK(atp_ledger_entry_at(ledger, 2u, &entry) == ATP_OK);
    CHECK(entry.outcome == ATP_LEDGER_OUTCOME_LEARNED);
    toy_post third_committed = third;
    third_committed.outcome = ATP_LEDGER_OUTCOME_LEARNED;
    CHECK(check_entry_matches(&entry, &third_committed) == 0);
    CHECK(atp_ledger_entry_at(ledger, 3u, &entry) == ATP_ERR_NOT_FOUND);

    /* Dedup reflects the patched outcomes after the reopen. */
    CHECK(atp_ledger_lookup(ledger, "at://round/1", content_digest(&first), &entry) ==
          ATP_LEDGER_EXISTS_COMMITTED);
    CHECK(atp_ledger_lookup(ledger, "at://round/3", content_digest(&third), &entry) ==
          ATP_LEDGER_EXISTS_COMMITTED);

    atp_ledger_destroy(ledger);
    remove_ledger_files(dir);
    return 0;
}

/* First of two *separate OS processes* sharing one ledger directory. Commits
 * two posts and leaves a third reservation in PENDING to simulate a crash
 * before the outcome was finalised. */
static int run_dedup_phase1(const char *dir) {
    remove_ledger_files(dir);
    CHECK(atp_mkdir(dir) == 0);

    char path[1024];
    snprintf(path, sizeof(path), "%s/ledger.bin", dir);
    atp_ledger *ledger = open_or_fail(path);

    const toy_post post_a = {"at://post/a", "did:plc:alice", "hello world", 100u,
                             ATP_LEDGER_OUTCOME_LEARNED};
    const toy_post post_b = {"at://post/b", "did:plc:bob", "moonlight", 200u,
                             ATP_LEDGER_OUTCOME_LEARNED};
    const toy_post post_c = {"at://post/c", "did:plc:cara", "crash before finalise", 300u,
                             ATP_LEDGER_OUTCOME_PENDING};
    append_post(ledger, &post_a);
    append_post(ledger, &post_b);
    append_post(ledger, &post_c);
    CHECK(atp_ledger_count(ledger) == 3u);

    atp_ledger_destroy(ledger);
    return 0;
}

/* Second process run over the same files: the unique (source id + digest)
 * index must block every already-committed post, expose the pending post as
 * retryable, and accept genuinely new content. Digests are recomputed from
 * the same source text, proving the key is stable across runs. */
static int run_dedup_phase2(const char *dir) {
    char path[1024];
    snprintf(path, sizeof(path), "%s/ledger.bin", dir);
    atp_ledger *ledger = open_or_fail(path);
    CHECK(atp_ledger_count(ledger) == 3u);

    /* Fresh-process recomputation of each post's digest. */
    const uint64_t digest_a = atp_ledger_digest("hello world", 11u);
    const uint64_t digest_b = atp_ledger_digest("moonlight", 9u);
    const uint64_t digest_c = atp_ledger_digest("crash before finalise", 21u);

    atp_ledger_entry entry = {0};
    CHECK(atp_ledger_lookup(ledger, "at://post/a", digest_a, &entry) ==
          ATP_LEDGER_EXISTS_COMMITTED);
    CHECK(entry.id == 1u);
    CHECK(entry.outcome == ATP_LEDGER_OUTCOME_LEARNED);
    CHECK(entry.content_digest == digest_a);
    CHECK(strcmp(entry.author_did, "did:plc:alice") == 0);

    atp_status status = ATP_OK;
    uint64_t out_id = 0u;

    /* Re-appending the same posts must never create new entries. */
    CHECK(atp_ledger_append(ledger, "at://post/a", "did:plc:alice", 100u, digest_a,
                            ATPERSON_SCHEMA_VERSION, ATP_LEDGER_OUTCOME_PENDING, &out_id,
                            &status) == ATP_LEDGER_EXISTS_COMMITTED);
    CHECK(status == ATP_OK);
    CHECK(atp_ledger_append(ledger, "at://post/b", "did:plc:bob", 200u, digest_b,
                            ATPERSON_SCHEMA_VERSION, ATP_LEDGER_OUTCOME_PENDING, &out_id,
                            &status) == ATP_LEDGER_EXISTS_COMMITTED);
    CHECK(status == ATP_OK);

    /* The entry left PENDING by phase 1 is retryable, not a duplicate. */
    CHECK(atp_ledger_append(ledger, "at://post/c", "did:plc:cara", 300u, digest_c,
                            ATPERSON_SCHEMA_VERSION, ATP_LEDGER_OUTCOME_PENDING, &out_id,
                            &status) == ATP_LEDGER_EXISTS_PENDING);
    CHECK(status == ATP_OK);
    CHECK(out_id == 3u);

    /* Same source, different digest (an edited post) is a new observation. */
    const uint64_t digest_c_edited = atp_ledger_digest("edited again", 12u);
    CHECK(atp_ledger_append(ledger, "at://post/c", "did:plc:cara", 301u, digest_c_edited,
                            ATPERSON_SCHEMA_VERSION, ATP_LEDGER_OUTCOME_PENDING, &out_id,
                            &status) == ATP_LEDGER_NEW);
    CHECK(status == ATP_OK);
    CHECK(out_id == 4u);
    CHECK(atp_ledger_set_outcome(ledger, out_id, ATP_LEDGER_OUTCOME_LEARNED) == ATP_OK);

    /* Finish the crashed reservation and re-check the index flips. */
    CHECK(atp_ledger_set_outcome(ledger, 3u, ATP_LEDGER_OUTCOME_LEARNED) == ATP_OK);
    CHECK(atp_ledger_lookup(ledger, "at://post/c", digest_c, &entry) ==
          ATP_LEDGER_EXISTS_COMMITTED);

    CHECK(atp_ledger_count(ledger) == 4u);
    atp_ledger_destroy(ledger);
    return 0;
}

/* Crash safety. Simulates the two crash windows the write ordering defends:
 *
 *  - a torn tail in the log after the log fsync but before the commit-marker
 *    rename is discarded on reopen (the previous marker wins); and
 *  - a lost/missing commit marker heals from the longest valid log prefix.
 *
 * Also verifies that damage inside the committed prefix is refused loudly
 * instead of silently truncating.
 */
static int run_crash_safety(const char *dir) {
    remove_ledger_files(dir);
    CHECK(atp_mkdir(dir) == 0);

    char path[1024];
    snprintf(path, sizeof(path), "%s/ledger.bin", dir);
    char off_path[1024];
    snprintf(off_path, sizeof(off_path), "%s/ledger.bin.off", dir);
    char off_tmp_path[1024];
    snprintf(off_tmp_path, sizeof(off_tmp_path), "%s/ledger.bin.off.tmp", dir);

    const toy_post alpha = {"at://crash/x/1", "did:plc:alpha", "alpha text", 4000u,
                            ATP_LEDGER_OUTCOME_LEARNED};
    atp_ledger *ledger = open_or_fail(path);
    append_post(ledger, &alpha);
    atp_ledger_destroy(ledger);

    /* Crash window 1: record bytes reached the log but the marker rename
     * never ran. A torn tail and a half-written staging marker are left
     * behind. */
    FILE *file = fopen(path, "ab");
    CHECK(file != NULL);
    static const char torn_tail[] = "torn-tail-garbage-after-committed-fence";
    CHECK(fwrite(torn_tail, 1u, sizeof(torn_tail) - 1u, file) == sizeof(torn_tail) - 1u);
    CHECK(fclose(file) == 0);
    file = fopen(off_tmp_path, "wb");
    CHECK(file != NULL);
    CHECK(fwrite("partial-marker-write", 1u, 19u, file) == 19u);
    CHECK(fclose(file) == 0);

    ledger = open_or_fail(path);
    CHECK(atp_ledger_count(ledger) == 1u);
    atp_ledger_entry entry = {0};
    CHECK(atp_ledger_lookup(ledger, "at://crash/x/1", content_digest(&alpha), &entry) ==
          ATP_LEDGER_EXISTS_COMMITTED);
    atp_ledger_destroy(ledger);

    /* The stale staging file must have been removed and the tail truncated:
     * a subsequent append must not collide with leftover bytes. */
    file = fopen(off_tmp_path, "rb");
    CHECK(file == NULL);
    ledger = open_or_fail(path);
    CHECK(atp_ledger_count(ledger) == 1u);
    const toy_post bravo = {"at://crash/x/2", "did:plc:bravo", "bravo text", 4001u,
                            ATP_LEDGER_OUTCOME_LEARNED};
    append_post(ledger, &bravo);
    atp_ledger_destroy(ledger);

    /* Crash window 2: the marker itself is lost. The log is the authority,
     * so the valid prefix heals and a fresh marker is written. */
    CHECK(remove(off_path) == 0);
    ledger = open_or_fail(path);
    CHECK(atp_ledger_count(ledger) == 2u);
    atp_ledger_destroy(ledger);

    /* Damage inside the committed prefix (between the marker fence) must be a
     * loud format error, never a silent partial recovery. */
    ledger = open_or_fail(path);
    atp_ledger_destroy(ledger);
    file = fopen(path, "rb+");
    CHECK(file != NULL);
    CHECK(fseek(file, 12 + 8, SEEK_SET) == 0); /* record 1 type byte */
    CHECK(fputc('x', file) != EOF);
    CHECK(fclose(file) == 0);
    atp_status status = ATP_OK;
    atp_ledger *damaged = atp_ledger_open(path, &status);
    CHECK(damaged == NULL);
    CHECK(status == ATP_ERR_FORMAT);

    remove_ledger_files(dir);
    return 0;
}

int main(int argc, char **argv) {
    if (argc != 3) {
        fprintf(stderr,
                "usage: %s <roundtrip|dedup-phase1|dedup-phase2|"
                "crash-safety> <dir>\n",
                argv[0]);
        return 2;
    }
    const char *command = argv[1];
    const char *dir = argv[2];

    if (strcmp(command, "roundtrip") == 0) {
        return run_roundtrip(dir);
    }
    if (strcmp(command, "dedup-phase1") == 0) {
        return run_dedup_phase1(dir);
    }
    if (strcmp(command, "dedup-phase2") == 0) {
        return run_dedup_phase2(dir);
    }
    if (strcmp(command, "crash-safety") == 0) {
        return run_crash_safety(dir);
    }
    fprintf(stderr, "unknown command: %s\n", command);
    return 2;
}
