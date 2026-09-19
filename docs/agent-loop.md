# Agent-loop contract

The runtime is an operator-led, fail-closed loop. Its stages are:

```text
perceive -> ledger -> learn -> decide -> policy -> control -> execute -> journal -> outcome mapping
```

Ownership is deliberately split. Wolfram owns AT Protocol sessions and repository
mechanics. The C++ runtime owns orchestration, extraction, policy, control and
durable runtime metadata. The C23 core owns the observation ledger, learned
state, candidate planning and replay. The action journal owns self-authored
provenance and the explicit outcome events that may update valence.

An observation is never learned unless the supported-record and ingestion-policy
checks accept it. A core abstention produces no outbound proposal. Missing or
disabled policy denies, an exhausted budget defers, and pause, dry-run, disabled
writes or missing approval produce no write. Unsupported actions are denied.
Execution failure is recorded as a failed outcome and never treated as success;
without an explicit outcome rule, no valence update occurs.

Credentials and session tokens are runtime-only. They must not enter snapshots,
the observation ledger, the action journal or audit data. The writer is lazy, so
refusal and dry-run do not require authentication.

The current implementation does not enable an autonomous scheduler, LLM,
persona, private-message learning, or broader social actions. Public Jetstream
perception is bounded and restart-safe; private message records such as
`chat.bsky.convo.defs#messageView` remain outside public-post ingestion.

This document describes the authority boundary, not permission to broaden the
runtime. Any future autonomous capability must preserve provenance, inspectable
decisions, rate limits, operator control and deterministic recovery.
