/* Ingestion-state format and sync-engine restart/failure semantics.
 *
 * Everything here runs offline: the page fetcher is a fixture, so the
 * restart, partial-page-failure, and cursor-reset behaviours are exercised
 * without a live network. */
#include "atperson/graph.hpp"
#include "atperson/ledger.hpp"
#include "ingestion/state.hpp"
#include "engine.hpp"

#include <cassert>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

std::filesystem::path scratch_dir(const char *tag) {
    const auto root = std::filesystem::temp_directory_path() /
                      std::filesystem::path("atperson-sync-" + std::string(tag));
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root);
    return root;
}

std::string read_file(const std::filesystem::path &path) {
    std::ifstream input(path, std::ios::binary);
    std::ostringstream buffer;
    buffer << input.rdbuf();
    return buffer.str();
}

void write_file(const std::filesystem::path &path, const std::string &content) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output << content;
}

atperson::SyncObservation obs(const std::string &uri, const std::string &text) {
    return atperson::SyncObservation{
        .text = text,
        .source_uri = uri,
        .author_did = "did:plc:author",
        .created_at = "2026-09-16T00:00:00Z",
    };
}

/* ---------------------------------------------------------------- */
/* Ingestion state format                                            */
/* ---------------------------------------------------------------- */

void test_missing_file_is_clean_initial_state() {
    const auto dir = scratch_dir("missing");
    const auto state = atperson::load_ingestion_state(dir / "state.json",
                                                      "https://bsky.social",
                                                      "did:plc:abc");
    assert(!state.catchup.active);
    assert(!state.catchup.cursor);
    assert(state.checkpoint.generation == 0u);
    assert(!state.checkpoint.saved_at);
    assert(state.checkpoint.pages_completed == 0u);
    assert(state.checkpoint.observations_seen == 0u);
    assert(state.source.kind == "atproto-timeline");
    assert(state.source.service == "https://bsky.social");
    assert(state.source.account_did == "did:plc:abc");
    assert(state.source.endpoint == "app.bsky.feed.getTimeline");
    assert(!state.source.algorithm);
}

void test_round_trip_is_exact() {
    const auto dir = scratch_dir("roundtrip");
    const auto path = dir / "state.json";

    atperson::IngestionState state =
        atperson::initial_ingestion_state("https://bsky.social/", "did:plc:abc");
    state.catchup.active = true;
    state.catchup.cursor = std::string("opaque-server-cursor");
    state.checkpoint.generation = 17u;
    state.checkpoint.saved_at = std::string("2026-09-16T02:45:00Z");
    state.checkpoint.pages_completed = 6u;
    state.checkpoint.observations_seen = 287u;

    atperson::save_ingestion_state(state, path);
    const auto loaded = atperson::load_ingestion_state(path, "https://bsky.social",
                                                       "did:plc:abc");
    assert(loaded.version == 1u);
    assert(loaded.source.kind == "atproto-timeline");
    assert(loaded.source.service == "https://bsky.social"); /* trailing / normalised */
    assert(loaded.source.account_did == "did:plc:abc");
    assert(loaded.source.endpoint == "app.bsky.feed.getTimeline");
    assert(!loaded.source.algorithm);
    assert(loaded.catchup.active);
    assert(loaded.catchup.cursor == std::optional<std::string>("opaque-server-cursor"));
    assert(loaded.checkpoint.generation == 17u);
    assert(loaded.checkpoint.pages_completed == 6u);
    assert(loaded.checkpoint.observations_seen == 287u);
    /* saved_at is stamped at write time, so the round trip carries the time
     * this save happened, not the fixture's old value. It must be a
     * non-empty RFC 3339 UTC timestamp. */
    assert(loaded.checkpoint.saved_at.has_value());
    assert(!loaded.checkpoint.saved_at->empty());
    assert(loaded.checkpoint.saved_at->size() == 20u);
    assert(loaded.checkpoint.saved_at->back() == 'Z');

    /* Serialise -> load -> serialise is byte-identical (deterministic field
     * order, no drift) now that both sides carry the same stamped value. */
    const auto first = atperson::serialise_ingestion_state(loaded);
    const auto second = atperson::serialise_ingestion_state(loaded);
    assert(first == second);
    (void)state;
}

