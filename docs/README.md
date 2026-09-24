# Documentation

The README gives the short version of `atperson`. These documents are the deeper reference for the parts that need more explanation than belongs on the front page.

The project has one architectural rule running through all of them: **learned state belongs to the C23 core; orchestration, policy and AT Protocol integration belong outside it.** Wolfram handles the protocol mechanics rather than `atperson` growing a second AT Protocol implementation of its own.

## Architecture and state

- [`architecture.md`](architecture.md) — the full C23/C++23 boundary, persistence model, ledger semantics and rebuild rules.
- [`conversation-context.md`](conversation-context.md) — reply roots, parents and quote targets as planning metadata rather than learnable text.
- [`interaction-state.md`](interaction-state.md) — neutral author/source continuity derived from existing learned evidence.
- [`valence.md`](valence.md) — experience-derived value state and its replay/provenance contract.
- [`resources.md`](resources.md) — runtime CPU, memory and disk budgeting without changing model semantics.

## Memory and planning

- [`episodic-recall.md`](episodic-recall.md) — ranked episodic recall and its evidence components.
- [`planner-context.md`](planner-context.md) — deterministic structured context selection.
- [`action-inspection.md`](action-inspection.md) — CLI surfaces for inspecting candidates, plans and decision traces.
- [`action-termination.md`](action-termination.md) — abstention, stop guards and the boundary between planning and policy.
- [`audit.md`](audit.md) — optional external advisory inspection through TypeSafe System One.

## Runtime and outbound behaviour

- [`daemon.md`](daemon.md) — long-running ingestion, retry/backoff and operator control.
- [`outbound-policy.md`](outbound-policy.md) — default-deny action policy and durable rate budgets.
- [`outbound-execution.md`](outbound-execution.md) — the operator-led `publish` path and its network-write gates.
- [`network-state.md`](network-state.md) — publishing durable experience as AT Protocol records and reconstructing state from the network (#142).
- [`action-journal.md`](action-journal.md) — durable history of the entity's own outbound attempts, later events and applied valence.
- [`attestation.md`](attestation.md) — operator-owned signing-key and audit metadata contract for future outbound record attestation.
- [`docker.md`](docker.md) — container builds and deployment.

## Repository operation

- [`ci-matrix.md`](ci-matrix.md) — supported CI/compiler coverage.
- [`issue-scopes.md`](issue-scopes.md) — the subsystem scope taxonomy used for issues.

These docs describe the current implementation, not an aspirational architecture. Roadmap work should be called out as roadmap work rather than written as though it already exists.
