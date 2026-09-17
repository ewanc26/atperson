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

/* Local little-endian store helpers + record CRC for hand-building v1 log
 * files; the ledger's own are internal to ledger.c. */
static void store_u32_le(unsigned char *out, uint32_t value) {
    out[0] = (unsigned char)(value & 0xffu);
    out[1] = (unsigned char)((value >> 8u) & 0xffu);
    out[2] = (unsigned char)((value >> 16u) & 0xffu);
    out[3] = (unsigned char)((value >> 24u) & 0xffu);
}

static void store_u64_le(unsigned char *out, uint64_t value) {
    for (unsigned i = 0u; i < 8u; ++i) {
        out[i] = (unsigned char)((value >> (8u * i)) & 0xffu);
    }
}

static uint32_t record_checksum(const void *data, size_t length) {
    const unsigned char *cursor = data;
    uint64_t hash = UINT64_C(1469598103934665603);
    for (size_t i = 0u; i < length; ++i) {
        hash ^= (uint64_t)cursor[i];
        hash *= UINT64_C(1099511628211);
    }
    return (uint32_t)(hash ^ (hash >> 32u));
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
                            content_digest(post), ATPERSON_SCHEMA_VERSION, post->outcome,
                            post->text, strlen(post->text), &id, &status) == ATP_LEDGER_NEW);
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
    CHECK(ATPERSON_LEDGER_VERSION == 3u);

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
                            ATP_LEDGER_OUTCOME_PENDING, NULL, 0u, &unused,
                            &status) == ATP_LEDGER_NOT_FOUND);
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

    /* Retained payloads survive the reopen byte-for-byte. */
    char out[64];
    size_t out_len = 0u;
    CHECK(atp_ledger_entry_payload(ledger, id1, out, sizeof(out), &out_len) == ATP_OK);
    CHECK(out_len == strlen(first.text));
    CHECK(memcmp(out, first.text, out_len) == 0);
    CHECK(atp_ledger_entry_payload(ledger, id2, out, sizeof(out), &out_len) == ATP_OK);
    CHECK(out_len == strlen(second.text));
    CHECK(memcmp(out, second.text, out_len) == 0);
    /* Capacity too small is refused, and a null out only queries the length. */
    CHECK(atp_ledger_entry_payload(ledger, id1, out, 1u, &out_len) == ATP_ERR_INVALID_ARGUMENT);
    CHECK(atp_ledger_entry_payload(ledger, id1, NULL, 0u, &out_len) == ATP_OK);
    CHECK(out_len == strlen(first.text));
    /* Unknown ids are not found. */
    CHECK(atp_ledger_entry_payload(ledger, 999u, out, sizeof(out), &out_len) ==
          ATP_ERR_NOT_FOUND);

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
                            ATPERSON_SCHEMA_VERSION, ATP_LEDGER_OUTCOME_PENDING, NULL, 0u,
                            &out_id, &status) == ATP_LEDGER_EXISTS_COMMITTED);
    CHECK(status == ATP_OK);
    CHECK(atp_ledger_append(ledger, "at://post/b", "did:plc:bob", 200u, digest_b,
                            ATPERSON_SCHEMA_VERSION, ATP_LEDGER_OUTCOME_PENDING, NULL, 0u,
                            &out_id, &status) == ATP_LEDGER_EXISTS_COMMITTED);
    CHECK(status == ATP_OK);

    /* The entry left PENDING by phase 1 is retryable, not a duplicate. */
    CHECK(atp_ledger_append(ledger, "at://post/c", "did:plc:cara", 300u, digest_c,
                            ATPERSON_SCHEMA_VERSION, ATP_LEDGER_OUTCOME_PENDING, NULL, 0u,
                            &out_id, &status) == ATP_LEDGER_EXISTS_PENDING);
    CHECK(status == ATP_OK);
    CHECK(out_id == 3u);

    /* Same source, different digest (an edited post) is a new observation. */
    const uint64_t digest_c_edited = atp_ledger_digest("edited again", 12u);
    CHECK(atp_ledger_append(ledger, "at://post/c", "did:plc:cara", 301u, digest_c_edited,
                            ATPERSON_SCHEMA_VERSION, ATP_LEDGER_OUTCOME_PENDING, NULL, 0u,
                            &out_id, &status) == ATP_LEDGER_NEW);
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

