# Agent loop contract

Issue #61 defines the boundary between atperson's learned state and any future autonomous AT Protocol runtime. This document is a contract, not an enablement switch: **autonomous network writes are not enabled by this design**.

The loop is deliberately explicit:

```text
perceive
   |
   v
ledger commit
   |
   v
learn / remember
   |
   v
bounded C23 decision
   |
   v
C++ outbound policy
   |
   v
operator control gates
   |
   v
network execution through Wolfram
   |
   v
action/outcome journal
   |
   v
explicit outcome -> valence mapping
```

A later runtime may schedule these stages, but it must not collapse their ownership boundaries.

## 1. Session

**Owner:** C++23 runtime + Wolfram.

The connected AT Protocol account is runtime identity, not learned state.

- authentication is established through the Wolfram-backed client;
- credentials come from runtime configuration/environment only;
- credentials and session tokens must never be written to the model snapshot, observation ledger, action journal, outbound audit log or any other durable atperson file;
- a session should be created lazily and live only as long as the operation needs it;
- C23 receives no credential or session object.

The existing outbound execution path already creates its writer lazily after pause, policy, dry-run and control gates. A denied or dry-run action therefore never needs to authenticate.

## 2. Perception

**Owner:** C++23 runtime for transport/orchestration; C23 ledger for durable observation authority.

Public observations enter through an ingestion transport, then the explicit ingestion policy, then the observation ledger. Learning never bypasses the ledger.

Current perception includes bounded timeline polling and the long-running daemon. Real-time repository/firehose perception remains tracked by #60.

The required ordering is:

```text
transport -> extraction -> ingestion policy -> ledger commit -> learned state
```

The transport cursor is runtime metadata. It is not learned state and cannot make a rebuild learn something that is absent from the ledger.

### Private-message boundary

Private messages are not learning input.

The current ingestion policy accepts only `app.bsky.feed.post` records. Any private-message/conversation payload presented to that policy must be classified as `unsupported-record` and never reach `remember`/training.

A future DM transport must remain outside the learning graph unless a new, explicit privacy design changes this contract. Adding a transport must not silently broaden the ingestion policy.

## 3. Decision

**Owner:** authoritative C23 learned core.

Decision begins with inspectable learned evidence, not network policy.

The current core provides:

- one-step action candidates;
- bounded deterministic plans;
- structured context selection;
- guarded decision/abstention;
- explicit stop/abstain reasons and step evidence.

An accepted C23 decision means only:

> the learned core found enough evidence for this candidate/plan.

It does **not** mean the network action is permitted.

The C++ runtime must not replace or reinterpret the core score with a hidden prompt, persona, LLM judgment or opaque policy score.

## 4. Policy

**Owner:** C++23 outbound policy/runtime control.

Every proposed write crosses two independent fail-closed boundaries.

### Outbound policy

The operator-authored outbound policy decides whether a supported action kind is enabled and whether its durable rate/duplicate budgets permit it.

The decision vocabulary is inspectable:

- `allow`;
- `deny`;
- `defer`.

Stable reason codes explain the result. A missing/default policy disables writes rather than enabling them.

### Operator control

Even an `allow` from outbound policy is not permission to write. The control layer still applies:

1. pause;
2. dry-run;
3. master `writes_enabled` gate;
4. exact-digest approval when approval is required.

These controls are runtime metadata, never learned state.

## 5. Execution

**Owner:** C++23 runtime through Wolfram.

The only implemented network write path is an explicit `atperson publish <action-file>` using a frozen, operator-inspected action document.

Execution must preserve these rules:

- the action text is not regenerated after approval;
- the rkey is frozen before the write so retries are idempotent;
- replies resolve current strong references only at execution;
- Wolfram owns AT Protocol mechanics;
- the rate budget is committed only after confirmed success;
- every attempt is written to the credential-free outbound audit log and action/outcome journal.

The current runtime supports operator-led posts and replies only. Likes, follows, reposts, moderation, DMs and other writes remain unsupported/fail-closed.

