#include "remote_poll.hpp"

#include <wolfram/repo_typed.h>
#include <wolfram/syntax.h>
#include <wolfram/xrpc.h>

#include <cJSON.h>

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace atperson {
namespace {

/* One fetched candidate, newest-first from the service. */
struct Candidate {
    std::string rkey;
    std::string json;
};

/* `rkey` is the last path segment of the at-URI. */
std::string uri_rkey(const std::string &at_uri) {
    const std::string::size_type slash = at_uri.rfind('/');
    if (slash == std::string::npos || slash + 1u == at_uri.size()) {
        throw std::runtime_error("remote control: record at-URI has no record key: " + at_uri);
    }
    return at_uri.substr(slash + 1u);
}

std::string uri_authority(const std::string &at_uri) {
    const std::string::size_type start = at_uri.find("://");
    if (start == std::string::npos) {
        return {};
    }
    const std::string::size_type from = start + 3u;
    const std::string::size_type slash = at_uri.find('/', from);
    if (slash == std::string::npos) {
        return {};
    }
    return at_uri.substr(from, slash - from);
}

} // namespace

RemotePollReport RemoteControlChannel::poll() {
    RemotePollReport report;

    /* Separation before anything else. A channel whose operator is the
     * account is not a channel: the entity could command itself. Refuse
     * the pass loudly rather than quietly applying self-authored records. */
    const OperatorChannelStatus channel =
        check_operator_channel(config_.account_did, config_.operator_did);
    if (channel == OperatorChannelStatus::Conflict) {
        report.conflict = true;
        report.refusals.push_back(
            {"", operator_channel_denial(config_.account_did, config_.operator_did)});
        return report;
    }
    report.enabled = !config_.operator_did.empty();
    if (!report.enabled) {
        report.refusals.push_back({"", operator_channel_denial(config_.account_did,
                                                               config_.operator_did)});
        return report;
    }

    /* An unusable cursor aborts the pass. Falling back to zero would reopen
     * the replay window the cursor exists to close. */
    const RemoteControlCursor cursor = load_remote_control_cursor(cursor_file_);
    std::uint64_t watermark = cursor.last_seq;
    report.watermark = watermark;

    /* Fetch newest-first, and stop at the first record we have already
     * applied. The service orders the collection, so nothing past that
     * point can be new. */
    std::vector<Candidate> candidates;
    {
        wf_repo_record_list list{};
        const int limit = static_cast<int>(std::max<std::size_t>(config_.max_records, 1u));
        const wf_status status = wf_agent_list_records_typed(
            session_.agent(), config_.operator_did.c_str(), std::string(kControlCollection).c_str(),
            limit, /*cursor=*/nullptr,
            /*reverse=*/1, &list);
        if (status != WF_OK) {
            wf_repo_record_list_free(&list);
            throw wolfram_error("remote control listRecords", status);
        }
        for (std::size_t i = 0u; i < list.count; ++i) {
            const wf_repo_record &item = list.items[i];
            if (!item.uri) {
                continue;
            }
            Candidate candidate;
            candidate.rkey = uri_rkey(item.uri);
            /* The authority comes from the at-URI the service returned, not
             * from the document: a record cannot claim to be someone
             * else's. A URI from another repo is dropped here. */
            if (uri_authority(item.uri) != config_.operator_did) {
                continue;
            }
            if (item.value) {
                char *raw = cJSON_PrintUnformatted(item.value);
                if (raw) {
                    candidate.json = raw;
                    cJSON_free(raw);
                }
            }
            candidates.push_back(std::move(candidate));
        }
        wf_repo_record_list_free(&list);
    }

    /* Newest-first, so reverse into ascending order before applying: a
     * pause the operator sent before a resume must not be applied after
     * it. */
    std::reverse(candidates.begin(), candidates.end());

    for (const Candidate &candidate : candidates) {
        if (candidates.size() > 0u && report.examined >= config_.max_records) {
            break;
        }
        ++report.examined;

        ControlRequest request;
        try {
            request = parse_control_request(candidate.json);
        } catch (const ControlRemoteError &error) {
            report.refusals.push_back({candidate.rkey, error.what()});
            continue;
        }

        /* Nothing at or below the watermark is new work. */
        if (request.seq <= watermark) {
            continue;
        }

        if (report.applied >= config_.max_applies) {
            report.refusals.push_back(
                {candidate.rkey, "apply budget exhausted for this pass; a later pass continues"});
            break;
        }

        /* apply_control_request mutates only on acceptance, so a refusal
         * here leaves the in-memory state exactly as it was. */
        ControlState state = load_control_state(control_file_);
        const RemoteApplyReport applied = apply_control_request(
            state, watermark, request, config_.operator_did, config_.operator_did);
        if (!applied.applied) {
            report.refusals.push_back({candidate.rkey, applied.reason});
            continue;
        }

        save_control_state(state, control_file_);
        /* Cursor last: a crash between the two re-applies an idempotent
         * op on the next pass rather than skipping one. */
        save_remote_control_cursor({1u, watermark}, cursor_file_);
        ++report.applied;
        report.last_op = applied.op;
        report.watermark = watermark;
    }

    return report;
}

} // namespace atperson
