# ATperson compared with conventional agents

ATperson is an autonomous agent, but it is not built like the LLM-based agents
that dominate the literature: there is no language model, no prompt, no
embedding store and no seeded persona. Its learned state is a language graph, an
episodic memory, familiarity and valence, its decisions are a bounded
deterministic planner, and every step is rebuildable from a durable ledger.

That difference is the reason to compare. Conventional agent stacks have spent
years discovering what an autonomous system needs around its "brain" in order to
be safe and dependable when nobody is watching. This page records that
comparison: what the conventional practice is, what ATperson does, and where it
had a genuine gap. Gaps are only closed in ways that fit the project's rules
(deterministic, inspectable, no hidden model, no invented personality; see
`AGENTS.md`). Where a conventional technique depends on an LLM, the honest answer
is often "not applicable", and that is recorded as such rather than imitated.

## What conventional agents do, and ATperson's equivalent

| Conventional practice | ATperson | Status |
| --- | --- | --- |
| **Memory stream with recency / importance / relevance retrieval** (Generative Agents) | Source-linked episodic memory; recall combines exact token support, learned graph association, bounded familiarity, recency and previous use, each separately inspectable. No LLM-scored "importance". | Equivalent by design |
| **Reflection**: periodically consolidate memories into higher-level notes | Deterministic reflection pass writing bounded, template-authored thoughts; read-only over learned state | Equivalent by design |
| **Learning from failure** (Reflexion) | Outcomes map to explicit valence through journal rules; expectations are resolved against what actually happened | Have it (and see below for the decision-side use) |
| **Skill library** (Voyager): keep verified behaviours as reusable code | Not applicable: nothing generates code. The learned graph and valence are the procedural memory | Not applicable |
| **Planning and tool use** | Bounded beam planner with abstention, repetition and cycle guards; the only "tool" is the outbound writer, behind policy | Have it |
| **Human-in-the-loop approvals** | Per-digest approval, or a bounded standing envelope; kill switches stay | Have it; the unattended path is `autonomy arm` (#192) |
| **Checkpointing, replay, durable execution** | Ledger replay, snapshots, torn-tail repair on every append-only store, a kill-the-host recovery drill | Have it (#189) |
| **Observability and trajectory evaluation** | Journal trace of one action (#180), audit log, self-evaluation metrics, heartbeat and health | Have it; a single end-to-end trace object is still roadmap |
| **Output guardrails** | Did not exist | **Closed** (#193) |
| **Loop detection, circuit breakers, retry limits** | Cycle and repetition guards existed inside a plan; nothing for repeated *execution* failures | **Closed** (#194) |
| **Budget and rate caps** | Per-kind budgets, spacing, duplicate suppression, envelope ceilings | Have it (#192 makes them one setup step) |
| **Consent and etiquette toward the platform's users** | Graduated likes targeted any post read | **Closed** (#195) |
| **Setup that can be verified** (readiness checks) | Enabling autonomy was five hand edits; a mistake left the entity silently idle | **Closed** (#192) |
| **Input guardrails** (prompt injection) | There is no prompt, but the analogue is learned-content poisoning: hostile public text shaping what the entity later says | Mitigated, open item below |
| **Escalation to a human when something is wrong** | Health and heartbeat, breaker and preflight report; the entity has no channel to message its operator | Open item below |
| **Behavioural regression evaluation** | Deterministic replay and property tests; no suite that scores end behaviour over a fixed corpus | Open item below |
| **Identity and persona** | Deliberately absent: no seeded biography, opinions or preferences | By design |

## Gaps that were real, and what closed them

These are the places where the comparison found something an unattended agent
needs that ATperson lacked. Each is checked against the code, not assumed.

- **Output guardrail (#193).** The text the entity publishes is assembled from
  learned tokens, and the tokens come from whatever public text it has read.
  Nothing made that safe to publish unattended, and nothing detected the same
  words going out twice (duplicate suppression is keyed to the decision, not the
  text). A deterministic guard now refuses empty or invalid text, over-long text,
  URLs, mentions, hashtags, operator-denylisted terms and repeats, at proposal
  time and again at execution.
- **Circuit breaker and bounded retry (#194).** A proposal that could never
  succeed was retried every cycle forever, and a systemic failure was retried at
  full speed. Conventional runtimes use loop detection, cool-downs and half-open
  probes for exactly this. The scheduler now opens a breaker after a streak of
  failures, probes once after a cool-down that doubles up to a cap, quarantines
  poison proposals, and caps the pending queue.
- **Consent and opt-out (#195).** The platform's own bot guidance is to interact
  only with users who have engaged with the bot. The entity now needs an
  invitation before liking, reposting or following, and honours an operator
  do-not-engage list in every mode.
- **Bot self-label (#197).** The operator can inspect, set or clear the
  `bot` self-label with `atperson autonomy bot-label`. It reads the existing
  `app.bsky.actor.profile/self` record and preserves the rest of the profile;
  the autonomous scheduler never changes this identity metadata.

- **A verifiable setup (#192).** `autonomy arm` writes the policy, envelope and
  control state together, and `autonomy preflight` proves the chain is complete,
  including a trap where a scope on an envelope silently made likes unauthorisable.

## Open items

- **Learned-content poisoning.** The ingestion policy, withdrawal, per-author
  neutral exposure state and the output guard limit the damage, but there is no
  detector for a coordinated attempt to shift what the entity learns.
- **Server-signalled rate limits.** The breaker backs off on any failure, which
  covers rate limiting, but the entity does not read the server's rate-limit
  headers to wait exactly as long as asked.
- **Invitations by mention.** An invitation is currently a reply or quote of
  something the entity published. Mentions are not captured, and the entity cannot
  see that someone has blocked it.
- **Operator notification.** When the breaker trips or the preflight would fail,
  the state is visible (`autonomy breaker`, `autonomy preflight`, `autonomy
  health`) but nothing tells the operator.
- **End-to-end behavioural evaluation** over a fixed corpus, to catch a change
  that alters what the entity says.

## Sources

- [Memory for Autonomous LLM Agents: Mechanisms, Evaluation, and Emerging Frontiers](https://arxiv.org/html/2603.07670v1)
- [Generative Agents: Interactive Simulacra of Human Behavior](https://www.researchgate.net/publication/375063078_Generative_Agents_Interactive_Simulacra_of_Human_Behavior) (memory stream, reflection, recency/importance/relevance retrieval)
- [Voyager: An Open-Ended Embodied Agent with Large Language Models](https://arxiv.org/html/2305.16291) (skill library) and Reflexion (verbal reinforcement from failed episodes)
- [LLM Powered Autonomous Agents](https://lilianweng.github.io/posts/2023-06-23-agent/) (planning, memory, tool use)
- [A Survey on AgentOps](https://arxiv.org/pdf/2508.02121) and [Four Bounds That Keep Agent Loops From Running Away](https://formation.dev/blog/agent-loop-runaway-costs) (loop detection, budget bounds, circuit breakers)
- [Designing Fault-Tolerant Autonomous AI Agents: Circuit Breakers, Retry Policies and Observability](https://dev.to/tamizuddin/designing-fault-tolerant-autonomous-ai-agents-circuit-breakers-retry-policies-and-observability-3f47)
- [Bluesky Community Guidelines](https://bsky.social/about/support/community-guidelines) (spam, repeated posting, engagement manipulation)
- [AT Protocol: Build an Agent](https://atproto.com/guides/bot-tutorial) (bot self-label; interact only when the user has tagged the bot; respect rate limits)
- [Bluesky rate limits](https://bsky.network/docs/rate-limits/) (points per hour and day; `ratelimit-*` response headers)