/* Payload variants: binary bytes with embedded NULs, UTF-8, and empty
 * observations all round-trip exactly. Oversized payloads are rejected
 * before any durable write. */
static int run_payloads(const char *dir) {
    remove_ledger_files(dir);
    CHECK(atp_mkdir(dir) == 0);
    char path[1024];
    snprintf(path, sizeof(path), "%s/ledger.bin", dir);

    atp_ledger *ledger = open_or_fail(path);

    /* Binary payload with embedded NULs — the payload is length-prefixed,
     * never NUL-terminated. */
    const unsigned char binary[8] = {0x00, 0xff, 'a', 0x00, 0x01, 0xfe, 0x7f, 0x80};
    const uint64_t binary_digest = atp_ledger_digest(binary, sizeof(binary));
    uint64_t id = 0u;
    atp_status status = ATP_OK;
    CHECK(atp_ledger_append(ledger, "at://bin/1", "did:plc:bin", 10u, binary_digest,
                            ATPERSON_SCHEMA_VERSION, ATP_LEDGER_OUTCOME_LEARNED, binary,
                            sizeof(binary), &id, &status) == ATP_LEDGER_NEW);
    CHECK(status == ATP_OK);
    CHECK(id == 1u);

    /* UTF-8 multibyte payload. */
    const char *utf8 = "Tha mi Pàgannach — oidhche mhath 🐺";
    const size_t utf8_len = strlen(utf8);
    const uint64_t utf8_digest = atp_ledger_digest(utf8, utf8_len);
    CHECK(atp_ledger_append(ledger, "at://utf8/1", "did:plc:gd", 20u, utf8_digest,
                            ATPERSON_SCHEMA_VERSION, ATP_LEDGER_OUTCOME_LEARNED, utf8,
                            utf8_len, &id, &status) == ATP_LEDGER_NEW);
    CHECK(status == ATP_OK);
    CHECK(id == 2u);

    /* Empty payload: append succeeds, read reports honest absence. */
    CHECK(atp_ledger_append(ledger, "at://empty/1", "did:plc:empty", 30u, 0u,
                            ATPERSON_SCHEMA_VERSION, ATP_LEDGER_OUTCOME_LEARNED, NULL, 0u,
                            &id, &status) == ATP_LEDGER_NEW);
    CHECK(status == ATP_OK);
    CHECK(id == 3u);

    /* Payload over the retention limit is rejected before any durable write. */
    unsigned char *oversized = malloc(ATPERSON_LEDGER_PAYLOAD_LIMIT + 1u);
    CHECK(oversized != NULL);
    memset(oversized, 'x', ATPERSON_LEDGER_PAYLOAD_LIMIT + 1u);
    CHECK(atp_ledger_append(ledger, "at://big/1", "did:plc:big", 40u,
                            atp_ledger_digest(oversized, ATPERSON_LEDGER_PAYLOAD_LIMIT + 1u),
                            ATPERSON_SCHEMA_VERSION, ATP_LEDGER_OUTCOME_PENDING, oversized,
                            ATPERSON_LEDGER_PAYLOAD_LIMIT + 1u, &id,
                            &status) == ATP_LEDGER_NOT_FOUND);
    CHECK(status == ATP_ERR_INVALID_ARGUMENT);
    /* NULL payload with nonzero length is rejected too. */
    CHECK(atp_ledger_append(ledger, "at://bad/1", "did:plc:bad", 50u, 123u,
                            ATPERSON_SCHEMA_VERSION, ATP_LEDGER_OUTCOME_PENDING, NULL, 4u,
                            &id, &status) == ATP_LEDGER_NOT_FOUND);
    CHECK(status == ATP_ERR_INVALID_ARGUMENT);
    free(oversized);
    CHECK(atp_ledger_count(ledger) == 3u);

    /* Exactly at the limit is accepted. */
    unsigned char *limit = malloc(ATPERSON_LEDGER_PAYLOAD_LIMIT);
    CHECK(limit != NULL);
    memset(limit, 'y', ATPERSON_LEDGER_PAYLOAD_LIMIT);
    CHECK(atp_ledger_append(ledger, "at://limit/1", "did:plc:limit", 60u,
                            atp_ledger_digest(limit, ATPERSON_LEDGER_PAYLOAD_LIMIT),
                            ATPERSON_SCHEMA_VERSION, ATP_LEDGER_OUTCOME_LEARNED, limit,
                            ATPERSON_LEDGER_PAYLOAD_LIMIT, &id, &status) == ATP_LEDGER_NEW);
    CHECK(status == ATP_OK);
    CHECK(id == 4u);
    free(limit);

    atp_ledger_destroy(ledger);

    /* Reopen: every payload returns byte-for-byte. */
    ledger = open_or_fail(path);
    CHECK(atp_ledger_count(ledger) == 4u);
    unsigned char out[256];
    size_t out_len = 0u;
    CHECK(atp_ledger_entry_payload(ledger, 1u, out, sizeof(out), &out_len) == ATP_OK);
    CHECK(out_len == sizeof(binary));
    CHECK(memcmp(out, binary, out_len) == 0);
    CHECK(atp_ledger_entry_payload(ledger, 2u, out, sizeof(out), &out_len) == ATP_OK);
    CHECK(out_len == utf8_len);
    CHECK(memcmp(out, utf8, out_len) == 0);
    /* Empty: ATP_OK with zero length — absence, not error. */
    CHECK(atp_ledger_entry_payload(ledger, 3u, out, sizeof(out), &out_len) == ATP_OK);
    CHECK(out_len == 0u);
    /* At-limit payload survives restart intact. */
    unsigned char *big_out = malloc(ATPERSON_LEDGER_PAYLOAD_LIMIT);
    CHECK(big_out != NULL);
    CHECK(atp_ledger_entry_payload(ledger, 4u, big_out, ATPERSON_LEDGER_PAYLOAD_LIMIT,
                                   &out_len) == ATP_OK);
    CHECK(out_len == ATPERSON_LEDGER_PAYLOAD_LIMIT);
    CHECK(big_out[0] == 'y' && big_out[ATPERSON_LEDGER_PAYLOAD_LIMIT - 1u] == 'y');
    free(big_out);

    atp_ledger_destroy(ledger);
    remove_ledger_files(dir);
    return 0;
}

