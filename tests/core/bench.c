/*
 * Scale benchmarks (issue #9): deterministic synthetic histories at
 * small (1k), medium (10k), and large (50k) sizes.
 *
 * Measures observe throughput, association lookup, recall query,
 * snapshot save/load, and memory footprint estimate. Not part of the
 * default test run — run via `bench` suite or the bench target.
 *
 * Deterministic: fixed seeds, fixed vocabularies, no wall-clock inputs.
 * Timing is reported, never asserted, so CI variance cannot flake.
 */

/* clock_gettime is POSIX. glibc hides it under strict C23 (which defines
 * __STRICT_ANSI__), so request the POSIX API explicitly on non-Windows
 * targets; macOS exposes it by default. */
#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE) && !defined(_XOPEN_SOURCE)
#define _POSIX_C_SOURCE 200809L
#endif

#include "atperson/core.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#if defined(_WIN32)
#include <windows.h>
#endif

static const char *BENCH_SNAPSHOT = "atperson-bench-snapshot.bin";

typedef struct {
    const char *name;
    size_t observations; /* number of synthetic observations */
    size_t vocab;        /* distinct token vocabulary */
} bench_profile;

static const bench_profile PROFILES[] = {
    {"small", 1000u, 500u},
    {"medium", 10000u, 5000u},
    {"large", 50000u, 25000u},
};