void test_active_cursor_survives_restart() {
    const auto dir = scratch_dir("restart");
    const auto path = dir / "state.json";

    atperson::IngestionState state =
        atperson::initial_ingestion_state("https://bsky.social", "did:plc:abc");
    state.catchup.active = true;
    state.catchup.cursor = std::string("cursor-value");
    atperson::save_ingestion_state(state, path);

    const auto loaded = atperson::load_ingestion_state(path, "https://bsky.social",
                                                       "did:plc:abc");
    assert(loaded.catchup.active);
    assert(loaded.catchup.cursor == std::optional<std::string>("cursor-value"));
}

void test_inactive_state_requires_null_cursor() {
    const auto dir = scratch_dir("inactive");
    const auto path = dir / "state.json";

    atperson::IngestionState state =
        atperson::initial_ingestion_state("https://bsky.social", "did:plc:abc");
    atperson::save_ingestion_state(state, path);
    const auto loaded = atperson::load_ingestion_state(path, "https://bsky.social",
                                                       "did:plc:abc");
    assert(!loaded.catchup.active);
    assert(!loaded.catchup.cursor);

    /* active=false with a cursor present is impossible and must be refused. */
    write_file(path,
               "{\"format\":\"atperson-ingestion-state\",\"version\":1,"
               "\"source\":{\"kind\":\"atproto-timeline\",\"service\":\"https://bsky.social\","
               "\"account_did\":\"did:plc:abc\",\"endpoint\":\"app.bsky.feed.getTimeline\","
               "\"algorithm\":null},"
               "\"catchup\":{\"active\":false,\"cursor\":\"stale\"},"
               "\"checkpoint\":{\"generation\":1,\"saved_at\":null,"
               "\"pages_completed\":0,\"observations_seen\":0}}");
    bool threw = false;
    try {
        (void)atperson::load_ingestion_state(path, "https://bsky.social", "did:plc:abc");
    } catch (const atperson::IngestionStateError &) {
        threw = true;
    }
    assert(threw);

    /* active=true with an empty cursor is likewise refused. */
    write_file(path,
               "{\"format\":\"atperson-ingestion-state\",\"version\":1,"
               "\"source\":{\"kind\":\"atproto-timeline\",\"service\":\"https://bsky.social\","
               "\"account_did\":\"did:plc:abc\",\"endpoint\":\"app.bsky.feed.getTimeline\","
               "\"algorithm\":null},"
               "\"catchup\":{\"active\":true,\"cursor\":\"\"},"
               "\"checkpoint\":{\"generation\":1,\"saved_at\":null,"
               "\"pages_completed\":0,\"observations_seen\":0}}");
    threw = false;
    try {
        (void)atperson::load_ingestion_state(path, "https://bsky.social", "did:plc:abc");
    } catch (const atperson::IngestionStateError &) {
        threw = true;
    }
    assert(threw);
}

void test_malformed_json_is_explicit_error() {
    const auto dir = scratch_dir("malformed");
    const auto path = dir / "state.json";
    write_file(path, "{\"format\":\"atperson-ingestion-state\",\"version\":1");
    bool threw = false;
    try {
        (void)atperson::load_ingestion_state(path, "https://bsky.social", "did:plc:abc");
    } catch (const atperson::IngestionStateError &) {
        threw = true;
    }
    assert(threw);
}

void test_unsupported_version_is_explicit_error() {
    const auto dir = scratch_dir("version");
    const auto path = dir / "state.json";
    write_file(path,
               "{\"format\":\"atperson-ingestion-state\",\"version\":2,"
               "\"source\":{\"kind\":\"atproto-timeline\",\"service\":\"https://bsky.social\","
               "\"account_did\":\"did:plc:abc\",\"endpoint\":\"app.bsky.feed.getTimeline\","
               "\"algorithm\":null},"
               "\"catchup\":{\"active\":false,\"cursor\":null},"
               "\"checkpoint\":{\"generation\":0,\"saved_at\":null,"
               "\"pages_completed\":0,\"observations_seen\":0}}");
    bool threw = false;
    try {
        (void)atperson::load_ingestion_state(path, "https://bsky.social", "did:plc:abc");
    } catch (const atperson::IngestionStateError &error) {
        threw = true;
        assert(std::string(error.what()).find("unsupported version") != std::string::npos);
    }
    assert(threw);
}

void test_wrong_format_is_explicit_error() {
    const auto dir = scratch_dir("format");
    const auto path = dir / "state.json";
    write_file(path, "{\"format\":\"something-else\",\"version\":1}");
    bool threw = false;
    try {
        (void)atperson::load_ingestion_state(path, "https://bsky.social", "did:plc:abc");
    } catch (const atperson::IngestionStateError &) {
        threw = true;
    }
    assert(threw);
}