/* Corruption: a payload byte flipped on disk must never be returned as
 * data. The record CRC catches it at recovery, so the log is refused (or
 * self-healed to the last valid prefix when no marker exists). */
static int run_payload_corruption(const char *dir) {
    remove_ledger_files(dir);
    CHECK(atp_mkdir(dir) == 0);
    char path[1024];
    snprintf(path, sizeof(path), "%s/ledger.bin", dir);

    atp_ledger *ledger = open_or_fail(path);
    const char *text_a = "first payload";
    const char *text_b = "second payload";
    uint64_t id = 0u;
    atp_status status = ATP_OK;
    CHECK(atp_ledger_append(ledger, "at://c/1", "did:plc:c", 1u,
                            atp_ledger_digest(text_a, strlen(text_a)), ATPERSON_SCHEMA_VERSION,
                            ATP_LEDGER_OUTCOME_LEARNED, text_a, strlen(text_a), &id,
                            &status) == ATP_LEDGER_NEW);
    CHECK(atp_ledger_append(ledger, "at://c/2", "did:plc:c", 2u,
                            atp_ledger_digest(text_b, strlen(text_b)), ATPERSON_SCHEMA_VERSION,
                            ATP_LEDGER_OUTCOME_LEARNED, text_b, strlen(text_b), &id,
                            &status) == ATP_LEDGER_NEW);
    atp_ledger_destroy(ledger);

    /* Flip one payload byte inside the second entry's record. The record
     * framing is: 4-byte len, 4-byte CRC, 1-byte type, body. The body holds
     * entry fields then payload_len + payload; the payload sits at the very
     * end of the record. */
    FILE *log = fopen(path, "r+b");
    CHECK(log != NULL);
    CHECK(fseek(log, 0, SEEK_END) == 0);
    const long size = ftell(log);
    CHECK(size > 0);
    CHECK(fseek(log, size - 3, SEEK_SET) == 0);
    int byte = fgetc(log);
    CHECK(byte != EOF);
    CHECK(fseek(log, size - 3, SEEK_SET) == 0);
    CHECK(fputc(byte ^ 0x40, log) != EOF);
    CHECK(fclose(log) == 0);

    /* With a durable marker fencing the corrupted prefix, open refuses. */
    atp_status open_status = ATP_OK;
    atp_ledger *reopened = atp_ledger_open(path, &open_status);
    CHECK(reopened == NULL);
    CHECK(open_status == ATP_ERR_FORMAT);

    /* Without the marker, the longest valid prefix wins and the tail is
     * truncated away — the flipped record is dropped, the earlier entry
     * survives intact. */
    char off_path[1024];
    snprintf(off_path, sizeof(off_path), "%s.off", path);
    CHECK(remove(off_path) == 0);
    reopened = open_or_fail(path);
    CHECK(atp_ledger_count(reopened) == 1u);
    char out[64];
    size_t out_len = 0u;
    CHECK(atp_ledger_entry_payload(reopened, 1u, out, sizeof(out), &out_len) == ATP_OK);
    CHECK(out_len == strlen(text_a));
    CHECK(memcmp(out, text_a, out_len) == 0);
    CHECK(atp_ledger_entry_payload(reopened, 2u, out, sizeof(out), &out_len) ==
          ATP_ERR_NOT_FOUND);
    atp_ledger_destroy(reopened);

    remove_ledger_files(dir);
    return 0;
}

