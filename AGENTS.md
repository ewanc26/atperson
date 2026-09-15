# AGENTS.md

## Project invariant

atperson is a C23/C++23 project.

- C23 owns authoritative learned state and learning algorithms.
- C++23 owns application/runtime concerns and AT Protocol integration.
- AT Protocol functionality comes from `ewanc26/wolfram`; do not duplicate
  Wolfram APIs locally.

If a change makes it impossible to run or test the learning graph without the
C++ network runtime, the boundary is probably wrong.

## Learning model rules

The entity starts without a seeded persona.

Do not add hard-coded:

- opinions;
- political/religious preferences;
- biography;
- favourite things;
- emotional history;
- social relationships;
- vocabulary presented as learned experience.

Small random neural initialisation is implementation state, not personality.
Tests may use fixed toy observations.

Learning changes must remain inspectable. Prefer explicit counters, scores,
provenance, and serialisable state over opaque global behaviour.

## Network rules

The current network path is read-only. Do not add autonomous posts, replies,
likes, follows, reposts, DMs, or moderation actions as a side effect of a
learning change.

Never log or persist app passwords.

Do not ingest private messages into the learning graph.

Any future continuous ingestion loop must first have durable source
deduplication so restarting the process cannot repeatedly train on the same
events.

## C23 core

Public C API lives in `include/atperson/`.

Core implementation lives in `src/core/`.

Avoid C++ types, exceptions, STL, or Wolfram dependencies in the C core.

Snapshot changes require an explicit format-version decision and tests covering
round-trip persistence.

## C++23 runtime

Keep `LanguageGraph` thin; model logic belongs in C.

Use RAII for Wolfram and JSON resources.

Environment/config parsing, filesystem handling, scheduling, and protocol event
translation belong here.

## Verification

For core-only work:

```sh
cmake -S . -B build-core -DATPERSON_BUILD_NETWORK=OFF
cmake --build build-core -j
ctest --test-dir build-core --output-on-failure
```

For network changes, also configure and build with
`ATPERSON_BUILD_NETWORK=ON`.

Keep commits atomic and scoped. Do not manufacture commit timestamps.
