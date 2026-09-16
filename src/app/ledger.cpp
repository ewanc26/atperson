#include "atperson/ledger.hpp"

#include <stdexcept>
#include <utility>

namespace atperson {

namespace {

void require(atp_status status, std::string_view operation) {
    if (status == ATP_OK) {
        return;
    }
    throw std::runtime_error(std::string(operation) + ": " + atp_status_string(status));
}

LedgerResult to_result(atp_ledger_result result) {
    switch (result) {
    case ATP_LEDGER_NEW:
        return LedgerResult::New;
    case ATP_LEDGER_EXISTS_PENDING:
        return LedgerResult::ExistsPending;
    case ATP_LEDGER_EXISTS_COMMITTED:
        return LedgerResult::ExistsCommitted;
    case ATP_LEDGER_NOT_FOUND:
        break;
    }
    throw std::runtime_error("ledger lookup returned an impossible result");
}

} // namespace

Ledger::Ledger(const std::filesystem::path &path) {
    atp_status status = ATP_OK;
    ledger_ = atp_ledger_open(path.string().c_str(), &status);
    if (!ledger_) {
        require(status, "open observation ledger");
        throw std::runtime_error("open observation ledger: unknown failure");
    }
}

Ledger::~Ledger() {
    atp_ledger_destroy(ledger_);
}

Ledger::Ledger(Ledger &&other) noexcept : ledger_(std::exchange(other.ledger_, nullptr)) {}

Ledger &Ledger::operator=(Ledger &&other) noexcept {
    if (this != &other) {
        atp_ledger_destroy(ledger_);
        ledger_ = std::exchange(other.ledger_, nullptr);
    }
    return *this;
}

std::uint64_t Ledger::digest(std::string_view content) {
    return atp_ledger_digest(content.data(), content.size());
}

LedgerResult Ledger::append(std::string_view source_id, std::string_view author_did,
                            std::uint64_t observed_at, std::uint64_t content_digest,
                            std::uint32_t schema_version, atp_ledger_outcome outcome,
                            std::string_view payload, std::uint64_t *out_id) {
    const std::string owned_source(source_id);
    const std::string owned_author(author_did);
    atp_status status = ATP_OK;
    atp_ledger_result result =
        atp_ledger_append(ledger_, owned_source.c_str(), owned_author.c_str(), observed_at,
                          content_digest, schema_version, outcome, payload.data(),
                          payload.size(), out_id, &status);
    require(status, "append ledger entry");
    return to_result(result);
}

void Ledger::set_outcome(std::uint64_t id, atp_ledger_outcome outcome) {
    require(atp_ledger_set_outcome(ledger_, id, outcome), "set ledger outcome");
}

void Ledger::withdraw(std::uint64_t id) {
    require(atp_ledger_withdraw(ledger_, id), "withdraw ledger entry");
}

std::size_t Ledger::withdraw_source(std::string_view source_id) {
    const std::string owned_source(source_id);
    return atp_ledger_withdraw_source(ledger_, owned_source.c_str());
}

std::size_t Ledger::withdraw_author(std::string_view author_did) {
    const std::string owned_author(author_did);
    return atp_ledger_withdraw_author(ledger_, owned_author.c_str());
}

LedgerResult Ledger::lookup(std::string_view source_id, std::uint64_t content_digest,
                            atp_ledger_entry *out_entry) const {
    const std::string owned_source(source_id);
    return to_result(atp_ledger_lookup(ledger_, owned_source.c_str(), content_digest, out_entry));
}

std::uint64_t Ledger::count() const noexcept {
    return atp_ledger_count(ledger_);
}

std::vector<atp_ledger_entry> Ledger::entries() const {
    std::vector<atp_ledger_entry> result(static_cast<std::size_t>(count()));
    for (std::size_t i = 0; i < result.size(); ++i) {
        require(atp_ledger_entry_at(ledger_, i, &result[i]), "read ledger entry");
    }
    return result;
}

std::string Ledger::payload(std::uint64_t id) const {
    std::size_t length = 0u;
    require(atp_ledger_entry_payload(ledger_, id, nullptr, 0u, &length), "query ledger payload");
    if (length == 0u) {
        return {};
    }
    std::string result(length, '\0');
    require(atp_ledger_entry_payload(ledger_, id, result.data(), result.size(), &length),
            "read ledger payload");
    return result;
}

} // namespace atperson