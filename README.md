# atperson

`atperson` is an experiment in building a persistent digital entity on the
[AT Protocol](https://atproto.com/) whose internal language model starts empty
and changes through experience rather than beginning with a hand-written
persona.

The project is deliberately split between **C23** and **C++23**:

- **C23 is authoritative for learned state.** It owns the language graph,
  vocabulary, trainable embeddings, neural association scorer, online updates,
  statistics, and snapshot persistence.
- **C++23 owns the application/runtime boundary.** It provides RAII around the
  C core, configuration, command-line orchestration, JSON extraction, and
  AT Protocol connectivity.
- **[wolfram](https://github.com/ewanc26/wolfram) owns AT Protocol mechanics.**
  atperson does not reimplement XRPC, sessions, identity, repositories, or
  Bluesky agent calls.

This is an early scaffold. It does **not** claim sentience or personhood, and it
does not yet autonomously post. The intended research direction is a durable
entity whose vocabulary, associations, memories, preferences, and eventually
behaviour emerge from accumulated interaction instead of a predefined
character prompt.

## What exists now

The first scaffold provides:

- an initially empty directed token graph;
- 16-dimensional trainable embeddings created only when a token is observed;
- a small online neural scorer trained from observed bigrams with negative
  sampling;
- edge strength, exposure counts, and hashed source provenance;
- a **durable observation ledger** (C23 append-only log) that keeps every
  observation fed to the core, with an in-memory unique `(source id + digest)`
  index rebuilt on open and a crash-safe offset marker written via
  temp-file + rename;
- versioned binary snapshots (v2 mirrors the ledger) containing the complete
  mutable learning state;
- a C++23 RAII wrapper around the C23 graph and the ledger;
- a Wolfram-backed read-only timeline ingestion path running through the
  ledger for cross-run deduplication;
- offline C and C++ tests including crash-safety and cross-process dedup.

The model starts with **zero words and zero relationships**. Neural weights have
small deterministic random initial values so learning can begin, but there is
no seeded vocabulary, personality, biography, ideology, or preference set.

## Architecture

```text
AT Protocol network
        |
        v
+---------------------------+
| C++23 runtime             |
| - Wolfram session         |
| - feed/event extraction   |
| - lifecycle/config        |
| - future action policy    |
+-------------+-------------+
              |
              | observations
              v
+---------------------------+
| C23 learning core         |
| - token graph             |
| - embeddings              |
| - online neural training  |
| - learned statistics      |
| - observation ledger      |
| - persistence             |
+-------------+-------------+
              |
              v
       model snapshot
```

See [`docs/architecture.md`](docs/architecture.md) for the invariants and
planned progression.

## Build

A normal build fetches the pinned Wolfram revision and builds the network
runtime:

```sh
cmake -S . -B build
cmake --build build -j
ctest --test-dir build --output-on-failure
```

For work on the language graph alone, skip network dependencies:

```sh
cmake -S . -B build-core -DATPERSON_BUILD_NETWORK=OFF
cmake --build build-core -j
ctest --test-dir build-core --output-on-failure
```

The Wolfram dependency is currently pinned to commit
`9e63f76ab0b4f97f2cb5c62a5d0129b3d9023917`.

## Runtime

State defaults to `.atperson/model.bin`; the observation ledger defaults to
`.atperson/ledger.bin`. Override with `ATPERSON_STATE` and `ATPERSON_LEDGER`.

```sh
./build/atperson stats
./build/atperson ingest "hello world" local:first-observation
./build/atperson assoc hello
```

To learn from the authenticated account's **public home timeline**, use an app
password:

```sh
export ATPERSON_IDENTIFIER="handle.example"
export ATPERSON_APP_PASSWORD="xxxx-xxxx-xxxx-xxxx"
export ATPERSON_SERVICE="https://bsky.social" # optional

./build/atperson sync 50
```

Credentials are read from environment variables rather than command-line
arguments. Do not commit them.

Each fetched post is recorded in the ledger before training, so a restarted
process cannot re-train on already-committed observations; empty posts are
recorded but skipped. `sync` is intentionally one-shot at this stage. A
continuous daemon comes after durable source deduplication (the ledger) is
paired with replay/unlearning semantics for the snapshot; the ledger's pending
-> committed outcome fencing is what a future ingestion loop will rely on.

## Current boundaries

The scaffold intentionally does not:

- read private messages or private data;
- autonomously like, follow, reply, repost, or publish;
- use an LLM as a hidden personality engine;
- seed a biography or opinions into the learning graph;
- treat a generated response as evidence of consciousness;
- pretend that source deletion/unlearning is solved.

Before autonomous output is enabled, the project needs managed long-term memory
(the next growth stage), replay/unlearning semantics over the observation
ledger, inspectable action selection, rate limiting, and a way to rebuild or
remove learned contributions when source material is withdrawn.

## Licence

AGPL-3.0. See [`LICENSE`](LICENSE).
