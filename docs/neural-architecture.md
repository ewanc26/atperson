# Neural architecture direction

The current C23 learner is a durable, deterministic dense scorer over entity
pairs. It is useful for associative memory and must remain available as a
portable fallback, but it is not the language model for ATperson: a bounded
embedding plus a few fully-connected layers cannot represent long context,
compositional syntax, tool-use traces, or protocol explanations.

## Required model boundary

ATperson's language capability must be supplied by a real autoregressive
Transformer family model, not by a Markov chain, n-gram table, or a larger
version of the current pair scorer. The model contract is:

- token embeddings, positional information, causal self-attention, residual
  connections, normalization, and feed-forward blocks;
- a versioned tokenizer and model manifest, including parameter count, layer
  count, hidden width, attention heads, context window, precision, and
  checkpoint digest;
- either a large dense model or a sparse Mixture-of-Experts model with
  explicit routing and top-k activation. Capacity is measured by total
  parameters and activated parameters separately;
- retrieval-augmented context for protocol evidence, with source IDs and
  verification state preserved in every retrieved context item;
- a separately versioned inference backend. The portable C23 graph is an
  associative-memory and deterministic fallback, never an implicit claim of
  language understanding.

The first production target should be a large sparse MoE Transformer when the
hardware budget supports it. MoE provides substantially greater total model
capacity without requiring every expert for every token; the routing policy
and activated-parameter count must be observable. A dense Transformer remains
the compatibility target for runtimes without sparse kernels.

## Training and knowledge separation

Protocol literacy is trained and retrieved as a technical knowledge domain,
not mixed into social preference learning. `./protocol/` remains the source
of replayable, provenance-bearing evidence. The language model may consume a
materialized training/retrieval view of that evidence, but it must not rewrite
the evidence ledger or turn unverified observations into facts.

The training corpus should combine:

1. official AT Protocol specifications and Lexicon schemas;
2. verified repository/DID/identity observations from the protocol ledger;
3. labeled positive and negative examples for deletion, missing data,
   signature failure, cursor gaps, and App View authority;
4. ordinary language data for generalization, with domain and provenance
   labels retained for evaluation.

Evaluation must include held-out protocol questions, multi-hop identity and
repository explanations, citation correctness, contradiction handling, and
resync reasoning. Perplexity alone is insufficient.

## Operational constraints

Large-model inference and training belong behind a backend boundary and may
use accelerator or remote execution. The C23 core still owns deterministic
state transitions, snapshot/replay, and policy enforcement. No model output
may autonomously perform an external action; actions remain explicit plans
requiring approval.

The manifest must reject configurations that identify only an embedding width
or hidden-layer count. Such a configuration is the legacy associative scorer,
not a language model. Model upgrades are immutable, digest-addressed, and
recorded alongside the protocol knowledge view used for the response.

## Research basis

- Shazeer et al., *Sparsely-Gated Mixture-of-Experts*:
  https://arxiv.org/abs/1701.06538
- Du et al., *GLaM: Efficient Scaling of Language Models with Mixture-of-Experts*:
  https://arxiv.org/abs/2112.06905
- Lewis et al., *Retrieval-Augmented Generation for Knowledge-Intensive NLP
  Tasks*: https://arxiv.org/abs/2005.11401
- AT Protocol Lexicon specification:
  https://atproto.com/specs/lexicon