/* v1 -> current migration: a hand-built v1 log migrates on open. Entries and
 * flattened outcomes survive; payloads and context are honestly absent. */
static int run_migration(const char *dir) {
    remove_ledger_files(dir);
    CHECK(atp_mkdir(dir) == 0);
    char path[1024];
    snprintf(path, sizeof(path), "%s/ledger.bin", dir);

    /* Hand-build a v1 log. v1 framing is identical to current except the entry
     * body has no payload or context fields and the magics end in '1'. */
    FILE *log = fopen(path, "wb");
    CHECK(log != NULL);
    unsigned char header[12];
    header[0] = 'A'; header[1] = 'T'; header[2] = 'P'; header[3] = 'L';
    header[4] = 'D'; header[5] = 'G'; header[6] = '0'; header[7] = '1';
    store_u32_le(&header[8], 1u);
    CHECK(fwrite(header, 1u, sizeof(header), log) == sizeof(header));

    /* v1 entry body: id u64 | source_len u32 | source | author_len u32 |
     * author | observed_at u64 | digest u64 | schema u32 | outcome u8. */
    const char *source = "at://v1/1";
    const char *author = "did:plc:v1";
    const size_t source_len = strlen(source);
    const size_t author_len = strlen(author);
    unsigned char body[128];
    size_t pos = 0u;
    store_u64_le(&body[pos], 1u);
    pos += 8u;
    store_u32_le(&body[pos], (uint32_t)source_len);
    pos += 4u;
    memcpy(&body[pos], source, source_len);
    pos += source_len;
    store_u32_le(&body[pos], (uint32_t)author_len);
    pos += 4u;
    memcpy(&body[pos], author, author_len);
    pos += author_len;
    store_u64_le(&body[pos], 777u);
    pos += 8u;
    store_u64_le(&body[pos], 888u);
    pos += 8u;
    store_u32_le(&body[pos], ATPERSON_SCHEMA_VERSION);
    pos += 4u;
    body[pos] = (uint8_t)ATP_LEDGER_OUTCOME_PENDING;
    pos += 1u;

    unsigned char record[9u + 128];
    store_u32_le(&record[0], (uint32_t)pos);
    store_u32_le(&record[4], record_checksum(body, pos));
    record[8] = 1u; /* ATP_LEDGER_RECORD_ENTRY */
    memcpy(&record[9], body, pos);
    CHECK(fwrite(record, 1u, 9u + pos, log) == 9u + pos);
    CHECK(fclose(log) == 0);

    /* No marker: migration self-heals from the longest valid prefix. */
    atp_ledger *ledger = open_or_fail(path);
    CHECK(atp_ledger_count(ledger) == 1u);
    atp_ledger_entry entry = {0};
    CHECK(atp_ledger_entry_at(ledger, 0u, &entry) == ATP_OK);
    CHECK(entry.id == 1u);
    CHECK(strcmp(entry.source_id, source) == 0);
    CHECK(strcmp(entry.author_did, author) == 0);
    CHECK(entry.observed_at == 777u);
    CHECK(entry.content_digest == 888u);
    CHECK(entry.outcome == ATP_LEDGER_OUTCOME_PENDING);

    /* The v1 entry is payload-less: honest absence. Context is empty too:
     * v1 never carried it. */
    char out[64];
    size_t out_len = 0u;
    CHECK(atp_ledger_entry_payload(ledger, 1u, out, sizeof(out), &out_len) == ATP_OK);
    CHECK(out_len == 0u);
    atp_conversation_context context = {0};
    CHECK(atp_ledger_entry_context(ledger, 1u, &context) == ATP_OK);
    CHECK(context.reply_root_uri[0] == '\0');
    CHECK(context.reply_parent_uri[0] == '\0');
    CHECK(context.quote_uri[0] == '\0');

    /* Unknown ids are rejected, not silently zeroed. */
    CHECK(atp_ledger_entry_context(ledger, 99u, &context) == ATP_ERR_NOT_FOUND);

    /* The migrated log is current-format: appends with payloads work and
     * dedup against the migrated entry holds. */
    uint64_t id = 0u;
    atp_status status = ATP_OK;
    CHECK(atp_ledger_append(ledger, "at://v1/1", author, 777u, 888u, ATPERSON_SCHEMA_VERSION,
                            ATP_LEDGER_OUTCOME_PENDING, NULL, 0u, &id,
                            &status) == ATP_LEDGER_EXISTS_PENDING);
    const char *text = "post-migration payload";
    CHECK(atp_ledger_append(ledger, "at://v2/1", author, 999u,
                            atp_ledger_digest(text, strlen(text)), ATPERSON_SCHEMA_VERSION,
                            ATP_LEDGER_OUTCOME_LEARNED, text, strlen(text), &id,
                            &status) == ATP_LEDGER_NEW);
    CHECK(id == 2u);
    atp_ledger_destroy(ledger);

    /* Reopen: the migrated log is stable and the new payload survives. */
    ledger = open_or_fail(path);
    CHECK(atp_ledger_count(ledger) == 2u);
    CHECK(atp_ledger_entry_payload(ledger, 2u, out, sizeof(out), &out_len) == ATP_OK);
    CHECK(out_len == strlen(text));
    CHECK(memcmp(out, text, out_len) == 0);
    atp_ledger_destroy(ledger);

    remove_ledger_files(dir);
    return 0;
}