void test_source_mismatch_does_not_reuse_cursor() {
    const auto dir = scratch_dir("mismatch");
    const auto path = dir / "state.json";

    atperson::IngestionState state =
        atperson::initial_ingestion_state("https://old.example", "did:plc:one");
    state.catchup.active = true;
    state.catchup.cursor = std::string("old-account-cursor");
    atperson::save_ingestion_state(state, path);

    /* Different account DID: cursor must not survive. */
    auto loaded = atperson::load_ingestion_state(path, "https://old.example", "did:plc:two");
    assert(!loaded.catchup.active);
    assert(!loaded.catchup.cursor);
    assert(loaded.source.account_did == "did:plc:two");
    assert(loaded.checkpoint.generation == 0u);

    /* Different service: same. */
    loaded = atperson::load_ingestion_state(path, "https://new.example", "did:plc:one");
    assert(!loaded.catchup.active);
    assert(!loaded.catchup.cursor);

    /* Cosmetic trailing-slash difference must NOT invalidate. */
    atperson::save_ingestion_state(state, path);
    loaded = atperson::load_ingestion_state(path, "https://old.example/", "did:plc:one");
    assert(loaded.catchup.active);
    assert(loaded.catchup.cursor == std::optional<std::string>("old-account-cursor"));
}

void test_tmp_leftover_never_takes_precedence() {
    const auto dir = scratch_dir("tmp");
    const auto path = dir / "state.json";

    atperson::IngestionState committed =
        atperson::initial_ingestion_state("https://bsky.social", "did:plc:abc");
    committed.catchup.active = true;
    committed.catchup.cursor = std::string("committed-cursor");
    atperson::save_ingestion_state(committed, path);

    /* A leftover .tmp file (e.g. crash between write and rename) must be
     * ignored: the committed state file wins. */
    write_file(dir / "state.json.tmp",
               "{\"format\":\"atperson-ingestion-state\",\"version\":1,"
               "\"source\":{\"kind\":\"atproto-timeline\",\"service\":\"https://bsky.social\","
               "\"account_did\":\"did:plc:abc\",\"endpoint\":\"app.bsky.feed.getTimeline\","
               "\"algorithm\":null},"
               "\"catchup\":{\"active\":true,\"cursor\":\"half-written\"},"
               "\"checkpoint\":{\"generation\":99,\"saved_at\":null,"
               "\"pages_completed\":0,\"observations_seen\":0}}");

    const auto loaded = atperson::load_ingestion_state(path, "https://bsky.social",
                                                       "did:plc:abc");
    assert(loaded.catchup.cursor == std::optional<std::string>("committed-cursor"));
    assert(loaded.checkpoint.generation == 0u);
}

void test_reset_clears_cursor_only() {
    atperson::IngestionState state =
        atperson::initial_ingestion_state("https://bsky.social", "did:plc:abc");
    state.catchup.active = true;
    state.catchup.cursor = std::string("cursor");
    state.checkpoint.generation = 5u;
    state.checkpoint.pages_completed = 3u;
    state.checkpoint.observations_seen = 100u;

    atperson::reset_ingestion_state(state);
    assert(!state.catchup.active);
    assert(!state.catchup.cursor);
    assert(state.checkpoint.pages_completed == 0u);
    assert(state.checkpoint.observations_seen == 0u);
    /* generation is monotonic across resets */
    assert(state.checkpoint.generation == 5u);
    /* source identity survives a reset */
    assert(state.source.account_did == "did:plc:abc");
}

void test_jetstream_kind_initial_state() {
    const auto dir = scratch_dir("jetstream-initial");
    const auto state = atperson::load_ingestion_state(
        dir / "state.json", "https://bsky.social", "", atperson::kSourceKindJetstream);
    assert(!state.catchup.active);
    assert(!state.catchup.cursor);
    assert(state.source.kind == "atproto-jetstream");
    assert(state.source.service == "https://bsky.social");
    assert(state.source.account_did.empty());
    assert(state.source.endpoint == "com.atproto.sync.subscribeRepos");
}

