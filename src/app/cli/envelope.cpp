#include "envelope.hpp"

#include "config.hpp"
#include "control/envelope.hpp"
#include "control/state.hpp"
#include "outbound/action.hpp"
#include "outbound/budget.hpp"
#include "outbound/config.hpp"

#include <optional>
#include <ostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace atperson {
namespace cli {
namespace {

[[noreturn]] void usage_error() {
    throw std::runtime_error(
        "control envelope usage: control envelope <list|show <id>|grant <id> "
        "[kinds=<kind:count/window[:floors]>...] [scope=<term,...>] [expires=<rfc3339>] "
        "[note=<text>]|revoke <id>|dry-run <action-file>>");
}

/* kinds=post:3/3600,reply:1/86400:0.4/0.2 — kind:max_in_window/
 * window_seconds[:min_plan_score/min_support_score]. */
std::vector<EnvelopeKindRule> parse_kind_specs(const std::vector<std::string_view> &values) {
    std::vector<EnvelopeKindRule> rules;
    for (const std::string_view value : values) {
        EnvelopeKindRule rule;
        const std::size_t colon = value.find(':');
        if (colon == std::string_view::npos) {
            throw std::runtime_error("kind spec must be <kind>:<max>/<window>[:<floors>]");
        }
        const std::string kind_name(value.substr(0u, colon));
        const auto kind = parse_outbound_kind(kind_name);
        if (!kind) {
            throw std::runtime_error("unknown action kind '" + kind_name + "'");
        }
        rule.kind = *kind;

        const std::string_view rest = value.substr(colon + 1u);
        const std::size_t slash = rest.find('/');
        if (slash == std::string_view::npos) {
            throw std::runtime_error("kind spec must be <kind>:<max>/<window>[:<floors>]");
        }
        rule.max_in_window = static_cast<std::uint32_t>(std::stoul(std::string(rest.substr(0u, slash))));
        if (rule.max_in_window == 0u) {
            throw std::runtime_error("max_in_window must be >= 1");
        }

        const std::size_t floors = rest.find(':', slash);
        if (floors == std::string_view::npos) {
            rule.window_seconds = std::stoll(std::string(rest.substr(slash + 1u)));
        } else {
            rule.window_seconds = std::stoll(std::string(rest.substr(slash + 1u, floors - slash - 1u)));
            const std::string_view floor_text = rest.substr(floors + 1u);
            const std::size_t floor_slash = floor_text.find('/');
            if (floor_slash == std::string_view::npos) {
                throw std::runtime_error("floors must be <min_plan_score>/<min_support_score>");
            }
            rule.min_plan_score = std::stod(std::string(floor_text.substr(0u, floor_slash)));
            rule.min_support_score = std::stod(std::string(floor_text.substr(floor_slash + 1u)));
        }
        if (rule.window_seconds < 1) {
            throw std::runtime_error("window_seconds must be >= 1");
        }
        rules.push_back(rule);
    }
    if (rules.empty()) {
        throw std::runtime_error("an envelope needs at least one kinds= spec");
    }
    return rules;
}

void print_envelope(std::ostream &out, const AuthorizationEnvelope &e) {
    out << "id: " << e.id << '\n'
        << "created: " << e.created_at << '\n'
        << "expires: " << (e.expires_at ? *e.expires_at : "never") << '\n';
    if (!e.scope_terms.empty()) {
        out << "scope: ";
        for (std::size_t i = 0u; i < e.scope_terms.size(); ++i) {
            if (i != 0u) {
                out << ", ";
            }
            out << e.scope_terms[i];
        }
        out << '\n';
    }
    for (const EnvelopeKindRule &rule : e.kinds) {
        out << "kind " << outbound_kind_name(rule.kind)
            << ": max " << rule.max_in_window << " per " << rule.window_seconds << "s";
        if (rule.min_plan_score > 0.0 || rule.min_support_score > 0.0) {
            out << " (floors: plan >= " << rule.min_plan_score
                << ", support >= " << rule.min_support_score << ")";
        }
        out << '\n';
    }
    if (!e.note.empty()) {
        out << "note: " << e.note << '\n';
    }
}

} // namespace

int run_envelope_command(std::ostream &out, const RuntimeResourceStatus &resource_status,
                         const std::filesystem::path &envelopes_dir,
                         const std::filesystem::path &policy_file, std::string_view sub,
                         const std::vector<std::string_view> &arguments, std::int64_t now) {
    const OutboundPolicy policy = load_outbound_policy(policy_file);

    if (sub == "list") {
        for (const std::string &id : list_authorization_envelopes(envelopes_dir)) {
            out << id << '\n';
        }
        return 0;
    }

    if (sub == "show") {
        if (arguments.empty()) {
            usage_error();
        }
        const auto envelope =
            load_authorization_envelope(envelopes_dir, arguments[0], policy);
        if (!envelope) {
            out << "no envelope " << arguments[0] << '\n';
            return 1;
        }
        print_envelope(out, *envelope);
        return 0;
    }

    if (sub == "grant") {
        if (arguments.empty()) {
            usage_error();
        }
        require_runtime_write_headroom(resource_status);
        const std::string id(arguments[0]);
        if (!is_envelope_id(id)) {
            throw std::runtime_error("envelope id must be 1-64 of [a-z0-9-], no edge hyphens");
        }
        if (load_authorization_envelope(envelopes_dir, id, policy)) {
            throw std::runtime_error("envelope " + id +
                                     " already exists; revoke it first or choose a new id");
        }

        AuthorizationEnvelope envelope;
        envelope.id = id;
        envelope.created_at = control_now_rfc3339();
        for (std::size_t i = 1u; i < arguments.size(); ++i) {
            const std::string_view argument = arguments[i];
            if (argument.starts_with("kinds=")) {
                std::vector<std::string_view> specs;
                const std::string_view list = argument.substr(6u);
                std::size_t start = 0u;
                while (start <= list.size()) {
                    const std::size_t comma = list.find(',', start);
                    specs.push_back(list.substr(start, comma == std::string_view::npos
                                                           ? std::string_view::npos
                                                           : comma - start));
                    if (comma == std::string_view::npos) {
                        break;
                    }
                    start = comma + 1u;
                }
                envelope.kinds = parse_kind_specs(specs);
            } else if (argument.starts_with("scope=")) {
                const std::string_view list = argument.substr(6u);
                std::size_t start = 0u;
                while (start <= list.size()) {
                    const std::size_t comma = list.find(',', start);
                    const std::string_view term =
                        list.substr(start, comma == std::string_view::npos
                                               ? std::string_view::npos
                                               : comma - start);
                    if (!term.empty()) {
                        envelope.scope_terms.emplace_back(term);
                    }
                    if (comma == std::string_view::npos) {
                        break;
                    }
                    start = comma + 1u;
                }
            } else if (argument.starts_with("expires=")) {
                envelope.expires_at = std::string(argument.substr(8u));
            } else if (argument.starts_with("note=")) {
                envelope.note = std::string(argument.substr(5u));
            } else {
                usage_error();
            }
        }
        if (envelope.kinds.empty()) {
            throw std::runtime_error("an envelope needs at least one kinds= spec");
        }

        /* Validate against the live policy before saving: an envelope can
         * only narrow it. parse_authorization_envelope's ceiling checks
         * run via save-time serialisation round-trip below. */
        const std::string json = serialise_authorization_envelope(envelope);
        parse_authorization_envelope(json, policy, "envelope " + id);
        save_authorization_envelope(envelope, envelopes_dir);
        out << "granted " << id << '\n';
        return 0;
    }

    if (sub == "revoke") {
        if (arguments.empty()) {
            usage_error();
        }
        require_runtime_write_headroom(resource_status);
        revoke_authorization_envelope(envelopes_dir, arguments[0]);
        out << "revoked " << arguments[0] << "; the next execution attempt will not be " <<
               "covered by it\n";
        return 0;
    }

    if (sub == "dry-run") {
        if (arguments.empty()) {
            usage_error();
        }
        const OutboundAction action = load_outbound_action(std::filesystem::path(arguments[0]));
        const OutboundBudgetState budget =
            load_outbound_budget_state(outbound_budget_path());
        const EnvelopeEvidence evidence{action.plan_score, action.support_score};
        const EnvelopeCoverage coverage =
            find_covering_envelope(envelopes_dir, policy, action.kind, action.text, evidence,
                                   budget, now);
        if (coverage.covered) {
            out << "covered by envelope " << coverage.envelope_id << '\n';
        } else {
            out << "not covered (" << coverage.reason << ")\n";
        }
        return 0;
    }

    usage_error();
}

} // namespace cli
} // namespace atperson
