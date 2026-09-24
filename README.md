# ATperson

`ATperson` is an experiment in building a persistent digital entity on the [AT Protocol](https://atproto.com/) that starts with effectively nothing and develops its own learned state through experience.

The point is not to wrap an LLM in a biography and call it a person. `ATperson` starts without a seeded vocabulary, personality, ideology, preferences, opinions or life story. What it knows has to come from observations, and the state produced by those observations has to be durable, inspectable and reconstructable.

This is experimental software. `ATperson` does **not** claim sentience or personhood, and autonomous network behaviour is deliberately still gated behind explicit policy and operator control.

## Design

The codebase has a strict C23/C++23 split:

- **C23 owns learned state.** The language graph, trainable embeddings, online learning, episodic memory, familiarity, valence, action scoring, observation ledger and snapshot persistence live here.
- **C++23 owns orchestration.** It handles lifecycle, configuration, RAII wrappers, the CLI, record extraction and the runtime around the core.
- **[Wolfram](https://github.com/ewanc26/wolfram) owns AT Protocol mechanics.** Sessions, XRPC, identity and repository operations are dependencies rather than being reimplemented here.

The rule I want to keep is simple: **C++ can orchestrate the entity, but it must not quietly become the authority for what the entity has learned.** Learned state and the evidence behind it stay in the C core.

```text
AT Protocol network
        |
        v
+---------------------------+
| C++23 runtime             |
| - Wolfram session         |
| - feed/record extraction  |
| - lifecycle/config        |
| - outbound control        |
+-------------+-------------+
              |
              | observations / actions
              v
+---------------------------+
| C23 learning core         |
| - token graph             |
| - embeddings              |
| - neural training         |
| - observation ledger      |
| - episodic memory         |
| - familiarity + valence   |
| - action scoring          |
| - persistence             |
+-------------+-------------+
              |
              v
        durable state
```

See [`docs/architecture.md`](docs/architecture.md) for the deeper architectural invariants and persistence model.

The OAuth/loopback integration contract and repository-scope safety rules are
documented in [`docs/oauth.md`](docs/oauth.md). OAuth session material is
runtime credential state, not learned experience, and must remain outside the
model snapshot and observation ledger.

## Current state

The project is already beyond the initial scaffold. The important pieces currently look like this:

| Area | State | Notes |
| --- | --- | --- |
| Language graph | Implemented | Empty-start vocabulary, directed associations, persisted variable-width embeddings, online neural scoring and negative sampling |
| Observation ledger | Implemented | Crash-safe append-only log with provenance, outcomes, canonical payloads and restart-safe deduplication |
| Episodic memory | Implemented | Source-linked memories, weighted token summaries, recall accounting and deterministic eviction |
| Familiarity | Implemented | Exposure-derived per-token familiarity |
| Valence | Implemented | Explicit experience-derived values in `[-1, 1]`; exposure alone never changes valence |
| Action model | First pass implemented | Deterministic, inspectable continuation candidates derived from learned state |
| Deterministic replay | Implemented | Learned state can be rebuilt from the ledger alone |
| Withdrawal / unlearning | Implemented | Sources can be durably withdrawn and excluded on rebuild |
| Schema compatibility | Implemented | Replay refuses incompatible learning schemas instead of silently reinterpreting them |
| Ledger compaction | Implemented | Final outcomes are flattened without changing rebuild semantics |
| Unicode tokenisation | Implemented | Schema-versioned `utf8proc` tokenisation shared across learning, recall and lookup |
| Growth limits | Implemented | Bounded node/edge growth with O(1) indexes and capacity rejection |
| Dynamic resource policy | Implemented | Runtime budgets respond to CPU, RAM, filesystem headroom and Linux cgroups |
| Long-running runtime | Implemented | `atperson daemon` performs bounded repeated sync cycles with backoff and graceful shutdown |
| Jetstream ingestion | Implemented | Unauthenticated public backfill via `atperson jetstream`, with zstd dictionary-compressed binary frames |
| Outbound policy | Implemented | Default-deny policy, durable rate budgets, duplicate suppression and inspectable decisions |
| Outbound execution | Operator-led | Approved frozen post/reply actions can be published through Wolfram with audit logging and idempotent record keys |
| Action/outcome journal | Implemented | Outbound attempts and outcomes are durable and replayable into valence |
| Container deployment | Implemented | Multi-stage Docker build and Docker Compose setup |
| Agent loop | Contract documented | Perception → decision → policy → control → execution → journal boundaries are defined; no autonomous scheduler is enabled |
| Autonomous social behaviour | Not enabled | Autonomous posting, replies, likes, follows, reposts, DMs and moderation remain fail-closed |

The model begins with **zero words and zero relationships**. Neural parameters have small deterministic initial values so training can start, but there is no seeded vocabulary, biography, ideology, personality or preference set.

## Why the ledger matters

The observation ledger is the authority for what external material has actually been committed. Learning happens on top of it, not instead of it.

That gives `ATperson` a few properties I care about:

- a crash does not silently train the same committed post twice;
- learned state can be rebuilt from retained observations;
- withdrawn material can be excluded and the state reconstructed as if it had never been learned;
- schema changes can fail explicitly instead of corrupting old meaning;
- episodic memories retain a route back to their source evidence;
- skipped material remains distinguishable from material that was never seen.

Snapshots remain generation-aware rather than being upgraded gratuitously. Legacy graphs still write **v5**; variable-topology generations that have never expanded write **v6**; and a generation with deterministic neural expansion history writes **v7**. v7 adds the ordered migration records (algorithm version, seed, source/target architecture and ledger boundary) required to reproduce topology changes during rebuild. v4/v5 generations remain readable at the legacy topology, and existing v6 generations remain readable unchanged. The standalone ledger remains the durable observation authority.

## Learning and memory

The C23 core currently provides:

- an initially empty directed token graph;
- trainable embeddings whose persisted width is selected when a model generation is first created;
- a persisted variable-shape online neural scorer trained from observed bigrams with negative sampling;
- learned edge strength, exposure counts and hashed source provenance;
- source-linked episodic memory with bounded deterministic retention;
- per-token familiarity learned from repeated exposure;
- per-token valence updated only by explicit events;
- durable conversation context for replies and quotes without training on quoted text;
- deterministic, read-only action candidate scoring;
- versioned portable snapshots and deterministic replay state.

The C++23 layer wraps that core and provides the CLI/runtime, configuration and Wolfram-backed AT Protocol ingestion and publishing paths.

AT Protocol literacy is a hard requirement, not an incidental side effect of
reading posts. The required protocol concepts, evidence provenance, replay
rules and capability gates are defined in [`docs/protocol-learning.md`](docs/protocol-learning.md).

More detail lives in:

- [`docs/valence.md`](docs/valence.md)
- [`docs/conversation-context.md`](docs/conversation-context.md)
- [`docs/action-inspection.md`](docs/action-inspection.md)
- [`docs/action-journal.md`](docs/action-journal.md)
- [`docs/jetstream.md`](docs/jetstream.md)

## `ATperson` vs `digital-person`

[`ewanc26/digital-person`](https://github.com/ewanc26/digital-person) explores a similar broad idea from the opposite direction.

| | `ATperson` | `digital-person` |
| --- | --- | --- |
| Starting point | Empty learner | Authored persona |
| Persona | Not seeded | Defined in files |
| Voice | Emergent work; no hidden prompt persona | Explicitly described |
| Memory | Replayable learned state + episodic memory | Letta memory blocks |
| Learning | Online learning from observations | Behaviour primarily shaped by authored persona and agent context |
| Core languages | C23 + C++23 | Python |
| AT Protocol | Wolfram-backed runtime | Platform adapters |
| Goal | Grow a persistent learned entity | Run a persistent authored digital person |

The short version is that `digital-person` is a **persona to run**, while `ATperson` is a **learner to grow**. They are related projects, but they are not interchangeable.

## Resource policy

`ATperson` derives runtime limits from the machine or container it is actually running on. Resource policy does not become learned state and is never persisted as personality or preference.

`ATperson resources` reports the detected limits and derived budget. The runtime considers:

- effective CPU capacity, including fractional quotas;
- total and currently available memory;
- free space on every filesystem that can receive durable state;
- Linux cgroup v1/v2 CPU and memory limits when they are tighter than the host.

These inputs affect graph growth headroom, write safety reserves, sync page size and bounded per-run work. They do **not** alter tokenisation, learning equations, familiarity semantics, planner scoring, replay ordering or schema compatibility.

See [`docs/resources.md`](docs/resources.md).

## Build

A normal build fetches the pinned Wolfram revision and builds the full runtime:

```sh
cmake -S . -B build
cmake --build build -j
ctest --test-dir build --output-on-failure
```

For core-only work without network dependencies:

```sh
cmake -S . -B build-core -DATPERSON_BUILD_NETWORK=OFF
cmake --build build-core -j
ctest --test-dir build-core --output-on-failure
```

The project uses strict C23 and C++23. Unix builds explicitly request POSIX.1-2008 for the durability APIs rather than relying on GNU language extensions.

GitHub Actions covers Linux GCC, Linux Clang, macOS Apple Clang, ASan/UBSan and the Wolfram-backed network build. See [`docs/ci-matrix.md`](docs/ci-matrix.md).

## Runtime execution policy

The persisted neural architecture is not the same thing as the resources used to execute it. `ATperson resources` reports a separate runtime neural execution policy derived from the CPU and memory available to the current process.

Policy v1 uses the portable deterministic CPU backend, keeps the C23 learner under one owner thread, and adapts surrounding worker allowance, staged observation work and transient workspace to current headroom. A weaker host can therefore reduce throughput without silently shrinking the learned model. Future SIMD or accelerator backends must define their replay/numeric compatibility before they can be selected.

## Runtime

By default, state lives under:

```text
~/.ewanc26/atperson/
```

The main files are the model snapshot (`model.bin`), observation ledger (`ledger.bin`) and ingestion cursor (`ingestion-state.json`). The whole directory can be moved with `ATPERSON_HOME`; individual paths can also be overridden.

On first run, `ATperson` creates the data directory and a local `.env` template without overwriting existing files.

For authenticated timeline ingestion:

```sh
export ATPERSON_IDENTIFIER="handle.example"
export ATPERSON_APP_PASSWORD="xxxx-xxxx-xxxx-xxxx"
export ATPERSON_SERVICE="https://bsky.social" # optional

./build/atperson sync 5
```

Credentials are read from environment variables, not command-line arguments. Do not commit them.

A few useful local commands:

```sh
./build/atperson stats
./build/atperson resources
./build/atperson ingest "hello world" local:first-observation
./build/atperson ingest-file ./notes.txt
./build/atperson assoc hello
./build/atperson candidates "hello world" 10
./build/atperson familiarity hello
./build/atperson recall "hello world" 5
```

### Continuous ingestion

```sh
./build/atperson daemon
./build/atperson daemon 5
```

The daemon uses the same ledger and cursor as `sync`, holds the writer lock for its lifetime, re-reads operator control each cycle and backs off on transport failures rather than spinning.

See [`docs/daemon.md`](docs/daemon.md).

### Neural capacity expansion

A stronger host can recommend a larger persisted neural topology without changing the model automatically:

```sh
./build/atperson neural status
```

If the proposal is monotonic and fits current safe memory headroom, expansion is an explicit operator action:

```sh
./build/atperson neural expand
```

The command takes the state writer lock, binds migration v1 to the current durable ledger boundary, expands a separately loaded candidate deterministically, and atomically replaces the snapshot only after the complete v7 generation is safely written. Smaller or incompatible recommendations are refused; merely moving the model to another machine never reshapes it.

### Rebuild

Learned state can be reconstructed from the observation ledger without network access or an existing snapshot:

```sh
./build/atperson rebuild
```

Replay uses the same observation path as live ingestion. For an expanded v7 generation, rebuild also replays the persisted neural migrations at their exact ledger boundaries rather than training the whole history at the final topology. The replacement snapshot is written atomically, and incompatible, incomplete or discontinuous replay metadata fails instead of quietly producing a different model.

### Withdrawal

Observations can be durably excluded from future learned state:

```sh
./build/atperson withdraw id 42
./build/atperson withdraw source at://did:plc:example/app.bsky.feed.post/abc
./build/atperson withdraw author did:plc:example
```

Withdrawal is append-only and idempotent. It does not mutate the live graph immediately; `atperson rebuild` applies the exclusion across the graph, neural state, familiarity, memory and counters.

## Ingestion policy

Every fetched record passes through an explicit policy before it can become a learning observation.

The current policy excludes unsupported records, the authenticated account's own output, blocked/muted relationships, moderation-filtered material, empty text and non-text-only posts. Skipped records are still represented in the ledger with a machine-readable reason.

Self-observation is intentionally excluded. I do not want the entity repeatedly learning from its own output and turning that feedback loop into fake evidence of experience.

## Outbound behaviour

An action candidate is evidence, not permission.

The outbound layer has its own action vocabulary and applies operator-authored policy, durable rate budgets and duplicate suppression before anything can reach the network. Missing policy is **default-deny**.

Useful inspection commands include:

```sh
./build/atperson outbound status
./build/atperson outbound rules
./build/atperson outbound evaluate reply at://did:plc:…/app.bsky.feed.post/… <digest>
./build/atperson outbound admit reply at://did:plc:…/app.bsky.feed.post/… <digest>
```

`evaluate` is read-only. `admit` can consume budget on an allowed action, but neither performs a network write.

See [`docs/outbound-policy.md`](docs/outbound-policy.md).

### Publishing

`atperson publish <action-file>` is the current network write path. It executes a frozen, approved action document through the operator, policy, rate-budget and dry-run gates before calling Wolfram.

```sh
./build/atperson publish action.json
```

Publishing is deliberately operator-led. Writes use a frozen record key so retries after ambiguous failures do not create duplicate posts, and attempts are recorded in a credential-free append-only audit log.

See [`docs/outbound-execution.md`](docs/outbound-execution.md).

### Network-native state

The observation ledger and action journal are publishable as AT Protocol
records under the entity's own DID, and fresh state can be reconstructed
from those records on a new host. Observation records carry provenance
and a content digest, never third-party text — reconstruction re-fetches
content from the source and verifies the digest before replaying.

```sh
./build/atperson thought the moon post made me curious
./build/atperson statepub status
./build/atperson statepub drain
./build/atperson statepub drain --offline
./build/atperson reconstruct --into ./fresh-state
```

`statepub drain` is gated on the control write gate and bounded per
pass. See [`docs/network-state.md`](docs/network-state.md).

## Current boundaries

`ATperson` currently does **not**:

- read private messages or private data;
- autonomously post, reply, like, follow, repost, DM or moderate;
- use an LLM as a hidden personality or decision engine;
- seed a biography, ideology, preferences or opinions into learned state;
- treat generated language as evidence of consciousness or personhood.

I do want the project to grow towards genuinely autonomous network behaviour, but not by skipping the hard parts. Sequence planning, policy, evidence, rate control, replay and operator-visible reasoning need to exist underneath it first.

## Licence

AGPL-3.0. See [`LICENSE`](LICENSE).

## Star History

<a href="https://www.star-history.com/?repos=ewanc26%2Fatperson&type=date&legend=bottom-right">
 <picture>
   <source media="(prefers-color-scheme: dark)" srcset="https://api.star-history.com/chart?repos=ewanc26/atperson&type=date&theme=dark&legend=bottom-right" />
   <source media="(prefers-color-scheme: light)" srcset="https://api.star-history.com/chart?repos=ewanc26/atperson&type=date&legend=bottom-right" />
   <img alt="Star History Chart" src="https://api.star-history.com/chart?repos=ewanc26/atperson&type=date&legend=bottom-right" />
 </picture>
</a>