void test_jetstream_kind_round_trip_with_empty_did() {
    const auto dir = scratch_dir("jetstream-roundtrip");
    const auto path = dir / "state.json";

    atperson::IngestionState state = atperson::initial_ingestion_state(
        "https://bsky.social", "", atperson::kSourceKindJetstream);
    state.catchup.active = true;
    state.catchup.cursor = std::string("the-jetstream-cursor-is-opaque");
    state.checkpoint.generation = 3u;
    state.checkpoint.pages_completed = 9u;
    state.checkpoint.observations_seen = 541u;

    atperson::save_ingestion_state(state, path);
    /* The empty account DID is legitimate for an unauthenticated feed and
     * must round-trip without being rejected as corruption. */
    const auto loaded = atperson::load_ingestion_state(
        path, "https://bsky.social", "", atperson::kSourceKindJetstream);
    assert(loaded.source.kind == "atproto-jetstream");
    assert(loaded.source.account_did.empty());
    assert(loaded.source.endpoint == "com.atproto.sync.subscribeRepos");
    assert(loaded.catchup.active);
    assert(loaded.catchup.cursor ==
           std::optional<std::string>("the-jetstream-cursor-is-opaque"));
    assert(loaded.checkpoint.pages_completed == 9u);
    assert(loaded.checkpoint.observations_seen == 541u);
}

void test_jetstream_endpoint_mismatch_does_not_reuse_cursor() {
    const auto dir = scratch_dir("jetstream-endpoint-mismatch");
    const auto path = dir / "jetstream-state.json";

    constexpr std::string_view first =
        "wss://jetstream.us-east.bsky.network/subscribe";
    constexpr std::string_view second =
        "wss://jetstream2.us-east.bsky.network/subscribe";

    atperson::IngestionState state = atperson::initial_ingestion_state(
        first, "", atperson::kSourceKindJetstream);
    state.catchup.active = true;
    state.catchup.cursor = std::string("1726200000123456");
    state.checkpoint.generation = 4u;
    atperson::save_ingestion_state(state, path);

    /* Same endpoint: the opaque server cursor is reusable across restart. */
    auto loaded = atperson::load_ingestion_state(
        path, first, "", atperson::kSourceKindJetstream);
    assert(loaded.catchup.active);
    assert(loaded.catchup.cursor ==
           std::optional<std::string>("1726200000123456"));

    /*
     * Different Jetstream server: never assume its cursor namespace is
     * interchangeable. Start clean; the shared observation ledger remains
     * responsible for duplicate suppression over any overlap.
     */
    loaded = atperson::load_ingestion_state(
        path, second, "", atperson::kSourceKindJetstream);
    assert(!loaded.catchup.active);
    assert(!loaded.catchup.cursor);
    assert(loaded.source.kind == "atproto-jetstream");
    assert(loaded.source.service == second);
    assert(loaded.source.account_did.empty());
    assert(loaded.checkpoint.generation == 0u);
}

void test_jetstream_kind_timeline_cursor_not_reusable() {
    const auto dir = scratch_dir("jetstream-kind-mismatch");
    const auto path = dir / "state.json";

    /* A timeline cursor must never be resumed by the Jetstream feed: the
     * cursors have different meaning and different endpoints. */
    atperson::IngestionState timeline =
        atperson::initial_ingestion_state("https://bsky.social", "did:plc:abc");
    timeline.catchup.active = true;
    timeline.catchup.cursor = std::string("app.bsky.feed.getTimeline-cursor");
    atperson::save_ingestion_state(timeline, path);

    const auto loaded = atperson::load_ingestion_state(
        path, "https://bsky.social", "", atperson::kSourceKindJetstream);
    assert(!loaded.catchup.active);
    assert(!loaded.catchup.cursor);
    assert(loaded.source.kind == "atproto-jetstream");
    assert(loaded.source.account_did.empty());
}

/* ---------------------------------------------------------------- */
/* Sync engine: restart and failure semantics (offline fixtures)      */
/* ---------------------------------------------------------------- */

/* Fixture feed: page 0 has two items and cursor c1, page 1 (from c1) has one
 * item and no cursor (exhausted). */
atperson::SyncPage fixture_page(const std::optional<std::string> &cursor) {
    if (!cursor) {
        return atperson::SyncPage{
            .items = {obs("at://fixture/p1", "alpha beta"), obs("at://fixture/p2", "gamma")},
            .next_cursor = std::string("c1"),
        };
    }
    return atperson::SyncPage{
        .items = {obs("at://fixture/p3", "delta")},
        .next_cursor = std::nullopt,
    };
}

