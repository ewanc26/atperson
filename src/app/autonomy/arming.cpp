#include "arming.hpp"

#include "../scheduler/breaker.hpp"
#include "../state/time.hpp"
#include "../scheduler/text_guard.hpp"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <set>
#include <sstream>
#include <system_error>

namespace atperson {
namespace {

bool is_executable_kind(OutboundActionKind kind) noexcept {
    switch (kind) {
    case OutboundActionKind::Post:
    case OutboundActionKind::Reply:
    case OutboundActionKind::Like:
    case OutboundActionKind::Repost:
    case OutboundActionKind::Follow:
        return true;
    case OutboundActionKind::Unfollow:
    case OutboundActionKind::Moderation:
        return false;
    }
    return false;
}

/* Kinds whose executable action carries no text and no decision evidence. */
bool is_textless(OutboundActionKind kind) noexcept {
    return kind == OutboundActionKind::Like || kind == OutboundActionKind::Repost ||
           kind == OutboundActionKind::Follow;
}

std::string window_text(std::int64_t seconds) {
    if (seconds % 86400 == 0) {
        return std::to_string(seconds / 86400) + "d";
    }
    if (seconds % 3600 == 0) {
        return std::to_string(seconds / 3600) + "h";
    }
    if (seconds % 60 == 0) {
        return std::to_string(seconds / 60) + "m";
    }
    return std::to_string(seconds) + "s";
}

std::string limit_text(std::uint32_t count, std::int64_t window) {
    return std::to_string(count) + " per " + window_text(window);
}

/* Atomic write: temp file + rename, so a crash leaves the old or the new
 * complete document. */
void write_file_atomic(const std::filesystem::path &path, const std::string &contents) {
    if (const auto parent = path.parent_path(); !parent.empty()) {
        std::error_code ec;
        std::filesystem::create_directories(parent, ec);
    }
    const std::filesystem::path tmp = path.string() + ".tmp";
    {
        std::ofstream output(tmp, std::ios::binary | std::ios::trunc);
        if (!output) {
            throw std::runtime_error("cannot write " + tmp.string());
        }
        output << contents;
        output.flush();
        if (!output) {
            throw std::runtime_error("failed while writing " + tmp.string());
        }
    }
    std::error_code ec;
    std::filesystem::rename(tmp, path, ec);
    if (ec) {
        throw std::runtime_error("cannot replace " + path.string() + ": " + ec.message());
    }
}

const EnvelopeKindRule *rule_for(const AuthorizationEnvelope &envelope, OutboundActionKind kind) {
    for (const EnvelopeKindRule &rule : envelope.kinds) {
        if (rule.kind == kind) {
            return &rule;
        }
    }
    return nullptr;
}

} // namespace

ArmPlan plan_arm(const ArmRequest &request, const OutboundPolicy &current_policy,
                 const ControlState &current_control, std::string_view now_rfc3339) {
    if (!is_envelope_id(request.envelope_id)) {
        throw ArmError("envelope id must be 1-64 characters of [a-z0-9-], not starting or ending "
                       "with a hyphen");
    }
    if (request.kinds.empty()) {
        throw ArmError("arming needs at least one action kind to authorise");
    }
    if (!std::isfinite(request.min_plan_score) || request.min_plan_score < 0.0 ||
        request.min_plan_score > 1.0 || !std::isfinite(request.min_support_score) ||
        request.min_support_score < 0.0 || request.min_support_score > 1.0) {
        throw ArmError("score floors must be between 0 and 1");
    }

    ArmPlan plan;
    plan.base_id = request.envelope_id;
    plan.policy = current_policy;
    plan.control = current_control;

    std::set<OutboundActionKind> seen;
    for (const ArmKindSpec &spec : request.kinds) {
        const std::string name = outbound_kind_name(spec.kind);
        if (!is_executable_kind(spec.kind)) {
            throw ArmError("kind '" + name +
                           "' is not executable (post, reply, like, repost and follow are)");
        }
        if (!seen.insert(spec.kind).second) {
            throw ArmError("kind '" + name + "' is listed twice");
        }
        if (spec.max_in_window < 1u) {
            throw ArmError("kind '" + name + "': the ceiling must be at least 1");
        }
        if (spec.window_seconds < 1 || spec.window_seconds > kMaxOutboundWindowSeconds) {
            throw ArmError("kind '" + name + "': the window must be between 1 second and 366 days");
        }

        ActionBudget budget = budget_for(plan.policy, spec.kind);
        const bool was_enabled = budget.enabled;
        budget.enabled = true;
        budget.max_in_window = spec.max_in_window;
        budget.window_seconds = spec.window_seconds;
        /* Spread actions out instead of allowing a burst at the start of each
         * window, and never repeat the identical action within a window. */
        budget.min_interval_seconds = spec.window_seconds / static_cast<std::int64_t>(spec.max_in_window) / 2;
        budget.duplicate_cooldown_seconds = spec.window_seconds;
        budget_for(plan.policy, spec.kind) = budget;
        plan.changes.push_back(std::string("policy: ") + (was_enabled ? "update " : "enable ") +
                               name + " at " + limit_text(spec.max_in_window, spec.window_seconds) +
                               ", min interval " + window_text(std::max<std::int64_t>(
                                                       budget.min_interval_seconds, 1)));
    }

    plan.control.paused = false;
    plan.control.writes_enabled = true;
    plan.control.dry_run = false;
    if (!current_control.writes_enabled || current_control.dry_run || current_control.paused) {
        plan.changes.push_back("control: writes on, dry-run off, not paused");
    }

    const bool any_text = std::any_of(request.kinds.begin(), request.kinds.end(),
                                      [](const ArmKindSpec &k) { return !is_textless(k.kind); });
    const bool any_textless = std::any_of(request.kinds.begin(), request.kinds.end(),
                                          [](const ArmKindSpec &k) { return is_textless(k.kind); });
    const std::string textless_id =
        any_text ? request.envelope_id + "-actions" : request.envelope_id;
    if (any_textless && !is_envelope_id(textless_id)) {
        throw ArmError("envelope id is too long to add the '-actions' companion");
    }

    for (const bool textless : {false, true}) {
        if (textless ? !any_textless : !any_text) {
            continue;
        }
        AuthorizationEnvelope envelope;
        envelope.id = textless ? textless_id : request.envelope_id;
        envelope.expires_at = request.expires_at;
        envelope.note = textless ? "autonomy arm (kinds without text)" : "autonomy arm";
        envelope.created_at = std::string(now_rfc3339);
        if (!textless) {
            envelope.scope_terms = request.scope_terms;
        }
        for (const ArmKindSpec &spec : request.kinds) {
            if (is_textless(spec.kind) != textless) {
                continue;
            }
            EnvelopeKindRule rule;
            rule.kind = spec.kind;
            rule.max_in_window = spec.max_in_window;
            rule.window_seconds = spec.window_seconds;
            if (!textless) {
                rule.min_plan_score = request.min_plan_score;
                rule.min_support_score = request.min_support_score;
            }
            envelope.kinds.push_back(rule);
        }

        /* The real parser is the authority on what an envelope may be: run the
         * plan through it against the resulting policy so a plan we produce can
         * never be rejected (or mean something different) when it is loaded. */
        try {
            envelope = parse_authorization_envelope(serialise_authorization_envelope(envelope),
                                                    plan.policy, "arm plan");
        } catch (const EnvelopeError &error) {
            throw ArmError(std::string("the envelope would be invalid: ") + error.what());
        }

        std::ostringstream text;
        text << "envelope '" << envelope.id << "': " << envelope.kinds.size() << " kind(s), "
             << (request.expires_at ? "expires " + *request.expires_at
                                    : std::string("never expires"));
        if (!envelope.scope_terms.empty()) {
            text << ", scope: ";
            for (std::size_t i = 0; i < envelope.scope_terms.size(); ++i) {
                text << (i == 0 ? "" : ",") << envelope.scope_terms[i];
            }
        }
        if (!textless && (request.min_plan_score > 0.0 || request.min_support_score > 0.0)) {
            text << ", floors: plan>=" << request.min_plan_score
                 << " support>=" << request.min_support_score;
        }
        if (textless) {
            text << ", ceilings only (no text or decision score to scope)";
        }
        plan.changes.push_back(text.str());
        plan.envelopes.push_back(std::move(envelope));
    }
    if (any_textless && (!request.scope_terms.empty() || request.min_plan_score > 0.0 ||
                         request.min_support_score > 0.0)) {
        plan.changes.push_back(
            "note: scope and score floors apply to post/reply only; like/repost/follow are "
            "bounded by their ceilings");
    }
    return plan;
}

std::vector<std::string> arm_envelope_ids(std::string_view envelope_id) {
    return {std::string(envelope_id), std::string(envelope_id) + "-actions"};
}

void apply_arm(const ArmPlan &plan, const ArmPaths &paths) {
    /* Order matters. The envelopes and policy grant nothing until writes are
     * on, so writing the control state last means a failure part-way through
     * leaves the entity inert rather than half-armed. Any envelope from an
     * earlier arm under the same id is replaced, not merged. */
    for (const std::string &id : arm_envelope_ids(plan.base_id)) {
        revoke_authorization_envelope(paths.envelopes_dir, id);
    }
    for (const AuthorizationEnvelope &envelope : plan.envelopes) {
        save_authorization_envelope(envelope, paths.envelopes_dir);
    }
    write_file_atomic(paths.policy_file, serialise_outbound_policy(plan.policy));
    save_control_state(plan.control, paths.control_file);
}

void disarm(const ArmPaths &paths, std::string_view envelope_id) {
    /* Writes off first: the moment this returns from the first step nothing can
     * publish, whatever happens to the rest. */
    ControlState control = load_control_state(paths.control_file);
    control.writes_enabled = false;
    save_control_state(control, paths.control_file);
    for (const std::string &id : arm_envelope_ids(envelope_id)) {
        revoke_authorization_envelope(paths.envelopes_dir, id);
    }
}

PreflightReport run_preflight(const PreflightEnvironment &environment, const ArmPaths &paths,
                              std::int64_t now_unix) {
    PreflightReport report;
    auto add = [&report](std::string name, bool ok, std::string detail, bool advisory = false) {
        report.checks.push_back({std::move(name), ok, advisory, std::move(detail)});
    };

    add("scheduler", environment.scheduler_enabled,
        environment.scheduler_enabled ? "enabled (ATPERSON_SCHEDULER=1)"
                                      : "off: set ATPERSON_SCHEDULER=1 so the daemon decides "
                                        "and proposes on its own");
    add("publishing-switch", environment.publishing_switch,
        environment.publishing_switch
            ? "on (ATPERSON_ALLOW_EXTERNAL_PUBLISHING)"
            : "off: set ATPERSON_ALLOW_EXTERNAL_PUBLISHING=true; nothing is published without it");
    const bool credentials = environment.has_identifier && environment.has_password;
    add("credentials", credentials,
        credentials ? "identifier and app password are set"
                    : "set ATPERSON_IDENTIFIER and ATPERSON_APP_PASSWORD so each cycle can "
                      "open a session");
    add("identity", environment.has_self_did,
        environment.has_self_did
            ? "ATPERSON_SELF_DID is set, so the entity's own posts are excluded from learning"
            : "ATPERSON_SELF_DID is unset: the entity would learn from its own posts",
        /*advisory=*/true);

    /* Output guard: the structural rules (no URLs, mentions or hashtags, length,
     * no repeated text) are always on; the denylist is the operator's part. */
    if (!paths.denylist_file.empty()) {
        try {
            const std::size_t terms = load_denylist(paths.denylist_file).size();
            add("output-guard", terms > 0,
                terms > 0 ? "structural rules on; denylist has " + std::to_string(terms) +
                                " term(s)"
                          : "structural rules on (no URLs, mentions or hashtags, length, no "
                            "repeated text); no denylist terms: add words the entity must never "
                            "say to " + paths.denylist_file.string(),
                /*advisory=*/true);
        } catch (const std::exception &error) {
            add("output-guard", false, std::string("denylist unreadable: ") + error.what());
        }
    }

    /* Circuit breaker: it heals on its own, so an open breaker is informative
     * rather than a setup fault, but an operator should be able to see why
     * nothing is being published right now. */
    if (!paths.breaker_file.empty()) {
        try {
            const BreakerState breaker = load_breaker_state(paths.breaker_file);
            const BreakerGate gate = breaker_gate(breaker, now_unix);
            if (gate == BreakerGate::Closed) {
                add("circuit-breaker", true, "closed", /*advisory=*/true);
            } else {
                add("circuit-breaker", false,
                    std::string(breaker_gate_name(gate)) + " after " +
                        std::to_string(breaker.consecutive_failures) +
                        " consecutive failure(s), " + std::to_string(breaker.trips) +
                        " trip(s); it retries by itself" +
                        (gate == BreakerGate::Open
                             ? " at " + rfc3339_from_unix(breaker.open_until)
                             : std::string(" on the next attempt")) +
                        ". Last failure: " + breaker.last_failure_detail,
                    /*advisory=*/true);
            }
        } catch (const std::exception &error) {
            add("circuit-breaker", false, std::string("breaker state unreadable: ") + error.what());
        }
    }

    /* Control state. */
    ControlState control;
    bool control_loaded = false;
    try {
        control = load_control_state(paths.control_file);
        control_loaded = true;
    } catch (const std::exception &error) {
        add("control", false, std::string("control state unreadable: ") + error.what());
    }
    if (control_loaded) {
        std::vector<std::string> problems;
        if (control.paused) {
            problems.push_back("paused (`control resume`)");
        }
        if (!control.writes_enabled) {
            problems.push_back("network writes are off (`control writes on`)");
        }
        if (control.dry_run) {
            problems.push_back("dry-run is on (`control dry-run off`)");
        }
        if (control.offline_mode) {
            problems.push_back("offline mode spools writes instead of publishing");
        }
        std::string detail;
        for (std::size_t i = 0; i < problems.size(); ++i) {
            detail += (i == 0 ? "" : "; ") + problems[i];
        }
        add("control", problems.empty(),
            problems.empty() ? "not paused, writes on, dry-run off, online" : detail);
    }

    /* Policy. */
    OutboundPolicy policy;
    bool policy_loaded = false;
    std::vector<OutboundActionKind> enabled;
    try {
        policy = load_outbound_policy(paths.policy_file);
        policy_loaded = true;
        for (std::size_t index = 0; index < kOutboundActionKindCount; ++index) {
            const auto kind = static_cast<OutboundActionKind>(index);
            if (is_executable_kind(kind) && budget_for(policy, kind).enabled) {
                enabled.push_back(kind);
            }
        }
        std::string names;
        for (std::size_t i = 0; i < enabled.size(); ++i) {
            names += (i == 0 ? "" : ", ") + std::string(outbound_kind_name(enabled[i]));
        }
        add("policy", !enabled.empty(),
            enabled.empty() ? "no action kind is enabled; every kind is default-off"
                            : "enabled: " + names);
    } catch (const std::exception &error) {
        add("policy", false, std::string("outbound policy unreadable: ") + error.what());
    }

    /* Authorisation: which enabled kinds can run without a per-action human
     * step. Coverage is judged by the real envelope logic, per envelope, with a
     * probe that satisfies that envelope's scope and floors. */
    if (policy_loaded && control_loaded) {
        std::vector<std::string> uncovered;
        std::size_t authorised = 0;
        std::vector<std::string> problems;
        std::vector<AuthorizationEnvelope> envelopes;
        for (const std::string &id : list_authorization_envelopes(paths.envelopes_dir)) {
            try {
                if (auto loaded = load_authorization_envelope(paths.envelopes_dir, id, policy)) {
                    envelopes.push_back(std::move(*loaded));
                }
            } catch (const std::exception &error) {
                problems.push_back("envelope '" + id + "' is unusable and covers nothing (" +
                                   error.what() + ")");
            }
        }
        for (const OutboundActionKind kind : enabled) {
            const std::string name = outbound_kind_name(kind);
            if (!control.approval_required) {
                ++authorised;
                report.bounds.push_back(name + ": " +
                                        limit_text(budget_for(policy, kind).max_in_window,
                                                   budget_for(policy, kind).window_seconds) +
                                        " (per-action approval is off; policy limits apply)");
                continue;
            }
            bool covered = false;
            std::string reason = "no envelope";
            for (const AuthorizationEnvelope &envelope : envelopes) {
                /* Probe exactly as the executor will see this kind: a like,
                 * repost or follow has no text and records no decision
                 * evidence, so a scope or score floor on an envelope that
                 * lists one is unsatisfiable and must show up as uncovered
                 * here rather than as an action that silently waits. Text kinds
                 * get a probe that meets the envelope's scope, and the best
                 * possible evidence. */
                const bool textless = is_textless(kind);
                const EnvelopeEvidence evidence = textless ? EnvelopeEvidence{}
                                                           : EnvelopeEvidence{1.0, 1.0};
                const std::string probe =
                    textless ? std::string()
                             : (envelope.scope_terms.empty() ? std::string("x")
                                                             : envelope.scope_terms.front());
                const EnvelopeCoverage coverage = covers_action(
                    envelope, kind, probe, evidence, OutboundKindUsage{}, now_unix);
                if (!coverage.covered) {
                    reason = "envelope '" + envelope.id + "': " + coverage.reason;
                    continue;
                }
                covered = true;
                const EnvelopeKindRule *rule = rule_for(envelope, kind);
                const ActionBudget &budget = budget_for(policy, kind);
                std::ostringstream line;
                line << name << ": at most "
                     << limit_text(std::min(budget.max_in_window, rule->max_in_window),
                                   rule->window_seconds)
                     << " (policy " << limit_text(budget.max_in_window, budget.window_seconds)
                     << ", envelope '" << envelope.id << "' "
                     << limit_text(rule->max_in_window, rule->window_seconds) << "), "
                     << (envelope.expires_at ? "until " + *envelope.expires_at
                                             : std::string("no expiry"));
                if (!envelope.scope_terms.empty()) {
                    line << ", scope: ";
                    for (std::size_t i = 0; i < envelope.scope_terms.size(); ++i) {
                        line << (i == 0 ? "" : ",") << envelope.scope_terms[i];
                    }
                }
                if (rule->min_plan_score > 0.0 || rule->min_support_score > 0.0) {
                    line << ", floors: plan>=" << rule->min_plan_score
                         << " support>=" << rule->min_support_score;
                }
                report.bounds.push_back(line.str());
                break;
            }
            if (covered) {
                ++authorised;
            } else {
                uncovered.push_back(name + " (" + reason + ")");
            }
        }
        std::string detail;
        if (authorised > 0) {
            detail = std::to_string(authorised) + " kind(s) can act without per-action approval";
        } else {
            detail = "nothing is authorised to run unattended: grant an envelope (`autonomy arm "
                     "--apply`) or every action waits for `control approve <digest>`";
        }
        if (!uncovered.empty()) {
            detail += "; still needs per-action approval: ";
            for (std::size_t i = 0; i < uncovered.size(); ++i) {
                detail += (i == 0 ? "" : ", ") + uncovered[i];
            }
        }
        for (const std::string &problem : problems) {
            detail += "; " + problem;
        }
        add("authorization", authorised > 0, detail);
    }

    report.ready = std::all_of(report.checks.begin(), report.checks.end(),
                               [](const PreflightCheck &check) { return check.ok || check.advisory; });
    return report;
}

} // namespace atperson
