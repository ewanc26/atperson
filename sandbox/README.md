# Offline sandbox

A self-contained environment for training and inspecting ATperson without the
network or the live data directory.

---

## What it is

`sandbox/bin/atperson-sandbox` is a wrapper around the `atperson` CLI that pins
`ATPERSON_HOME` to an isolated directory (`sandbox/data` by default) and unsets
network credentials before every invocation. Nothing it runs can touch the live
model in `~/.ewanc26/atperson`, establish a PDS session, or reach Jetstream.

The sandbox builds against the core-only configuration
(`ATPERSON_BUILD_NETWORK=OFF`), so Wolfram is not required.

---

## Setup

```sh
cmake -S . -B build-core -DATPERSON_BUILD_NETWORK=OFF
cmake --build build-core -j
```

The wrapper expects the binary at `build/atperson`. Override with
`ATPERSON_BIN` when using a different build directory:

```sh
ATPERSON_BIN="$PWD/build-core/atperson" sandbox/bin/atperson-sandbox run stats
```

---

## Usage

```sh
# run any CLI command inside the sandbox
sandbox/bin/atperson-sandbox run stats
sandbox/bin/atperson-sandbox run ingest "the wolf watches the moon" sandbox:manual

# scripted walkthrough: train, plan, decide, audit, outbound evaluation
sandbox/bin/atperson-sandbox scenario

# wipe and re-create the sandbox data directory
sandbox/bin/atperson-sandbox reset
```

| Command | Effect |
| --- | --- |
| `init` | Create the sandbox data directory (idempotent) |
| `reset` | Wipe the sandbox data directory |
| `run <args...>` | Run any `atperson` CLI command with the sandbox home |
| `scenario` | Full offline walkthrough over the fixtures |

Environment overrides:

| Variable | Default |
| --- | --- |
| `ATPERSON_SANDBOX_HOME` | `<repo>/sandbox/data` |
| `ATPERSON_BIN` | `<repo>/build/atperson` |

`sandbox/data` is gitignored; the fixtures are the durable content.

---

## Fixtures

### `fixtures/text/`

Plain-text corpora for `ingest-file`. Three files with distinct vocabularies
(werewolf lore, AT Protocol notes, quiet mornings) so association inspection
shows learned links between related tokens.

### `fixtures/jetstream/frames.jsonl`

Jetstream v1 frames in the real wire shape — commit creates, a commit delete,
and an identity frame. The scenario replays the post text through
`atperson context` with the frame's DID as author, which is the same
observation path live Jetstream ingestion uses. The delete and identity frames
exercise skip handling in the scenario script.

These frames are also reference material for the envelope shape: `did`,
`time_us`, `kind`, and the nested `commit` with `rev`, `operation`,
`collection`, `rkey`, `record` and `cid`.

### `fixtures/actions/reply-action.json`

A frozen `atperson-outbound-action` v1 document, for inspecting what
`atperson publish` consumes. Publishing from the sandbox always refuses:
credentials are unset and outbound policy is fail-closed by default.

---

## Scenario walkthrough

`atperson-sandbox scenario` runs ten steps and fails loudly if any step errors:

1. `stats` — confirm the empty graph
2. `ingest-file` — train on each text fixture
3. `ingest` — replay each jetstream frame's post text as an observation
4. `assoc` — inspect learned associations
5. `plans` — bounded plan generation from a trained context
6. `decide` — guarded decision on the same context
7. `outbound status/rules/evaluate` — offline policy evaluation
8. `journal valence` — experience-derived valence state
9. `stats` — final state

Every step is a plain CLI call, so the scenario doubles as documentation of
the normal offline workflow. `atperson audit` is deliberately absent: it sends
the decision trace to the TypeSafe advisory API and is therefore not an
offline command.

---

## Boundary notes

- The scenario's frame replay uses `atperson context` rather than a jetstream
  client because the frame ingestion path requires a live WebSocket; the
  observation semantics are the same. The frame's DID is printed for
  traceability; `atperson ingest` records the source id, not the author DID.
- `sandbox/data` holds model snapshots, ledger, and runtime state for the
  sandbox instance. Delete it freely; fixtures regenerate the content.
- Nothing in the sandbox writes to AT Protocol. Outbound evaluation is
  read-only and publishing refuses without credentials.

---

License: AGPL-3.0-only