void test_successful_page_checkpoints_next_cursor() {
    const auto dir = scratch_dir("page-checkpoint");
    atperson::LanguageGraph graph;
    atperson::Ledger ledger(dir / "ledger.bin");
    auto state = atperson::initial_ingestion_state("https://bsky.social", "did:plc:abc");

    atperson::SyncLimits limits;
    limits.max_pages = 1;
    const auto result = atperson::run_sync(graph, ledger, state, fixture_page, limits);

    assert(result.pages_completed == 1u);
    assert(result.observations_seen == 2u);
    assert(result.learned == 2u);
    assert(!result.exhausted);
    assert(state.catchup.active);
    assert(state.checkpoint.pages_completed == 1u);
    assert(state.checkpoint.observations_seen == 2u);

    /* Persist, then verify the cursor survives a "restart" (reload). */
    atperson::save_ingestion_state(state, dir / "state.json");
    const auto reloaded = atperson::load_ingestion_state(dir / "state.json",
                                                         "https://bsky.social",
                                                         "did:plc:abc");
    assert(reloaded.catchup.active);
    assert(reloaded.catchup.cursor == std::optional<std::string>("c1"));
}

void test_resumed_traversal_continues_from_cursor() {
    const auto dir = scratch_dir("resume");
    atperson::LanguageGraph graph;
    atperson::Ledger ledger(dir / "ledger.bin");
    auto state = atperson::initial_ingestion_state("https://bsky.social", "did:plc:abc");

    atperson::SyncLimits limits;
    limits.max_pages = 1;
    atperson::run_sync(graph, ledger, state, fixture_page, limits);
    assert(state.catchup.cursor == std::optional<std::string>("c1"));

    /* Restart: continue from c1, reach exhaustion, cursor must clear. */
    const auto result = atperson::run_sync(graph, ledger, state, fixture_page, limits);
    assert(result.pages_completed == 1u);
    assert(result.observations_seen == 1u);
    assert(result.learned == 1u);
    assert(result.exhausted);
    assert(!state.catchup.active);
    assert(!state.catchup.cursor);
    /* traversal counters accumulate across the whole catch-up */
    assert(state.checkpoint.pages_completed == 2u);
    assert(state.checkpoint.observations_seen == 3u);
}

void test_exhausted_timeline_clears_cursor() {
    const auto dir = scratch_dir("exhausted");
    atperson::LanguageGraph graph;
    atperson::Ledger ledger(dir / "ledger.bin");
    auto state = atperson::initial_ingestion_state("https://bsky.social", "did:plc:abc");

    atperson::SyncLimits limits;
    limits.max_pages = 5; /* more pages than the fixture has */
    const auto result = atperson::run_sync(graph, ledger, state, fixture_page, limits);
    assert(result.exhausted);
    assert(result.pages_completed == 2u);
    assert(!state.catchup.active);
    assert(!state.catchup.cursor);

    /* The next independent sync starts at the head again; ledger dedup
     * suppresses re-training. */
    const auto again = atperson::run_sync(graph, ledger, state, fixture_page, limits);
    assert(again.observations_seen == 3u);
    assert(again.learned == 0u);
    assert(again.duplicates == 3u);
    assert(again.exhausted);
    /* traversal counters reset for the new traversal: 2 pages, not 4 */
    assert(state.checkpoint.pages_completed == 2u);
    assert(state.checkpoint.observations_seen == 3u);
}

void test_partial_page_failure_keeps_old_cursor() {
    const auto dir = scratch_dir("partial-failure");
    atperson::LanguageGraph graph;
    atperson::Ledger ledger(dir / "ledger.bin");
    auto state = atperson::initial_ingestion_state("https://bsky.social", "did:plc:abc");

    /* First run: one page completes, cursor c1 checkpointed and persisted. */
    atperson::SyncLimits limits;
    limits.max_pages = 1;
    atperson::run_sync(graph, ledger, state, fixture_page, limits);
    assert(state.catchup.cursor == std::optional<std::string>("c1"));
    atperson::save_ingestion_state(state, dir / "state.json");

    /* Second run: the transport fails before any page is returned. The
     * in-memory cursor must remain c1 and the persisted file must remain
     * untouched — a failure anywhere in run_sync must not advance the
     * cursor past observations that were not processed. */
    bool threw = false;
    try {
        const auto always_fail = [](const std::optional<std::string> &) -> atperson::SyncPage {
            throw std::runtime_error("transport failure");
        };
        atperson::run_sync(graph, ledger, state, always_fail, limits);
    } catch (const std::runtime_error &) {
        threw = true;
    }
    assert(threw);
    assert(state.catchup.cursor == std::optional<std::string>("c1"));
    const auto persisted = atperson::load_ingestion_state(dir / "state.json",
                                                          "https://bsky.social",
                                                          "did:plc:abc");
    assert(persisted.catchup.cursor == std::optional<std::string>("c1"));
}

