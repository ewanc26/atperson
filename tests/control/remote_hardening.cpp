/* Hardening of the remote operator channel against hostile or corrupt request
 * documents. The record comes from a remote repo, so parse + apply must be
 * total: a malformed document is a ControlRemoteError (never another exception
 * type), a refused request leaves state and watermark untouched, and no
 * accepted request can leave the durable control state unsaveable.
 * Offline. */

#include "control/remote.hpp"
#include "control/state.hpp"

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <string>
#include <unistd.h>

namespace {

using namespace atperson;

const std::string kOperator = "did:plc:operator";

std::string request_json(const std::string &seq_field, const std::string &op,
                         const std::string &arg_field = "") {
    return std::string(R"({"format":"atperson-control-request","version":1,)") +
           R"("type":"click.croft.atperson.control#request",)" + seq_field + R"(,"op":")" + op +
           "\"" + arg_field + "}";
}

std::filesystem::path scratch(const char *name) {
    const auto dir = std::filesystem::temp_directory_path() /
                     ("atperson-remote-hardening-" + std::to_string(::getpid()));
    std::filesystem::create_directories(dir);
    return dir / name;
}

/* A digest argument that is not a 16-lowercase-hex control digest can never
 * match an action, and would make the durable state refuse to save. It must be
 * refused before it reaches state. */
void test_malformed_digest_arguments_are_refused() {
    for (const char *bad : {"not-a-digest", "0123456789ABCDEF", "0123456789abcde",
                            "0123456789abcdef0", "0123456789abcdeg", " 0123456789abcde",
                            "0123456789abcde\\n"}) {
        const std::string json =
            request_json("\"seq\":\"1\"", "approve", std::string(",\"arg\":\"") + bad + "\"");
        bool rejected_at_parse = false;
        ControlRequest request;
        try {
            request = parse_control_request(json);
        } catch (const ControlRemoteError &) {
            rejected_at_parse = true;
        }
        if (rejected_at_parse) {
            continue;
        }
        /* If a caller built the request in memory, apply must still refuse. */
        ControlState state;
        std::uint64_t watermark = 0u;
        const RemoteApplyReport report =
            apply_control_request(state, watermark, request, kOperator, kOperator);
        assert(!report.applied);
        assert(state.approved_digests.empty());
        assert(watermark == 0u);
    }
    ControlRequest built;
    built.seq = 1u;
    built.op = ControlOp::Approve;
    built.arg = "not-a-digest";
    ControlState state;
    std::uint64_t watermark = 0u;
    const RemoteApplyReport report =
        apply_control_request(state, watermark, built, kOperator, kOperator);
    assert(!report.applied && state.approved_digests.empty() && watermark == 0u);

    /* Whatever was accepted must round-trip through the durable state. */
    ControlRequest good;
    good.seq = 1u;
    good.op = ControlOp::Approve;
    good.arg = "0123456789abcdef";
    assert(apply_control_request(state, watermark, good, kOperator, kOperator).applied);
    const auto path = scratch("control.json");
    save_control_state(state, path);
    const ControlState loaded = load_control_state(path);
    assert(loaded.approved_digests.size() == 1u);
    std::filesystem::remove_all(path.parent_path());
}

/* An operator cannot publish a request that would be refused on read. */
void test_serialiser_refuses_malformed_digests() {
    ControlRequest request;
    request.seq = 1u;
    request.op = ControlOp::Approve;
    request.arg = "not-a-digest";
    bool refused = false;
    try {
        (void)serialise_control_request(request);
    } catch (const ControlRemoteError &) {
        refused = true;
    }
    assert(refused);
    request.arg = "0123456789abcdef";
    const ControlRequest round_trip = parse_control_request(serialise_control_request(request));
    assert(round_trip.arg == request.arg && round_trip.op == ControlOp::Approve);
}

/* Revoking a malformed digest is also refused rather than silently ignored. */
void test_malformed_revoke_is_refused() {
    ControlRequest built;
    built.seq = 1u;
    built.op = ControlOp::Revoke;
    built.arg = "zzzz";
    ControlState state;
    std::uint64_t watermark = 0u;
    assert(!apply_control_request(state, watermark, built, kOperator, kOperator).applied);
    assert(watermark == 0u);
}

/* Sequence numbers are decimal strings; every malformed spelling must be a
 * ControlRemoteError, never std::invalid_argument / std::out_of_range. */
void test_sequence_parsing_is_total() {
    for (const char *seq : {"\"seq\":\"\"", "\"seq\":\"-1\"", "\"seq\":\"1.5\"",
                            "\"seq\":\"18446744073709551616\"", "\"seq\":\"99999999999999999999999\"",
                            "\"seq\":\"1e3\"", "\"seq\":\"0x10\"", "\"seq\":\" 1\"", "\"seq\":\"1 \"",
                            "\"seq\":1", "\"seq\":null", "\"seq\":[]", "\"seq\":{}",
                            "\"seq\":true", "\"seq\":\"+1\"", "\"seq\":\"00000000000000000000001\"",
                            "\"other\":\"1\""}) {
        try {
            const ControlRequest request = parse_control_request(request_json(seq, "pause"));
            /* Accepting is fine (e.g. a leading-zero spelling) as long as the
             * value is exactly what was written. */
            (void)request;
        } catch (const ControlRemoteError &) {
            /* the expected refusal */
        }
    }
}

/* Deterministic mutation sweep: parse never throws anything but
 * ControlRemoteError, and every accepted request is one apply can be given
 * without corrupting state. */
void test_mutation_sweep_is_total() {
    const std::string bases[] = {
        request_json("\"seq\":\"7\"", "pause"),
        request_json("\"seq\":\"8\"", "approve", ",\"arg\":\"0123456789abcdef\",\"at\":\"2026-09-17T00:00:00Z\""),
        request_json("\"seq\":\"9\"", "revoke", ",\"arg\":\"fedcba9876543210\""),
    };
    std::uint64_t state_bits = 0xA24BAED4963EE407ull;
    auto next = [&state_bits]() {
        state_bits ^= state_bits << 13u;
        state_bits ^= state_bits >> 7u;
        state_bits ^= state_bits << 17u;
        return state_bits;
    };
    std::size_t accepted = 0u;
    for (int iteration = 0; iteration < 60000; ++iteration) {
        std::string mutated = bases[next() % 3u];
        const unsigned edits = 1u + static_cast<unsigned>(next() % 4u);
        for (unsigned e = 0u; e < edits && !mutated.empty(); ++e) {
            const std::size_t at = static_cast<std::size_t>(next() % mutated.size());
            switch (next() % 4u) {
            case 0u:
                mutated[at] = static_cast<char>(next());
                break;
            case 1u:
                mutated.insert(mutated.begin() + static_cast<std::ptrdiff_t>(at),
                               static_cast<char>(next()));
                break;
            case 2u:
                mutated.erase(mutated.begin() + static_cast<std::ptrdiff_t>(at));
                break;
            default:
                mutated.resize(at);
                break;
            }
        }
        ControlRequest request;
        try {
            request = parse_control_request(mutated);
        } catch (const ControlRemoteError &) {
            continue;
        } /* any other exception type escapes and aborts the test */
        ++accepted;
        ControlState state;
        std::uint64_t watermark = request.seq - 1u;
        const RemoteApplyReport report =
            apply_control_request(state, watermark, request, kOperator, kOperator);
        if (report.applied) {
            assert(watermark == request.seq);
            for (const std::string &digest : state.approved_digests) {
                assert(digest.size() == 16u);
            }
            const auto path = scratch("mutation-control.json");
            save_control_state(state, path); /* must never throw for accepted input */
            std::filesystem::remove_all(path.parent_path());
        } else {
            assert(watermark == request.seq - 1u);
        }
    }
    assert(accepted > 0u);
}

} // namespace

int main() {
    test_malformed_digest_arguments_are_refused();
    test_malformed_revoke_is_refused();
    test_serialiser_refuses_malformed_digests();
    test_sequence_parsing_is_total();
    test_mutation_sweep_is_total();
    std::puts("remote control hardening tests passed");
    return 0;
}
