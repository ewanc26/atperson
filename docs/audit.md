# Advisory audit (`atperson audit`)

A decision trace from the C23 guarded planner can be sent to TypeSafe System
One for a **non-authoritative advisory judgment** about the supporting
evidence. This is an operator-facing inspection extension, not a learning or
policy path.

```sh
ATPERSON_TYPESAFE_API_KEY="…" \
  ./build/atperson audit "alpha delta" [max-tokens] [beam-width]
```

`max-tokens` and `beam-width` use the same C23 defaults and bounds as the
`plans`/`decide` commands. The endpoint is configured with
`ATPERSON_TYPESAFE_ENDPOINT` (default `https://api.typesafe.ai/v1/systemone`);
the model is fixed to `jev-latest`.

## What is sent

The request body contains:

- `state` — the bounded, authoritative decision trace: the guarded context,
  the outcome (`plan` or `abstain`), raw/viable plan counts, the stop evidence
  with echoed thresholds, every accepted step with full candidate scores
  (association, familiarity, support, supporting observations, context
  matches), and the planner/guard configuration that produced the decision;
- a fixed `questions` map chosen only by outcome:
  - plan outcome: `first-step-credible` (noul), `plan-coherent` (noul),
    `evidence-quality` (score);
  - abstain outcome: `abstain-expected` (noul), `evidence-quality` (score).

The API key is read from the environment into a `Bearer` header only; it is
never logged. Sending a trace is opt-in by construction: the command refuses
to run without `ATPERSON_TYPESAFE_API_KEY`.

## What is printed

The report renders the authoritative `layer: learned-core` decision trace
first (identical evidence to `decide`), then the advisory block:

```text
audit-agent: typesafe
model: jev-latest
authority: advisory (non-authoritative; learned state, planning, and network
policy are unchanged)
```

The `audit-summary:` line is a deterministic, threshold-based description
derived only from the returned answers, e.g. `supported`, `weak`,
`unsupported`, `abstention-corroborated`/`questioned`, with
`coherence-concern`/`low-confidence` when the relevant answer is below 0.5.

## Boundaries

- The audit is **advisory only**. It never mutates learned state, planning,
  decision thresholds, or AT Protocol policy, and no network action follows
  from any answer.
- Planner/decision output is unchanged with or without the audit command; the
  audit adds no learning and never runs during other commands.
- Rate-limit (`429`) and overload (`529`) responses surface as errors with a
  retry hint; the CLI does not retry.
- The raw C23 decision path remains fully inspectable offline through
  `plans`/`decide`; `audit` only adds the external advisory reading.

The evidence marshalling, question schema, and response parsing/rendering are
pure and covered by offline tests; only the HTTP transport is network-only.