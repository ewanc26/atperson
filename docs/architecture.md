# Architecture

atperson has one hard boundary:

```text
C23 = authoritative learned state
C++23 = runtime, integration, policy, and presentation
```

The distinction is intentional. The entity's learned language graph must remain
usable, testable, serialisable, and deterministic without requiring the network
runtime.

## C23 learning core

`atperson_core` owns all state that changes because of learning:

- token vocabulary;
- per-token embeddings;
- directed association edges;
- neural-network weights and biases;
- online gradient updates;
- exposure and training counters;
- source hashes attached to learned edges;
- PRNG state;
- persistence format.

A new graph contains no tokens or edges. Tokens are interned only when an
observation introduces them.

The initial neural component is deliberately small. Each token receives a
16-dimensional embedding. A feed-forward scorer takes the source and target
embeddings, passes their concatenation through a 16-unit `tanh` hidden layer,
and predicts whether the pair belongs together. Observed adjacent pairs are
positive examples; another known token is sampled as a negative example when
possible. Both the shared scorer and token embeddings train online with
gradient descent.

This is a learning primitive, not a finished language model. It gives later
systems a persistent, experience-shaped substrate without baking in a
pretrained model or scripted personality.

## C++23 runtime

The C++ layer owns concerns that should not become part of the model itself:

- process lifecycle;
- filesystem and configuration;
- RAII;
- AT Protocol sessions;
- JSON extraction;
- scheduling;
- future event deduplication;
- future action selection and publishing policy.

`LanguageGraph` is intentionally a thin RAII wrapper over the C API.

## Wolfram boundary

AT Protocol networking belongs to
[`ewanc26/wolfram`](https://github.com/ewanc26/wolfram).

atperson consumes Wolfram's C23 implementation and C++ RAII ownership layer. It
must not grow parallel implementations of XRPC, session management, DID/handle
resolution, repository operations, or Bluesky procedures.

The first network path uses `wf_agent_login` and `wf_agent_get_timeline`, then
extracts public post text and passes each post to the learning core with its AT
URI as the source identifier.

## Persistence

Snapshots are versioned and contain the complete mutable graph, neural
parameters, counters, and PRNG state. Saving is performed through a temporary
file and rename so a partially written snapshot does not replace the previous
state.

Version 1 is deliberately host-oriented and writes fixed-width integers and
IEEE-754 floats directly. A future portable format should define byte order and
migration rules before snapshots become a long-term public interchange format.

## Provenance and replay

The core currently stores a stable 64-bit hash of the most recent source that
reinforced each edge. That is enough to make provenance visible during early
experiments, but it is not enough for reliable unlearning.

Before continuous unattended ingestion is enabled, add a durable observation
ledger containing at least:

- AT URI / stable source identifier;
- author DID where applicable;
- observed-at time;
- content digest;
- model/schema version;
- processing outcome.

The ledger must support cross-run deduplication. The preferred deletion model is
rebuild-from-ledger rather than attempting approximate inverse gradient steps.

## Growth path

The intended order is:

1. **Language graph** — vocabulary, embeddings, associations, persistence.
2. **Observation ledger** — durable dedupe, replay, provenance, deletion.
3. **Memory** — episodic and semantic structures linked to sources.
4. **Internal state** — slowly learned preferences/values derived from repeated
   experience, not hard-coded personality text.
5. **Action model** — candidate generation and inspectable scoring.
6. **Network behaviour** — carefully rate-limited output through Wolfram.
7. **Long-running runtime** — event-driven or scheduled learning with crash
   recovery and explicit operator controls.

No stage should skip observability just to make the entity appear more human.
