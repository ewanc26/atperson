# Contributing to ATperson

`ATperson` is an experiment in building a persistent digital entity on the AT Protocol that starts with effectively nothing and develops its own learned state through experience. Thanks for wanting to help!

## The design, briefly

- **C23 owns learned state.** The language graph, trainable embeddings, online learning, episodic memory, familiarity, valence, action scoring, the observation ledger, snapshot persistence.
- **C++23 owns orchestration.** Lifecycle, configuration, RAII, CLI, record extraction, the runtime around the core.
- **[Wolfram](https://github.com/ewanc26/wolfram) owns AT Protocol mechanics.** Sessions, XRPC, identity, repository operations are dependencies, not reimplementations.

The rule the project keeps: **C++ can orchestrate the entity, but it must not quietly become the authority for what the entity has learned.** Learned state and the evidence behind it stay in the C core.

## Notes for contributing

- Preserve that split. An easier C++ shortcut for something that is learned state is exactly the kind of change this repo exists to avoid.
- Learned state must stay **durable, inspectable, and reconstructable**: changes to persistence or the ledger need to show how a snapshot survives and how the evidence trail reads after your change.
- This is experimental software that does not claim sentience or personhood, and **autonomous network behaviour is deliberately gated behind explicit policy and operator control.** Contributions that loosen those gates will not be merged, however exciting they look.
- Test against signed-out/public reads and a scratch account for any network path; never log credentials.

## Before opening a PR

- Build with the project's toolchain and run the tests; CI runs on every push.
- Conventional commits; focused PRs; for learning-core changes, describe what behaviour the entity showed before and after, not just that the code compiled.
- AI-assisted PRs welcome; stand behind every line and actually run the build and tests.

## Licence

See [LICENSE](LICENSE). Contributions accepted under the same terms.