void test_refetch_after_failure_deduplicates_via_ledger() {
    const auto dir = scratch_dir("refetch-dedup");
    atperson::LanguageGraph graph;
    atperson::Ledger ledger(dir / "ledger.bin");
    auto state = atperson::initial_ingestion_state("https://bsky.social", "did:plc:abc");

    /* Page 1 processes fully; page 2's fetch fails. The run aborts without
     * advancing the checkpoint — a failed run leaves the state exactly as
     * it was, so the persisted cursor can never move past observations
     * that were not fully handled. */
    atperson::SyncLimits limits;
    limits.max_pages = 2;

    const auto feed = [](const std::optional<std::string> &cursor) -> atperson::SyncPage {
        if (!cursor) {
            return atperson::SyncPage{
                .items = {obs("at://fixture/q1", "alpha beta")},
                .next_cursor = std::string("q1"),
            };
        }
        if (*cursor == "q1") {
            throw std::runtime_error("transport failure on page 2");
        }
        return atperson::SyncPage{.items = {}, .next_cursor = std::nullopt};
    };

    bool threw = false;
    try {
        atperson::run_sync(graph, ledger, state, feed, limits);
    } catch (const std::runtime_error &) {
        threw = true;
    }
    assert(threw);
    /* The failed run advanced nothing: no cursor, no counted pages. Page 1's
     * observations are durably committed in the ledger regardless. */
    assert(!state.catchup.active);
    assert(!state.catchup.cursor);
    assert(state.checkpoint.pages_completed == 0u);
    assert(ledger.count() == 1u);

    /* Restart from the head: page 1 is refetched, its committed item is
     * suppressed by ledger dedup, and the traversal continues to
     * exhaustion. Refetch + deduplicate, never skip. */
    const auto recovered = [](const std::optional<std::string> &cursor) -> atperson::SyncPage {
        if (!cursor) {
            return atperson::SyncPage{
                .items = {obs("at://fixture/q1", "alpha beta")},
                .next_cursor = std::string("q1"),
            };
        }
        return atperson::SyncPage{
            .items = {obs("at://fixture/q2", "gamma")},
            .next_cursor = std::nullopt,
        };
    };
    const auto result = atperson::run_sync(graph, ledger, state, recovered, limits);
    assert(result.exhausted);
    assert(result.observations_seen == 2u);
    assert(result.duplicates == 1u);
    assert(result.learned == 1u);
    assert(!state.catchup.active);
    assert(ledger.count() == 2u);
}

void test_mid_page_processing_failure_does_not_advance_cursor() {
    const auto dir = scratch_dir("mid-page");
    atperson::LanguageGraph graph;
    atperson::Ledger ledger(dir / "ledger.bin");
    auto state = atperson::initial_ingestion_state("https://bsky.social", "did:plc:abc");

    /* A page whose second item is unprocessable (empty source URI makes the
     * ledger append fail) must abort the run without checkpointing the
     * page's next cursor. On restart the same page is refetched; the first
     * item is already committed and is suppressed by ledger dedup. */
    const auto feed = [](const std::optional<std::string> &) -> atperson::SyncPage {
        return atperson::SyncPage{
            .items = {obs("at://fixture/r1", "alpha beta"),
                      obs("", "this item has no source uri")},
            .next_cursor = std::string("r-next"),
        };
    };

    atperson::SyncLimits limits;
    limits.max_pages = 1;
    bool threw = false;
    try {
        atperson::run_sync(graph, ledger, state, feed, limits);
    } catch (const std::runtime_error &) {
        threw = true;
    }
    assert(threw);
    /* Cursor not advanced: still inactive (nothing checkpointed). */
    assert(!state.catchup.active);
    assert(state.checkpoint.pages_completed == 0u);

    /* Restart with a healthy feed: r1 must be a duplicate now. */
    const auto healthy = [](const std::optional<std::string> &) -> atperson::SyncPage {
        return atperson::SyncPage{
            .items = {obs("at://fixture/r1", "alpha beta"), obs("at://fixture/r2", "gamma")},
            .next_cursor = std::nullopt,
        };
    };
    const auto result = atperson::run_sync(graph, ledger, state, healthy, limits);
    assert(result.exhausted);
    assert(result.duplicates == 1u);
    assert(result.learned == 1u);
}

