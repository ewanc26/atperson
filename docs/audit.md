# Advisory audit

`atperson audit` can send a guarded C23 decision trace to TypeSafe System One for an external, non-authoritative reading of the evidence.

It is an inspection tool. It does not become a second planner, mutate learned state, change thresholds or grant network permission.

```sh
ATPERSON_TYPESAFE_API_KEY="…" \
  ./build/atperson audit "alpha delta" [max-tokens] [beam-width]
```

`max-tokens` and `beam-width` use the same defaults and hard bounds as `plans` and `decide`. `ATPERSON_TYPESAFE_ENDPOINT` defaults to `https://api.typesafe.ai/v1/systemone`; the configured model is `jev-latest`.

## What is sent

The request contains the bounded decision trace produced by the authoritative C23 path, including:

- context and final outcome (`plan` or `abstain`);
- raw and viable plan counts;
- stop/abstention evidence and the thresholds that produced it;
- every accepted step with association, familiarity and support evidence;
- planner and guard configuration.

The question set is fixed by outcome. A plan asks about first-step credibility, coherence and evidence quality; an abstention asks whether abstention is expected plus the same evidence-quality score.

The API key is read from the environment and used only in the bearer header. The command refuses to run without it.

## Output

The normal `layer: learned-core` trace is printed first. The external section is then labelled explicitly:

```text
audit-agent: typesafe
model: jev-latest
authority: advisory
```

The audit summary is a deterministic description of the returned answers. It is not fed back into C23 state or outbound policy.

## Boundary

- Running `audit` changes no learned state.
- It changes no planner configuration or decision threshold.
- It performs no AT Protocol write and cannot approve one.
- Other commands never invoke it implicitly.
- The C23 decision path remains fully usable and inspectable offline without this service.

HTTP 429 and 529 responses surface as errors with a retry hint; the CLI does not automatically retry them.

The evidence marshalling, question schema and response parsing/rendering are covered by offline tests. Only the external HTTP transport requires the network.