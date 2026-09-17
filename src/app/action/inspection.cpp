#include "inspection.hpp"

#include <atperson/core.h>

#include <charconv>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <iomanip>
#include <stdexcept>
#include <string>

namespace atperson {

std::string decision_digest(std::string_view context, const atp_action_decision &decision) {
    /* Canonical byte encoding: context text, then the accepted plan's
     * identity — step count, tokens, score bits, stop reason. Any change to
     * the context or the plan changes the digest, so an approval binds to
     * exactly the decision the operator inspected. */
    std::string canonical;
    canonical.reserve(context.size() + decision.plan.step_count * 16u + 16u);
    canonical.append(context);
    canonical.push_back('\0');
    canonical.append(reinterpret_cast<const char *>(&decision.plan.step_count),
                     sizeof(decision.plan.step_count));
    for (std::size_t i = 0u; i < decision.plan.step_count; ++i) {
        const atp_action_candidate &step = decision.plan.steps[i];
        const std::size_t token_length = strnlen(step.token, ATPERSON_TOKEN_BYTES);
        canonical.append(step.token, token_length);
        canonical.push_back('\0');
        std::uint32_t score_bits = 0u;
        static_assert(sizeof(step.score) == sizeof(score_bits),
                      "float must be 32-bit for stable digests");
        std::memcpy(&score_bits, &step.score, sizeof(score_bits));
        canonical.append(reinterpret_cast<const char *>(&score_bits), sizeof(score_bits));
    }
    std::uint32_t plan_score_bits = 0u;
    std::memcpy(&plan_score_bits, &decision.plan.score, sizeof(plan_score_bits));
    canonical.append(reinterpret_cast<const char *>(&plan_score_bits),
                     sizeof(plan_score_bits));
    const std::uint32_t stop_reason = static_cast<std::uint32_t>(decision.plan.stop_reason);
    canonical.append(reinterpret_cast<const char *>(&stop_reason), sizeof(stop_reason));

    const std::uint64_t digest =
        atp_ledger_digest(canonical.data(), canonical.size());
    char hex[17];
    std::snprintf(hex, sizeof(hex), "%016llx", static_cast<unsigned long long>(digest));
    return std::string(hex, 16u);
}

namespace {

std::size_t parse_bounded(std::string_view value, std::size_t minimum, std::size_t maximum,
                          std::string_view name) {
    std::size_t parsed = 0u;
    const auto result = std::from_chars(value.data(), value.data() + value.size(), parsed);
    if (result.ec != std::errc{} || result.ptr != value.data() + value.size() ||
        parsed < minimum || parsed > maximum) {
        throw std::runtime_error(std::string(name) + " must be between " +
                                 std::to_string(minimum) + " and " +
                                 std::to_string(maximum));
    }
    return parsed;
}

float parse_float_argument(std::string_view value, std::string_view name) {
    float parsed = 0.0f;
    const auto result = std::from_chars(value.data(), value.data() + value.size(), parsed);
    if (result.ec != std::errc{} || result.ptr != value.data() + value.size() ||
        !std::isfinite(parsed)) {
        throw std::runtime_error(std::string(name) + " must be a finite number");
    }
    return parsed;
}

const char *plan_stop_name(atp_action_plan_stop_reason reason) {
    switch (reason) {
    case ATP_ACTION_PLAN_STOP_NONE:
        return "none";
    case ATP_ACTION_PLAN_STOP_DEAD_END:
        return "dead-end";
    case ATP_ACTION_PLAN_STOP_MAX_TOKENS:
        return "max-tokens";
    case ATP_ACTION_PLAN_STOP_LOW_SCORE:
        return "low-score";
    case ATP_ACTION_PLAN_STOP_LOW_SUPPORT:
        return "low-support";
    case ATP_ACTION_PLAN_STOP_SCORE_DROP:
        return "score-drop";
    case ATP_ACTION_PLAN_STOP_REPETITION:
        return "repetition";
    case ATP_ACTION_PLAN_STOP_CYCLE:
        return "cycle";
    }
    return "unknown";
}

const char *abstain_name(atp_action_abstain_reason reason) {
    switch (reason) {
    case ATP_ACTION_ABSTAIN_NONE:
        return "none";
    case ATP_ACTION_ABSTAIN_EMPTY_CONTEXT:
        return "empty-context";
    case ATP_ACTION_ABSTAIN_NO_CANDIDATES:
        return "no-candidates";
    case ATP_ACTION_ABSTAIN_LOW_SCORE:
        return "low-score";
    case ATP_ACTION_ABSTAIN_LOW_SUPPORT:
        return "low-support";
    }
    return "unknown";
}

const char *context_kind_name(atp_context_item_kind kind) {
    switch (kind) {
    case ATP_CONTEXT_ITEM_IMMEDIATE:
        return "immediate";
    case ATP_CONTEXT_ITEM_RECENT:
        return "recent";
    case ATP_CONTEXT_ITEM_MEMORY:
        return "memory";
    case ATP_CONTEXT_ITEM_AUTHOR_STATE:
        return "author-state";
    case ATP_CONTEXT_ITEM_SOURCE_STATE:
        return "source-state";
    }
    return "unknown";
}

const char *context_reason_name(atp_context_reason reason) {
    switch (reason) {
    case ATP_CONTEXT_REASON_IMMEDIATE_INPUT:
        return "immediate-input";
    case ATP_CONTEXT_REASON_RECENT_INTERACTION:
        return "recent-interaction";
    case ATP_CONTEXT_REASON_EPISODIC_RECALL:
        return "episodic-recall";
    case ATP_CONTEXT_REASON_AUTHOR_FAMILIARITY:
        return "author-familiarity";
    case ATP_CONTEXT_REASON_SOURCE_FAMILIARITY:
        return "source-familiarity";
    }
    return "unknown";
}

void print_candidate(std::ostream &out, const atp_action_candidate &candidate, std::size_t index) {
    out << "  step " << index << " token=" << candidate.token << " score=" << std::fixed
        << std::setprecision(4) << candidate.score
        << " association=" << candidate.association_score
        << " familiarity=" << candidate.familiarity_score
        << " support=" << candidate.support_score
        << " observations=" << candidate.supporting_observations
        << " context-matches=" << candidate.context_matches << '\n';
}

void print_plan(std::ostream &out, const atp_action_plan &plan, std::size_t index) {
    out << "plan " << index << " score=" << std::fixed << std::setprecision(4) << plan.score
        << " stop=" << plan_stop_name(plan.stop_reason) << " steps=" << plan.step_count << '\n';
    for (std::size_t i = 0u; i < plan.step_count; ++i) {
        print_candidate(out, plan.steps[i], i);
    }
}

void print_stop_evidence(std::ostream &out, const atp_action_stop_evidence &evidence) {
    out << "stop-evidence reason=" << plan_stop_name(evidence.reason)
        << " step=" << evidence.step_index << " accepted=" << evidence.accepted_steps
        << " max-tokens=" << evidence.max_tokens;
    if (evidence.token[0] != '\0') {
        out << " token=" << evidence.token;
    }
    out << " candidate-score=" << std::fixed << std::setprecision(4) << evidence.candidate_score
        << " min-score=" << evidence.min_candidate_score
        << " support=" << evidence.support_score
        << " min-support=" << evidence.min_support_score
        << " previous-score=" << evidence.previous_score
        << " max-drop=" << evidence.max_score_drop
        << " consecutive=" << evidence.consecutive_occurrences
        << " max-consecutive=" << evidence.max_consecutive_occurrences;
    if (evidence.cycle_start_index != SIZE_MAX) {
        out << " cycle-start=" << evidence.cycle_start_index;
    }
    out << '\n';
}

void print_context_item(std::ostream &out, const atp_context_item &item, std::size_t index) {
    out << "item " << index << " kind=" << context_kind_name(item.kind)
        << " reason=" << context_reason_name(item.reason) << " score=" << std::fixed
        << std::setprecision(4) << item.score << " selected-tokens=" << item.selected_tokens
        << " available-tokens=" << item.available_tokens
        << " truncated=" << (item.truncated ? "yes" : "no");
    if (item.observed_at != 0u) {
        out << " observed-at=" << item.observed_at;
    }
    if (item.ledger_id != 0u) {
        out << " ledger=" << item.ledger_id;
    }
    if (item.source_id[0] != '\0') {
        out << " source=" << item.source_id;
    }
    if (item.author_did[0] != '\0') {
        out << " author=" << item.author_did;
    }
    out << '\n';

    if (item.kind == ATP_CONTEXT_ITEM_MEMORY) {
        out << "  recall total=" << item.recall.score << " exact=" << item.recall.exact_score
            << " association=" << item.recall.association_score
            << " familiarity=" << item.recall.familiarity_score
            << " recency=" << item.recall.recency_score << " use=" << item.recall.use_score
            << " exact-matches=" << item.recall.exact_token_matches
            << " association-matches=" << item.recall.association_token_matches << '\n';
    }
    if (item.kind == ATP_CONTEXT_ITEM_AUTHOR_STATE || item.kind == ATP_CONTEXT_ITEM_SOURCE_STATE) {
        out << "  interaction identifier=" << item.interaction.identifier
            << " encounters=" << item.interaction.encounter_count
            << " last-seen=" << item.interaction.last_seen_at
            << " remembered=" << item.interaction.remembered_episode_count
            << " familiarity=" << item.interaction.familiarity << '\n';
    }
}

atp_action_plan_config plan_config_from_arguments(const std::vector<std::string_view> &arguments) {
    atp_action_plan_config config = atp_action_plan_default_config();
    if (arguments.size() >= 2u) {
        config.max_tokens = parse_bounded(arguments[1], 1u, ATPERSON_PLAN_MAX_TOKENS,
                                          "max-tokens");
    }
    if (arguments.size() >= 3u) {
        config.beam_width = parse_bounded(arguments[2], 1u, ATPERSON_PLAN_MAX_BEAM_WIDTH,
                                          "beam-width");
    }
    return config;
}

} // namespace

bool is_action_inspection_command(std::string_view command) noexcept {
    return command == "plans" || command == "decide" || command == "context";
}

int run_action_inspection_command(std::ostream &out, const LanguageGraph &graph,
                                  std::string_view command,
                                  const std::vector<std::string_view> &arguments,
                                  std::uint64_t at_epoch) {
    if (!is_action_inspection_command(command)) {
        return 2;
    }

    if (command == "plans") {
        if (arguments.empty() || arguments.size() > 3u) {
            throw std::runtime_error("plans usage: plans <context> [max-tokens] [beam-width]");
        }
        const auto config = plan_config_from_arguments(arguments);
        const auto plans = graph.action_plans(arguments[0], config);
        out << "layer: learned-core\nnetwork-policy: not-evaluated\nplans: " << plans.size()
            << '\n'
            << "planner-config max-tokens=" << config.max_tokens
            << " beam-width=" << config.beam_width << '\n';
        for (std::size_t i = 0u; i < plans.size(); ++i) {
            print_plan(out, plans[i], i);
        }
        return 0;
    }

    if (command == "decide") {
        if (arguments.empty() || arguments.size() > 7u) {
            throw std::runtime_error(
                "decide usage: decide <context> [max-tokens] [beam-width] [min-candidate] "
                "[min-support] [max-drop] [max-consecutive]");
        }
        atp_action_decision_config config = atp_action_decision_default_config();
        config.planner = plan_config_from_arguments(arguments);
        if (arguments.size() >= 4u) {
            const float value = parse_float_argument(arguments[3], "min-candidate");
            if (value < 0.0f || value > 1.0f) {
                throw std::runtime_error("min-candidate must be between 0 and 1");
            }
            config.guards.min_candidate_score = value;
        }
        if (arguments.size() >= 5u) {
            const float value = parse_float_argument(arguments[4], "min-support");
            if (value < 0.0f || value > 1.0f) {
                throw std::runtime_error("min-support must be between 0 and 1");
            }
            config.guards.min_support_score = value;
        }
        if (arguments.size() >= 6u) {
            const float value = parse_float_argument(arguments[5], "max-drop");
            if (value < 0.0f || value > 1.0f) {
                throw std::runtime_error("max-drop must be between 0 and 1");
            }
            config.guards.max_score_drop = value;
        }
        if (arguments.size() >= 7u) {
            const std::size_t value =
                parse_bounded(arguments[6], 1u, 2u, "max-consecutive");
            config.guards.max_consecutive_occurrences = value;
        }
        const auto decision = graph.action_decide(arguments[0], config);
        out << "layer: learned-core\nnetwork-policy: not-evaluated\n"
            << "outcome: " << (decision.abstained ? "abstain" : "plan") << '\n'
            << "abstain-reason: " << abstain_name(decision.abstain_reason) << '\n'
            << "raw-plans: " << decision.raw_plan_count << '\n'
            << "viable-plans: " << decision.viable_plan_count << '\n'
            << "guard-thresholds min-candidate=" << std::fixed << std::setprecision(2)
            << config.guards.min_candidate_score << " min-support="
            << config.guards.min_support_score << " max-drop=" << config.guards.max_score_drop
            << " max-consecutive=" << config.guards.max_consecutive_occurrences << '\n';
        print_stop_evidence(out, decision.evidence);
        if (!decision.abstained) {
            print_plan(out, decision.plan, 0u);
            out << "digest: " << decision_digest(arguments[0], decision) << '\n';
        }
        return 0;
    }

    if (arguments.empty() || arguments.size() > 3u) {
        throw std::runtime_error("context usage: context <text> [source-id] [author-did]");
    }
    const std::string text(arguments[0]);
    const std::string source = arguments.size() >= 2u ? std::string(arguments[1]) : std::string{};
    const std::string author = arguments.size() >= 3u ? std::string(arguments[2]) : std::string{};
    const atp_context_request request{
        .immediate_text = text.c_str(),
        .source_id = source.empty() ? nullptr : source.c_str(),
        .author_did = author.empty() ? nullptr : author.c_str(),
        .recent = nullptr,
        .recent_count = 0u,
        .at_epoch = at_epoch,
    };
    const auto selection = graph.select_context(request);
    out << "layer: learned-core\nnetwork-policy: not-evaluated\n"
        << "items: " << selection.item_count << "\n"
        << "tokens: " << selection.token_count << "\n"
        << "recent-scored: " << selection.recent_inputs_scored << "\n"
        << "memory-scanned: " << selection.memory_episodes_scanned << "\n"
        << "item-limit-reached: " << (selection.item_limit_reached ? "yes" : "no") << '\n'
        << "token-limit-reached: " << (selection.token_limit_reached ? "yes" : "no") << '\n'
        << "memory-scan-truncated: " << (selection.memory_scan_truncated ? "yes" : "no")
        << '\n';
    for (std::size_t i = 0u; i < selection.item_count; ++i) {
        print_context_item(out, selection.items[i], i);
    }
    return 0;
}

} // namespace atperson