The autonomous scheduler (#140) invokes the same frozen-action path through the shared attempt composition: it composes the guarded C23 decision on recent ledger contexts into frozen proposal documents, and executes only operator-approved digests through the identical gate chain (pause, policy, dry-run, control, external-publishing). It does not get a privileged write API. It is off by default (`ATPERSON_SCHEDULER`), bounded per cycle, and testable offline with a fake writer.

## 6. Outcome and experience

**Owner:** action/outcome journal for durable self-authored provenance; C23 valence for learned effect.

The action journal is the authority for what the entity itself attempted. It records:

- the frozen action/rkey/digest;
- execution outcome and reason;
- resulting AT URI/CID for confirmed writes;
- later public reply/root/quote events linked by stable URI;
- explicit valence applications.

The observation ledger remains the authority for third-party observations. The two evidence streams are intentionally separate.

Outcome-to-valence mapping is governed by #56 and is explicit, bounded and off by default. Network success or social response does not silently rewrite learned preferences.

## Fail-closed matrix

| Missing/refused boundary | Required result |
| --- | --- |
| No supported public observation | Nothing is learned |
| Unsupported/private-message record | Ledgered/skipped where applicable; never trained |
| Core decision abstains | No outbound proposal |
| Missing/disabled outbound policy | Deny with named reason |
| Rate/cooldown budget exhausted | Defer with named reason |
| Runtime paused | Deny before write |
| Dry-run enabled | Full evaluation, no login/write |
| Writes disabled | Deny |
| Required approval absent/mismatched | Deny |
| Writer/session/network failure | Failed outcome; no confirmed-success budget commit |
| Unsupported action kind | Deny/unsupported |
| Missing outcome mapping rule | No valence update |

There is no fallback that means “write anyway”.

## Inspection / WorkTrace target

A future autonomous loop should emit one trace object per attempted decision cycle. The exact wire format is intentionally not fixed here, but the trace must be able to answer:

1. **What was perceived?** Stable observation/ledger provenance, never credentials.
2. **What did the learned core decide?** Context selection, candidate/plan evidence, score and abstention/stop reason.
3. **What did policy decide?** Action kind, allow/deny/defer reason and the budget state used.
4. **What did operator control decide?** Pause/dry-run/write/approval gate result.
5. **What was executed?** Frozen action id/rkey/digest and network result, if any.
6. **What happened afterwards?** Journal events linked to the action.
7. **What changed in learned state?** Only explicit, provenance-linked outcome/valence updates.

This is the atperson equivalent of a WorkTrace: evidence is carried across boundaries rather than replaced by a final opaque “agent said yes”.

## Dependency map

| Responsibility | Current subsystem / tracked work |
| --- | --- |
| Session | Wolfram-backed `AtprotoClient` / outbound writer |
| Polling perception | `sync` + ingestion state |
| Long-running perception | `daemon` |
| Real-time firehose perception | #60 |
| Public-data ingestion policy | policy layer / roadmap #20 |
| Durable third-party experience | C23 observation ledger |
| Candidate/planning/abstention | C23 action/decision/context APIs |
| Outbound policy + durable budgets | #23 |
| Operator pause/dry-run/approval | #22 |
| Operator-led network execution | #25 |
| Self-authored action/outcome provenance | #27 |
| Outcome-to-valence mapping | #56 |
| Optional outbound attestation | #57 |

Every responsibility is therefore either implemented already or tracked independently. #61 does not smuggle an untracked protocol/policy subsystem into the agent layer.

## What #61 does not enable

This contract does not remove approval, enable write kinds, broaden ingestion, add an LLM, seed a persona, or create a second protocol implementation. The scheduler added by #140 follows step 5 exactly: it composes the existing contracts without bypassing any of them, and every executed action still requires an operator-approved exact digest.

The safe implementation sequence remains:

1. keep perception and learned-state reconstruction reliable;
2. keep decision evidence inspectable;
3. keep policy/control default-deny;
4. keep the only write path frozen and auditable;
5. only then add a scheduler that composes those existing contracts without bypassing any of them.