static double now_seconds(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

/* Deterministic PRNG (xorshift64) so every run sees the same history. */
static uint64_t rng_state = 0x9E3779B97F4A7C15ull;

static uint32_t rng_next(void) {
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 7;
    rng_state ^= rng_state << 17;
    return (uint32_t)(rng_state >> 32);
}

/* Emit a synthetic observation: 2-8 tokens drawn from the profile's
 * vocabulary, skewed so a few tokens are very common (hub pressure). */
static size_t synth_text(char *out, size_t vocab) {
    static const char *SHAPES[] = {"%s %s", "%s %s %s", "%s %s %s %s", "%s %s %s %s %s"};
    const uint32_t shape = rng_next() % 4u;
    char tokens[5][24];
    for (uint32_t i = 0; i <= shape; ++i) {
        /* 1-in-8 tokens come from a tiny hub set; the rest from vocab. */
        const uint32_t draw = rng_next();
        const uint32_t id = (draw & 7u) == 0u ? (draw >> 8) % 16u : (draw >> 8) % (uint32_t)vocab;
        snprintf(tokens[i], sizeof(tokens[i]), "w%u", id);
    }
    return (size_t)snprintf(out, 128, SHAPES[shape], tokens[0], tokens[1], tokens[2],
                            tokens[3], tokens[4]);
}

static atp_graph *build_history(const bench_profile *p, atp_graph_stats *out_stats) {
    atp_graph_config config = atp_graph_default_config();
    config.seed = 42u;
    atp_graph *graph = atp_graph_create(&config);
    if (!graph) {
        return NULL;
    }

    rng_state = 0x9E3779B97F4A7C15ull; /* reset per profile */
    char text[128];
    char source[32];
    for (size_t i = 0u; i < p->observations; ++i) {
        synth_text(text, p->vocab);
        snprintf(source, sizeof(source), "at://bench/%zu", i % 64u);
        if (atp_graph_observe_with_memory(graph, text, source, "did:plc:bench", i * 1000ull,
                                          0u, ATPERSON_SCHEMA_VERSION, 0u, NULL) != ATP_OK) {
            atp_graph_destroy(graph);
            return NULL;
        }
    }
    *out_stats = atp_graph_get_stats(graph);
    return graph;
}

static void bench_lookup(atp_graph *graph, size_t vocab) {
    atp_association assoc[8];
    size_t found = 0u;
    char token[24];
    double t0 = now_seconds();
    const size_t queries = 5000u;
    for (size_t i = 0u; i < queries; ++i) {
        snprintf(token, sizeof(token), "w%zu", (i * 7919u) % vocab); /* prime stride */
        atp_graph_associations(graph, token, assoc, 8u, &found);
    }
    double dt = now_seconds() - t0;
    printf("  lookup: %5zu queries in %6.1fms (%.1f us/query)\n", queries, dt * 1e3,
           dt * 1e6 / (double)queries);
}

static void bench_recall(atp_graph *graph, size_t vocab) {
    char query[64];
    double t0 = now_seconds();
    const size_t queries = 500u;
    for (size_t i = 0u; i < queries; ++i) {
        snprintf(query, sizeof(query), "w%zu w%zu", (i * 7919u) % vocab,
                 (i * 104729u) % vocab);
        atp_graph_recall(graph, query, (uint64_t)i * 1000ull, NULL, NULL, NULL, 0u, NULL);
    }
    double dt = now_seconds() - t0;
    printf("  recall: %5zu queries in %6.1fms (%.1f us/query)\n", queries, dt * 1e3,
           dt * 1e6 / (double)queries);
}

static void bench_snapshot(atp_graph *graph, const char *name) {
    double t0 = now_seconds();
    const atp_status saved = atp_graph_save(graph, BENCH_SNAPSHOT);
    double save_dt = now_seconds() - t0;
    if (saved != ATP_OK) {
        printf("  snapshot: save FAILED (%s)\n", atp_status_string(saved));
        return;
    }

    t0 = now_seconds();
    atp_status status = ATP_OK;
    atp_graph *loaded = atp_graph_load(BENCH_SNAPSHOT, &status);
    double load_dt = now_seconds() - t0;
    remove(BENCH_SNAPSHOT);
    if (!loaded || status != ATP_OK) {
        printf("  snapshot: load FAILED (%s)\n", atp_status_string(status));
        if (loaded) {
            atp_graph_destroy(loaded);
        }
        return;
    }

    atp_graph_stats a = atp_graph_get_stats(graph);
    atp_graph_stats b = atp_graph_get_stats(loaded);
    printf("  snapshot: save %6.1fms load %6.1fms (nodes %zu->%zu, edges %zu->%zu, %s)\n",
           save_dt * 1e3, load_dt * 1e3, a.node_count, b.node_count, a.edge_count, b.edge_count,
           (a.node_count == b.node_count && a.edge_count == b.edge_count) ? "match" : "MISMATCH");
    atp_graph_destroy(loaded);
    (void)name;
}

static void bench_memory(const atp_graph_stats *stats) {
    /* Footprint estimate from the internal layouts (internal.h):
     * atp_node ~= 136B (token ptr, counts, familiarity, embedding[16])
     * plus token string; atp_edge ~= 32B. malloc overhead ~16B per
     * allocation on darwin/glibc. */
    const size_t per_node = 136u + 24u /* token avg */ + 16u;
    const size_t per_edge = 32u + 16u;
    const size_t node_bytes = stats->node_count * per_node;
    const size_t edge_bytes = stats->edge_count * per_edge;
    printf("  memory: ~%.1f MB (%zu nodes x %zuB + %zu edges x %zuB)\n",
           (double)(node_bytes + edge_bytes) / (1024.0 * 1024.0), stats->node_count, per_node,
           stats->edge_count, per_edge);
}

static void run_profile(const bench_profile *p) {
    printf("profile: %s (%zu observations, %zu vocab)\n", p->name, p->observations, p->vocab);

    atp_graph_stats stats = {0};
    double t0 = now_seconds();
    atp_graph *graph = build_history(p, &stats);
    double build_dt = now_seconds() - t0;
    if (!graph) {
        printf("  FAILED to build history\n");
        return;
    }
    printf("  observe: %6.1fms total (%.2f us/observation, %zu nodes, %zu edges)\n",
           build_dt * 1e3, build_dt * 1e6 / (double)p->observations, stats.node_count,
           stats.edge_count);

    bench_lookup(graph, p->vocab);
    bench_recall(graph, p->vocab);
    bench_snapshot(graph, p->name);
    bench_memory(&stats);

    atp_graph_destroy(graph);
}

int main(int argc, char **argv) {
    if (argc < 2 || strcmp(argv[1], "bench") != 0) {
        fprintf(stderr, "usage: %s bench\n", argv[0]);
        return 1;
    }

    for (size_t i = 0u; i < sizeof(PROFILES) / sizeof(PROFILES[0]); ++i) {
        run_profile(&PROFILES[i]);
    }
    puts("bench: ok");
    return 0;
}
