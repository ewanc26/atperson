#ifndef ATPERSON_SCHEDULER_CYCLE_HPP
#define ATPERSON_SCHEDULER_CYCLE_HPP

// Autonomous scheduler cycle (#140): the composition point between bounded
// perception and the documented agent loop.
//
// After each successful daemon perception cycle, the scheduler:
//   1. selects the most recent committed ledger observations as decision
//      contexts (the ledger is the durable authority for what the entity
//      observed, so the scheduler never keeps a parallel context store);
//   2. asks the guarded C23 decision layer to decide on each context
//      (read-only; abstention is a first-class outcome);
//   3. freezes every accepted plan as an `atperson-outbound-action` v1
//      proposal document under <data>/scheduler/proposals/<digest>.json —
//      the exact bytes an operator inspects and `atperson publish` consumes;
//   4. executes proposals whose digest the operator has approved, strictly
//      through the existing #25 gate chain (pause -> policy -> dry-run ->
//      control -> Wolfram write), reloading control/policy/budget from disk
//      so an operator pause or revocation between cycles always wins.
//
// Pending social intent (#150) composes with the cycle, never bypassing it:
//   * every cycle opens with the intent sweep (expire/close intents whose
//     window or budget ended, journaled idempotently) followed by the
//     expectation resolution pass — so an expired intent's action resolves
//     to `unmet` in the same cycle that expired it;
//   * a candidate observation that continues an open intent (#150) is
//     ordered first by the intent drive when drives are enabled, and its
//     frozen proposal is composed as a reply into that thread (reply_root =
//     the intent's thread root, reply_parent = the triggered observation)
//     instead of an original post;
//   * after each executed action, the intent it continued (or opened) is
//     journaled — new intents only while under the active-intent cap.
//   Continuations are still decisions through the same C23 layer and the
//   same gate chain; intent only decides *what* the proposal is and what to
//   journal after execution.
//
// The scheduler composes the existing gates; it never gains a privileged
// write path. A refused or deferred proposal is reported and left in place
// for the operator. Failed executions are not retried within the same cycle.
//
// This atom is deliberately network- and Wolfram-free: the write boundary is
// the injected OutboundWriterFactory, so the whole cycle is testable
// offline with a fake writer (the same pattern as the publish tests).
//
// Bounds: max contexts examined per cycle, max proposals written per cycle,
// max execution attempts per cycle — all operator-configured, all
// inspectable in the cycle report.
//
// Ownership: borrows graph/ledger for the cycle's duration; never outlives
// them. Proposal files are written atomically (temp + rename). A proposal
// for a digest that already exists is not rewritten: the approval binds to
// the exact bytes.
//
// Failure modes: filesystem errors on the proposal directory throw; a
// decision error on one context skips that context and is counted; an
// execution failure is recorded by the gate chain itself (audit + journal)
// and reported, never thrown.

#include "intent/config.hpp"
#include "outbound/attempt.hpp"

#include "atperson/graph.hpp"
#include "atperson/ledger.hpp"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>