void test_state_operations_never_touch_learned_state() {
    const auto dir = scratch_dir("no-model-mutation");
    atperson::LanguageGraph graph;
    graph.observe("alpha beta gamma", "at://seed/1");
    const auto before = graph.stats();

    auto state = atperson::initial_ingestion_state("https://bsky.social", "did:plc:abc");
    state.catchup.active = true;
    state.catchup.cursor = std::string("c");
    atperson::save_ingestion_state(state, dir / "state.json");

    const auto loaded = atperson::load_ingestion_state(dir / "state.json",
                                                       "https://bsky.social",
                                                       "did:plc:abc");
    assert(loaded.catchup.active);
    atperson::reset_ingestion_state(state);
    const auto text = atperson::serialise_ingestion_state(state);

    const auto after = graph.stats();
    assert(after.node_count == before.node_count);
    assert(after.edge_count == before.edge_count);
    assert(after.training_steps == before.training_steps);
    assert(after.observations == before.observations);
    (void)text;
}

void test_observation_budget_checkpoints_mid_traversal() {
    const auto dir = scratch_dir("budget");
    atperson::LanguageGraph graph;
    atperson::Ledger ledger(dir / "ledger.bin");
    auto state = atperson::initial_ingestion_state("https://bsky.social", "did:plc:abc");

    atperson::SyncLimits limits;
    limits.max_pages = 10;
    limits.max_observations = 2u; /* stop after page 1 */
    const auto result = atperson::run_sync(graph, ledger, state, fixture_page, limits);
    assert(result.pages_completed == 1u);
    assert(!result.exhausted);
    assert(state.catchup.active);
    assert(state.catchup.cursor == std::optional<std::string>("c1"));
}

/* ---------------------------------------------------------------- */
/* Ingestion policy integration                                      */
/* ---------------------------------------------------------------- */

/* A policy-skipped item: empty text plus a skip reason, as the network
 * client produces them. */
atperson::SyncObservation skipped_obs(const std::string &uri,
                                      atperson::PolicyReason reason) {
    return atperson::SyncObservation{
        .text = "",
        .source_uri = uri,
        .author_did = "did:plc:author",
        .created_at = "2026-09-16T00:00:00Z",
        .policy_reason = reason,
    };
}

void test_policy_skipped_items_are_ledgered_not_trained() {
    const auto dir = scratch_dir("policy-skip");
    atperson::LanguageGraph graph;
    atperson::Ledger ledger(dir / "ledger.bin");
    auto state = atperson::initial_ingestion_state("https://bsky.social", "did:plc:abc");

    const auto feed = [](const std::optional<std::string> &) -> atperson::SyncPage {
        return atperson::SyncPage{
            .items = {
                obs("at://fixture/learn", "learnable text"),
                skipped_obs("at://fixture/self", atperson::PolicyReason::SelfAuthored),
                skipped_obs("at://fixture/blocked", atperson::PolicyReason::ViewerBlocked),
                skipped_obs("at://fixture/muted", atperson::PolicyReason::ViewerMuted),
                skipped_obs("at://fixture/empty", atperson::PolicyReason::EmptyText),
            },
            .next_cursor = std::nullopt,
        };
    };

    atperson::SyncLimits limits;
    limits.max_pages = 1;
    const auto result = atperson::run_sync(graph, ledger, state, feed, limits);

    /* Every item was observed and ledgered; only one was learned. */
    assert(result.observations_seen == 5u);
    assert(result.learned == 1u);
    assert(result.skipped == 4u);
    assert(result.duplicates == 0u);
    assert(ledger.count() == 5u);

    /* The ledger distinguishes skipped from learned, so "observed but not
     * learned" stays distinguishable from "never fetched". */
    const auto entries = ledger.entries();
    assert(entries.size() == 5u);
    for (const auto &entry : entries) {
        const bool learned_uri = std::string_view(entry.source_id) == "at://fixture/learn";
        assert(entry.outcome == (learned_uri ? ATP_LEDGER_OUTCOME_LEARNED
                                            : ATP_LEDGER_OUTCOME_SKIPPED));
    }
}

