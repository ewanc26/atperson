#include "arm.hpp"

#include "autonomy/arming.hpp"
#include "config.hpp"
#include "control/state.hpp"
#include "scheduler/breaker.hpp"
#include "state/time.hpp"

#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <ostream>
#include <system_error>
#include <stdexcept>
#include <string>

namespace atperson {
namespace cli {
namespace {

[[noreturn]] void usage_error(const std::string &detail = {}) {
    throw std::runtime_error(
        (detail.empty() ? std::string() : detail + "\n") +
        "autonomy usage: autonomy <arm --kinds kind:count/window[,...] [--scope a,b] "
        "[--min-plan n] [--min-support n] [--expires RFC3339|never] [--id id] [--apply] "
        "[--drives] [--intents] [--graduated-likes] [--valence-guard n] | "
        "disarm [--id id] | preflight | breaker [status|reset]>");
}

std::vector<std::string> split(std::string_view text, char separator) {
    std::vector<std::string> parts;
    std::size_t start = 0;
    while (start <= text.size()) {
        const std::size_t end = text.find(separator, start);
        const std::size_t stop = end == std::string_view::npos ? text.size() : end;
        if (stop > start) {
            parts.emplace_back(text.substr(start, stop - start));
        }
        if (end == std::string_view::npos) {
            break;
        }
        start = end + 1;
    }
    return parts;
}

std::int64_t parse_window(const std::string &text) {
    if (text.empty()) {
        usage_error("empty window");
    }
    std::int64_t multiplier = 1;
    std::string digits = text;
    switch (text.back()) {
    case 's': digits.pop_back(); break;
    case 'm': multiplier = 60; digits.pop_back(); break;
    case 'h': multiplier = 3600; digits.pop_back(); break;
    case 'd': multiplier = 86400; digits.pop_back(); break;
    default: break;
    }
    char *end = nullptr;
    errno = 0;
    const long long value = std::strtoll(digits.c_str(), &end, 10);
    if (digits.empty() || end == digits.c_str() || *end != '\0' || errno != 0 || value < 1) {
        usage_error("window '" + text + "' must be a positive number of seconds or N with s/m/h/d");
    }
    return value * multiplier;
}

double parse_unit(const std::string &text, const char *name) {
    char *end = nullptr;
    errno = 0;
    const double value = std::strtod(text.c_str(), &end);
    if (text.empty() || end == text.c_str() || *end != '\0' || errno != 0 || !std::isfinite(value)) {
        usage_error(std::string(name) + " must be a number");
    }
    return value;
}

std::vector<ArmKindSpec> parse_kinds(const std::string &text) {
    std::vector<ArmKindSpec> kinds;
    for (const std::string &item : split(text, ',')) {
        const std::size_t colon = item.find(':');
        const std::size_t slash = item.find('/');
        if (colon == std::string::npos || slash == std::string::npos || slash < colon) {
            usage_error("kind '" + item + "' must look like post:3/1d");
        }
        const auto kind = parse_outbound_kind(item.substr(0, colon));
        if (!kind) {
            usage_error("unknown action kind '" + item.substr(0, colon) + "'");
        }
        char *end = nullptr;
        const std::string count = item.substr(colon + 1, slash - colon - 1);
        errno = 0;
        const long long parsed = std::strtoll(count.c_str(), &end, 10);
        if (count.empty() || end == count.c_str() || *end != '\0' || errno != 0 || parsed < 1 ||
            parsed > 1000000) {
            usage_error("kind '" + item + "': the ceiling must be a positive number");
        }
        ArmKindSpec spec;
        spec.kind = *kind;
        spec.max_in_window = static_cast<std::uint32_t>(parsed);
        spec.window_seconds = parse_window(item.substr(slash + 1));
        kinds.push_back(spec);
    }
    return kinds;
}

std::string now_rfc3339() { return control_now_rfc3339(); }

ArmPaths paths_from_config() {
    const std::string denylist = env_or("ATPERSON_OUTPUT_DENYLIST");
    return ArmPaths{outbound_policy_path(), control_state_path(), authorization_envelopes_path(),
                    denylist.empty() ? data_dir() / "output-denylist.txt"
                                     : std::filesystem::path(denylist),
                    data_dir() / "scheduler-breaker.json"};
}

PreflightEnvironment environment_from_process() {
    PreflightEnvironment environment;
    environment.scheduler_enabled = env_or("ATPERSON_SCHEDULER") == "1";
    try {
        environment.publishing_switch = external_publishing_enabled();
    } catch (const std::exception &) {
        environment.publishing_switch = false; /* an invalid value is not "on" */
    }
    environment.has_identifier = !env_or("ATPERSON_IDENTIFIER").empty();
    environment.has_password = !env_or("ATPERSON_APP_PASSWORD").empty();
    environment.has_self_did = !env_or("ATPERSON_SELF_DID").empty();
    return environment;
}

int print_preflight(std::ostream &out, std::int64_t now_unix) {
    const PreflightReport report =
        run_preflight(environment_from_process(), paths_from_config(), now_unix);
    out << "autonomy preflight\n";
    std::size_t blocking = 0;
    for (const PreflightCheck &check : report.checks) {
        const char *tag = check.ok ? "ok  " : (check.advisory ? "note" : "FAIL");
        out << "  [" << tag << "] " << check.name << ": " << check.detail << '\n';
        if (!check.ok && !check.advisory) {
            ++blocking;
        }
    }
    if (!report.bounds.empty()) {
        out << "acts on its own within these bounds:\n";
        for (const std::string &line : report.bounds) {
            out << "  " << line << '\n';
        }
    }
    if (report.ready) {
        out << "result: READY. The daemon will act within these bounds with no further input.\n"
               "kill switches: `atperson control pause` (local) or a remote pause record from "
               "ATPERSON_OPERATOR_DID; `atperson autonomy disarm` revokes the authorisation.\n";
        return 0;
    }
    out << "result: NOT READY (" << blocking << " blocking)\n";
    return 1;
}

} // namespace

int run_autonomy_arming(std::ostream &out, std::string_view sub,
                        const std::vector<std::string_view> &arguments, std::int64_t now_unix) {
    if (sub == "preflight") {
        if (!arguments.empty()) {
            usage_error();
        }
        return print_preflight(out, now_unix);
    }

    if (sub == "breaker") {
        const std::filesystem::path file = data_dir() / "scheduler-breaker.json";
        const std::string action = arguments.empty() ? "status" : std::string(arguments[0]);
        if (arguments.size() > 1u || (action != "status" && action != "reset")) {
            usage_error();
        }
        if (action == "reset") {
            std::error_code ec;
            std::filesystem::remove(file, ec);
            out << "circuit breaker reset: closed, failure history cleared.\n";
            return 0;
        }
        const BreakerState state = load_breaker_state(file);
        const BreakerGate gate = breaker_gate(state, now_unix);
        out << "circuit breaker: " << breaker_gate_name(gate) << '\n'
            << "consecutive failures: " << state.consecutive_failures << '\n'
            << "trips: " << state.trips << '\n';
        if (state.open_until != 0) {
            out << "open until: " << rfc3339_from_unix(state.open_until)
                << " (cool-down " << state.cooldown_seconds << "s)\n";
        }
        if (!state.last_failure_detail.empty()) {
            out << "last failure: " << rfc3339_from_unix(state.last_failure_at) << " "
                << state.last_failure_detail << '\n';
        }
        if (!state.proposal_failures.empty()) {
            out << "proposals with failures: " << state.proposal_failures.size() << '\n';
        }
        out << "it heals by itself: after the cool-down one attempt is made, and a success "
               "closes it.\n";
        return 0;
    }

    ArmRequest request;
    bool apply = false;
    bool drives = false;
    bool intents = false;
    bool graduated_likes = false;
    std::string valence_guard;
    bool have_kinds = false;
    for (std::size_t i = 0; i < arguments.size(); ++i) {
        const std::string flag(arguments[i]);
        auto value = [&]() -> std::string {
            if (i + 1 >= arguments.size()) {
                usage_error(flag + " needs a value");
            }
            return std::string(arguments[++i]);
        };
        if (flag == "--id") {
            request.envelope_id = value();
        } else if (sub == "disarm") {
            usage_error("unexpected argument '" + flag + "'");
        } else if (flag == "--kinds") {
            request.kinds = parse_kinds(value());
            have_kinds = true;
        } else if (flag == "--scope") {
            request.scope_terms = split(value(), ',');
        } else if (flag == "--min-plan") {
            request.min_plan_score = parse_unit(value(), "--min-plan");
        } else if (flag == "--min-support") {
            request.min_support_score = parse_unit(value(), "--min-support");
        } else if (flag == "--expires") {
            const std::string when = value();
            if (when != "never") {
                request.expires_at = when;
            }
        } else if (flag == "--apply") {
            apply = true;
        } else if (flag == "--drives") {
            drives = true;
        } else if (flag == "--intents") {
            intents = true;
        } else if (flag == "--graduated-likes") {
            graduated_likes = true;
        } else if (flag == "--valence-guard") {
            valence_guard = value();
            const double guard = parse_unit(valence_guard, "--valence-guard");
            if (guard < -1.0 || guard > 0.0) {
                usage_error("--valence-guard must be between -1 and 0");
            }
        } else {
            usage_error("unknown option '" + flag + "'");
        }
    }

    const ArmPaths paths = paths_from_config();
    if (sub == "disarm") {
        disarm(paths, request.envelope_id);
        out << "disarmed: network writes are off and envelope '" << request.envelope_id
            << "' (and '" << request.envelope_id << "-actions') are revoked. Nothing can be published until `atperson autonomy arm --apply` "
               "or `atperson control writes on` plus a per-action approval.\n";
        return 0;
    }
    if (sub != "arm") {
        usage_error();
    }
    if (!have_kinds) {
        usage_error("--kinds is required");
    }

    ControlState control = load_control_state(paths.control_file);
    const OutboundPolicy policy = load_outbound_policy(paths.policy_file);
    ArmPlan plan;
    try {
        plan = plan_arm(request, policy, control, now_rfc3339());
    } catch (const ArmError &error) {
        usage_error(error.what());
    }

    out << (apply ? "arming:\n" : "arming plan (dry run; nothing is written):\n");
    for (const std::string &change : plan.changes) {
        out << "  " << change << '\n';
    }
    if (!apply) {
        out << "re-run with --apply to write it.\n";
    } else {
        apply_arm(plan, paths);
        out << "written:";
        for (const AuthorizationEnvelope &envelope : plan.envelopes) {
            out << ' ' << paths.envelopes_dir.string() << '/' << envelope.id << ".json,";
        }
        out << ' ' << paths.policy_file.string() << ", " << paths.control_file.string() << '\n';
    }

    out << "environment (put these in the .env file and restart the daemon; this command never "
           "edits it):\n"
           "  ATPERSON_SCHEDULER=1\n"
           "  ATPERSON_ALLOW_EXTERNAL_PUBLISHING=true\n";
    if (drives) {
        out << "  ATPERSON_SCHEDULER_DRIVES=1\n";
    }
    if (intents) {
        out << "  ATPERSON_INTENTS=1\n";
    }
    if (graduated_likes) {
        out << "  ATPERSON_SCHEDULER_GRADUATED_LIKES=1\n";
    }
    if (!valence_guard.empty()) {
        out << "  ATPERSON_DECISION_MIN_VALENCE=" << valence_guard << '\n';
    }
    out << "  ATPERSON_IDENTIFIER / ATPERSON_APP_PASSWORD / ATPERSON_SELF_DID as for sync\n";

    if (apply) {
        return print_preflight(out, now_unix);
    }
    return 0;
}

} // namespace cli
} // namespace atperson