namespace atperson {

/* Operator-facing knobs for one scheduler cycle. Defaults keep the
 * scheduler inert: it must be explicitly enabled and bounded. */
struct SchedulerConfig {
    bool enabled{false};
    /* How many recent committed ledger observations to consider as
     * decision contexts. */
    std::size_t max_contexts{4};
    /* How many proposal documents one cycle may write. */
    std::size_t max_proposals{1};
    /* How many approved proposals one cycle may attempt to execute. */
    std::size_t max_executions{1};
    /* Wall-clock bound for the whole cycle, in milliseconds; 0 = unbounded.
     * Checked between execution attempts, the only potentially slow stage. */
    std::int64_t max_cycle_ms{0};
    /* Reorder candidate decision contexts by experience-derived drives
     * (#148): intent first (a reply an open intent is waiting on), then
     * reciprocity, then curiosity, then newest-first. Only changes which
     * contexts are decided on first; never widens the decision or gate
     * bounds. Off by default. */
    bool drives_enabled{false};
    /* Pending social intent (#150): on/off, caps and reply window. Off by
     * default; when enabled it composes with the cycle as documented at the
     * top of this header. */
    IntentConfig intents;
    /* Graduated actions (#152): when the guarded decision layer abstains
     * below the text floor (LOW_SCORE/LOW_SUPPORT) on a candidate whose
     * source is a likeable record, compose an explicit like proposal for
     * that subject instead. The like is a distinct decision with its own
     * digest and evidence — never an automatic downgrade of a rejected
     * plan, and it still passes every execution gate unchanged. Off by
     * default. */
    bool graduated_likes{false};
};

/* Accounting for one scheduler cycle, reported to the operator. */
struct SchedulerCycleReport {
    std::size_t contexts_examined{};
    std::size_t decisions{};
    std::size_t abstentions{};
    std::size_t proposals_written{};
    std::size_t proposals_existing{};
    std::size_t executions_attempted{};
    std::size_t executed{};
    std::size_t refused{};
    std::size_t failed{};
    std::string detail;
    /* Non-zero when drives reordering (#148) was applied to the candidate
     * context list for this cycle. */
    bool ordered_by_drives{false};
    /* Non-zero when at least one proposal written this cycle was composed as
     * the continuation of a pending social intent (#150). */
    bool ordered_by_intents{false};
    /* Non-zero when at least one proposal written this cycle was a
     * graduated like (#152) composed from a below-floor abstention. */
    std::size_t graduated_likes_written{};
    /* Expectation resolution (#149): how many executed expectations the
     * cycle's resolution pass evaluated, how many were still pending, and
     * how many terminal resolution lines it recorded. */
    std::size_t expectations_evaluated{};
    std::size_t expectations_pending{};
    std::size_t resolutions_written{};
    /* Pending social intent (#150): the intent sweep's accounting —
     * intents examined, and terminal intent lines it appended (expired when
     * the reply window closed, closed when the continuation budget was
     * reached) — plus per-cycle intent mutations from executed actions. */
    std::size_t intents_evaluated{};
    std::size_t intents_expired{};
    std::size_t intents_closed{};
    std::size_t intents_opened{};
    std::size_t intents_continued{};
    std::size_t intents_cap_reached{};
};

/* Paths and injected collaborators for one cycle. `writer_for` is the same
 * lazy factory the publish path uses: it must only establish a session
 * when a write is actually reached. `now` is injected for determinism in
 * tests. */
struct SchedulerCycle {
    /* Data directory: the outbound lock is taken per execution attempt, the
     * same lock `atperson publish` takes, so the two never race the budget. */
    std::filesystem::path data_dir;
    std::filesystem::path proposals_dir;
    OutboundAttemptPaths attempt;
    OutboundWriterFactory writer_for;
    std::int64_t now{};
    /* Monotonic milliseconds for the wall-clock bound; injected so tests
     * control time. Return 0 when unset is not possible — the caller must
     * always supply one in production (steady_clock). */
    std::function<std::int64_t()> steady_ms;
};

/* Run one bounded scheduler cycle. Returns the report; never throws for
 * decision-level problems (they are counted), only for infrastructure
 * failures (proposal directory I/O). */
[[nodiscard]] SchedulerCycleReport
run_scheduler_cycle(const SchedulerConfig &config, const SchedulerCycle &cycle,
                    const LanguageGraph &graph, const Ledger &ledger);

/* Operator configuration from the environment; off unless
 * ATPERSON_SCHEDULER=1. */
[[nodiscard]] SchedulerConfig scheduler_config_from_environment();

/* The outbound lock name, shared with `atperson publish`. */
inline constexpr const char *kSchedulerOutboundLockName = ".outbound-lock";

} // namespace atperson

#endif