/* v2 -> current migration: a hand-built v2 log migrates on open. Entries,
 * flattened outcomes and retained payloads survive; context is honestly
 * absent because v2 never carried it. */
static int run_migration_v2(const char *dir) {
    remove_ledger_files(dir);
    CHECK(atp_mkdir(dir) == 0);
    char path[1024];
    snprintf(path, sizeof(path), "%s/ledger.bin", dir);

    /* Hand-build a v2 log: magics end in '2'. */
    FILE *log = fopen(path, "wb");
    CHECK(log != NULL);
    unsigned char header[12];
    header[0] = 'A'; header[1] = 'T'; header[2] = 'P'; header[3] = 'L';
    header[4] = 'D'; header[5] = 'G'; header[6] = '0'; header[7] = '2';
    store_u32_le(&header[8], 2u);
    CHECK(fwrite(header, 1u, sizeof(header), log) == sizeof(header));

    /* v2 entry body: v1 fields plus payload_len u32 | payload. */
    const char *source = "at://v2/1";
    const char *author = "did:plc:v2";
    const char *text = "retained v2 payload";
    const size_t source_len = strlen(source);
    const size_t author_len = strlen(author);
    const size_t text_len = strlen(text);
    unsigned char body[256];
    size_t pos = 0u;
    store_u64_le(&body[pos], 1u);
    pos += 8u;
    store_u32_le(&body[pos], (uint32_t)source_len);
    pos += 4u;
    memcpy(&body[pos], source, source_len);
    pos += source_len;
    store_u32_le(&body[pos], (uint32_t)author_len);
    pos += 4u;
    memcpy(&body[pos], author, author_len);
    pos += author_len;
    store_u64_le(&body[pos], 4242u);
    pos += 8u;
    store_u64_le(&body[pos], atp_ledger_digest(text, text_len));
    pos += 8u;
    store_u32_le(&body[pos], ATPERSON_SCHEMA_VERSION);
    pos += 4u;
    body[pos] = (uint8_t)ATP_LEDGER_OUTCOME_LEARNED;
    pos += 1u;
    store_u32_le(&body[pos], (uint32_t)text_len);
    pos += 4u;
    memcpy(&body[pos], text, text_len);
    pos += text_len;

    unsigned char record[9u + 256];
    store_u32_le(&record[0], (uint32_t)pos);
    store_u32_le(&record[4], record_checksum(body, pos));
    record[8] = 1u; /* ATP_LEDGER_RECORD_ENTRY */
    memcpy(&record[9], body, pos);
    CHECK(fwrite(record, 1u, 9u + pos, log) == 9u + pos);
    CHECK(fclose(log) == 0);

    atp_ledger *ledger = open_or_fail(path);
    CHECK(atp_ledger_count(ledger) == 1u);
    atp_ledger_entry entry = {0};
    CHECK(atp_ledger_entry_at(ledger, 0u, &entry) == ATP_OK);
    CHECK(entry.id == 1u);
    CHECK(strcmp(entry.source_id, source) == 0);
    CHECK(entry.observed_at == 4242u);
    CHECK(entry.outcome == ATP_LEDGER_OUTCOME_LEARNED);

    /* The v2 payload survives migration unchanged. */
    char out[64];
    size_t out_len = 0u;
    CHECK(atp_ledger_entry_payload(ledger, 1u, out, sizeof(out), &out_len) == ATP_OK);
    CHECK(out_len == text_len);
    CHECK(memcmp(out, text, text_len) == 0);

    /* Context is empty: v2 never carried it. */
    atp_conversation_context context = {0};
    CHECK(atp_ledger_entry_context(ledger, 1u, &context) == ATP_OK);
    CHECK(context.reply_root_uri[0] == '\0' && context.quote_uri[0] == '\0');
    atp_ledger_destroy(ledger);

    remove_ledger_files(dir);
    return 0;
}