void test_policy_skipped_items_deduplicate_across_runs() {
    const auto dir = scratch_dir("policy-dedup");
    atperson::LanguageGraph graph;
    atperson::Ledger ledger(dir / "ledger.bin");
    auto state = atperson::initial_ingestion_state("https://bsky.social", "did:plc:abc");

    const auto feed = [](const std::optional<std::string> &) -> atperson::SyncPage {
        return atperson::SyncPage{
            .items = {skipped_obs("at://fixture/self", atperson::PolicyReason::SelfAuthored)},
            .next_cursor = std::nullopt,
        };
    };

    atperson::SyncLimits limits;
    limits.max_pages = 1;
    const auto first = atperson::run_sync(graph, ledger, state, feed, limits);
    assert(first.skipped == 1u);
    assert(ledger.count() == 1u);

    /* A replay of the same timeline: the skipped item is a duplicate, not a
     * fresh skip. Replaying the timeline is idempotent for policy-skipped
     * items too. */
    const auto second = atperson::run_sync(graph, ledger, state, feed, limits);
    assert(second.observations_seen == 1u);
    assert(second.duplicates == 1u);
    assert(second.skipped == 0u);
    assert(ledger.count() == 1u);
}

void test_quote_reason_is_learned() {
    const auto dir = scratch_dir("policy-quote");
    atperson::LanguageGraph graph;
    atperson::Ledger ledger(dir / "ledger.bin");
    auto state = atperson::initial_ingestion_state("https://bsky.social", "did:plc:abc");

    atperson::SyncObservation quote = obs("at://fixture/quote", "my own quote words");
    quote.policy_reason = atperson::PolicyReason::Quote;
    quote.context.quote_uri = "at://did:plc:quoted/app.bsky.feed.post/1";

    const auto feed = [&](const std::optional<std::string> &) -> atperson::SyncPage {
        return atperson::SyncPage{
            .items = {quote},
            .next_cursor = std::nullopt,
        };
    };

    atperson::SyncLimits limits;
    limits.max_pages = 1;
    const auto result = atperson::run_sync(graph, ledger, state, feed, limits);
    assert(result.learned == 1u);
    assert(result.skipped == 0u);
    assert(ledger.count() == 1u);
    assert(ledger.entries().front().outcome == ATP_LEDGER_OUTCOME_LEARNED);
}

void test_reposts_and_replies_are_learned_with_reason() {
    const auto dir = scratch_dir("policy-eligible-tags");
    atperson::LanguageGraph graph;
    atperson::Ledger ledger(dir / "ledger.bin");
    auto state = atperson::initial_ingestion_state("https://bsky.social", "did:plc:abc");

    atperson::SyncObservation repost = obs("at://fixture/repost", "reposted text");
    repost.policy_reason = atperson::PolicyReason::Repost;
    atperson::SyncObservation reply = obs("at://fixture/reply", "reply text");
    reply.policy_reason = atperson::PolicyReason::Reply;

    const auto feed = [&](const std::optional<std::string> &) -> atperson::SyncPage {
        return atperson::SyncPage{
            .items = {repost, reply},
            .next_cursor = std::nullopt,
        };
    };

    atperson::SyncLimits limits;
    limits.max_pages = 1;
    const auto result = atperson::run_sync(graph, ledger, state, feed, limits);
    assert(result.learned == 2u);
    assert(result.skipped == 0u);
    assert(ledger.count() == 2u);

    const auto entries = ledger.entries();
    for (const auto &entry : entries) {
        assert(entry.outcome == ATP_LEDGER_OUTCOME_LEARNED);
    }
}

} // namespace

int main() {
    test_missing_file_is_clean_initial_state();
    test_round_trip_is_exact();
    test_active_cursor_survives_restart();
    test_inactive_state_requires_null_cursor();
    test_malformed_json_is_explicit_error();
    test_unsupported_version_is_explicit_error();
    test_wrong_format_is_explicit_error();
    test_source_mismatch_does_not_reuse_cursor();
    test_tmp_leftover_never_takes_precedence();
    test_reset_clears_cursor_only();

    test_jetstream_kind_initial_state();
    test_jetstream_kind_round_trip_with_empty_did();
    test_jetstream_endpoint_mismatch_does_not_reuse_cursor();
    test_jetstream_kind_timeline_cursor_not_reusable();

    test_successful_page_checkpoints_next_cursor();
    test_resumed_traversal_continues_from_cursor();
    test_exhausted_timeline_clears_cursor();
    test_partial_page_failure_keeps_old_cursor();
    test_refetch_after_failure_deduplicates_via_ledger();
    test_mid_page_processing_failure_does_not_advance_cursor();
    test_state_operations_never_touch_learned_state();
    test_observation_budget_checkpoints_mid_traversal();

    test_policy_skipped_items_are_ledgered_not_trained();
    test_policy_skipped_items_deduplicate_across_runs();
    test_quote_reason_is_learned();
    test_reposts_and_replies_are_learned_with_reason();

    std::printf("sync tests passed\n");
    return 0;
}