/* Context round trip: append with and without conversational context, verify
 * read-back, restart persistence, overlong rejection, and that compaction
 * preserves context. */
static int run_context(const char *dir) {
    remove_ledger_files(dir);
    CHECK(atp_mkdir(dir) == 0);
    char path[1024];
    snprintf(path, sizeof(path), "%s/ledger.bin", dir);

    atp_ledger *ledger = open_or_fail(path);
    const char *text = "context round trip";
    atp_conversation_context context = {0};
    strcpy(context.reply_root_uri, "at://did:plc:a/app.bsky.feed.post/root");
    strcpy(context.reply_parent_uri, "at://did:plc:a/app.bsky.feed.post/parent");
    strcpy(context.quote_uri, "at://did:plc:b/app.bsky.feed.post/quote");
    uint64_t id = 0u;
    atp_status status = ATP_OK;
    CHECK(atp_ledger_append_with_context(ledger, "at://ctx/1", "did:plc:a", 10u,
                                         atp_ledger_digest(text, strlen(text)),
                                         ATPERSON_SCHEMA_VERSION,
                                         ATP_LEDGER_OUTCOME_LEARNED, text, strlen(text),
                                         &context, &id, &status) == ATP_LEDGER_NEW);
    CHECK(id == 1u);
    CHECK(atp_ledger_append(ledger, "at://ctx/2", "did:plc:a", 20u, 42u,
                            ATPERSON_SCHEMA_VERSION, ATP_LEDGER_OUTCOME_SKIPPED, NULL, 0u,
                            &id, &status) == ATP_LEDGER_NEW);

    /* Read-back in-process. */
    atp_conversation_context read = {0};
    CHECK(atp_ledger_entry_context(ledger, 1u, &read) == ATP_OK);
    CHECK(strcmp(read.reply_root_uri, context.reply_root_uri) == 0);
    CHECK(strcmp(read.reply_parent_uri, context.reply_parent_uri) == 0);
    CHECK(strcmp(read.quote_uri, context.quote_uri) == 0);
    /* The context-less append yields empty context, not stale values. */
    CHECK(atp_ledger_entry_context(ledger, 2u, &read) == ATP_OK);
    CHECK(read.reply_root_uri[0] == '\0' && read.quote_uri[0] == '\0');

    /* An unterminated URI is rejected before any mutation. */
    atp_conversation_context bad = {0};
    memset(bad.quote_uri, 'x', sizeof(bad.quote_uri));
    CHECK(atp_ledger_append_with_context(ledger, "at://ctx/bad", "did:plc:a", 30u, 43u,
                                         ATPERSON_SCHEMA_VERSION, ATP_LEDGER_OUTCOME_PENDING,
                                         NULL, 0u, &bad, &id, &status) == ATP_LEDGER_NOT_FOUND);
    CHECK(status == ATP_ERR_INVALID_ARGUMENT);
    CHECK(atp_ledger_count(ledger) == 2u);
    atp_ledger_destroy(ledger);

    /* Reopen: context survived the durable round trip. */
    ledger = open_or_fail(path);
    CHECK(atp_ledger_entry_context(ledger, 1u, &read) == ATP_OK);
    CHECK(strcmp(read.quote_uri, context.quote_uri) == 0);

    /* Compaction preserves context (metadata, not payload). */
    atp_compact_report report = {0};
    CHECK(atp_ledger_compact(ledger, &report) == ATP_OK);
    CHECK(atp_ledger_entry_context(ledger, 1u, &read) == ATP_OK);
    CHECK(strcmp(read.reply_root_uri, context.reply_root_uri) == 0);
    CHECK(strcmp(read.reply_parent_uri, context.reply_parent_uri) == 0);
    CHECK(strcmp(read.quote_uri, context.quote_uri) == 0);
    atp_ledger_destroy(ledger);

    /* And after reopening the compacted log. */
    ledger = open_or_fail(path);
    CHECK(atp_ledger_entry_context(ledger, 1u, &read) == ATP_OK);
    CHECK(strcmp(read.quote_uri, context.quote_uri) == 0);
    CHECK(atp_ledger_entry_context(ledger, 2u, &read) == ATP_OK);
    CHECK(read.quote_uri[0] == '\0');
    atp_ledger_destroy(ledger);

    remove_ledger_files(dir);
    return 0;
}

int main(int argc, char **argv) {
    if (argc != 3) {
        fprintf(stderr,
                "usage: %s <roundtrip|dedup-phase1|dedup-phase2|"
                "crash-safety|payloads|payload-corruption|migration|migration-v2|"
                "context> <dir>\n",
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
    if (strcmp(command, "payloads") == 0) {
        return run_payloads(dir);
    }
    if (strcmp(command, "payload-corruption") == 0) {
        return run_payload_corruption(dir);
    }
    if (strcmp(command, "migration") == 0) {
        return run_migration(dir);
    }
    if (strcmp(command, "migration-v2") == 0) {
        return run_migration_v2(dir);
    }
    if (strcmp(command, "context") == 0) {
        return run_context(dir);
    }
    fprintf(stderr, "unknown command: %s\n", command);
    return 2;
}
