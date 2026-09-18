/* Journal-integrity MAC (#57, scoped): known-answer SHA-256 and HMAC-SHA256
 * vectors, create/verify round-trip, key_hint shape, and negative cases.
 * Offline: the MAC module is pure (no network, no Wolfram).
 *
 * This is a symmetric journal-integrity check, not a badge.blue attestation:
 * it never performs a record-side signature operation and knows no
 * repository mechanics. */

#include "journal/mac.hpp"

#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <string>

namespace {

using atperson::compute_journal_mac_canonical_string;
using atperson::compute_journal_mac_digest;
using atperson::create_journal_mac;
using atperson::JournalMac;
using atperson::JournalMacVerifyResult;
using atperson::verify_journal_mac;

void fail(const char *label) {
    std::fprintf(stderr, "FAIL %s\n", label);
    std::exit(1);
}

/* Known-answer vector: the canonical string for the "abc" fixture embeds the
 * FIPS 180-4 sha256("abc") and starts with the documented MAC prefix. */
void test_canonical_known_answer() {
    const std::string canonical =
        compute_journal_mac_canonical_string("did:plc:example", "3kabc", "abc",
                                             "2026-09-17T00:00:00Z");
    assert(canonical ==
           "atperson-journal-mac-v1:did:plc:example:3kabc:"
           "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad:"
           "2026-09-17T00:00:00Z");

    const std::string again =
        compute_journal_mac_canonical_string("did:plc:example", "3kabc", "abc",
                                             "2026-09-17T00:00:00Z");
    assert(canonical == again);
    std::printf("ok canonical known answer\n");
}

/* Known-answer vector: the digest of the canonical string for the
 * "hello world" fixture, computed independently. */
void test_digest_known_answer() {
    const std::string digest =
        compute_journal_mac_digest("did:plc:example", "3kabc", "hello world",
                                   "2026-09-17T00:00:00Z");
    assert(digest == "296a8c3ea23f24884539351b7e63c7e605bae1be0ef45f5bbae8b7e6516598a5");
    assert(digest.size() == 64u);
    std::printf("ok digest known answer\n");
}

/* Known-answer vector: HMAC-SHA256 of the canonical string with a 64-hex
 * key, computed independently. */
void test_signature_known_answer() {
    const std::string key_hex =
        "00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff";
    const auto mac = create_journal_mac(key_hex, "did:plc:example", "3kabc",
                                        "hello world", "2026-09-17T00:00:00Z");
    assert(mac.has_value());
    assert(mac->mode == "hmac-sha256");
    /* key_hint is the first 8 hex characters, never a fabricated DID. */
    assert(mac->key_hint == key_hex.substr(0, 8));
    assert(mac->digest == "296a8c3ea23f24884539351b7e63c7e605bae1be0ef45f5bbae8b7e6516598a5");
    assert(mac->sig == "3ed4e8746281cfa239b22b3f63618675701dee157db06422bd48d63edd64646e");
    std::printf("ok signature known answer\n");
}

/* An 8-hex (short) key still verifies against itself; key_hint is the whole
 * short key (min(8, len) characters). */
void test_short_key_round_trip() {
    const std::string key_hex = "00112233";
    const auto mac = create_journal_mac(key_hex, "did:plc:example", "3kabc",
                                        "hello world", "2026-09-17T00:00:00Z");
    assert(mac.has_value());
    assert(mac->key_hint == key_hex);
    const JournalMacVerifyResult result =
        verify_journal_mac(*mac, key_hex, "did:plc:example", "3kabc", "hello world",
                           "2026-09-17T00:00:00Z");
    assert(result.valid);
    assert(result.reason_code == "valid");
    std::printf("ok short key round trip\n");
}

/* Empty key disables MAC creation. */
void test_empty_key_disables() {
    assert(!create_journal_mac("", "did:plc:example", "3kabc", "hello world",
                               "2026-09-17T00:00:00Z").has_value());
    std::printf("ok empty key disables\n");
}

/* Verify rejects a tampered text, DID, rkey, created_at and signature. */
void test_verify_rejects_tampered_inputs() {
    const std::string key_hex = "00112233";
    const auto mac =
        create_journal_mac(key_hex, "did:plc:example", "3kabc", "hello world",
                           "2026-09-17T00:00:00Z")
            .value();

    const char *tampered[] = {"text", "did", "rkey", "created_at"};
    for (const char *which : tampered) {
        std::string text = "hello world";
        std::string did = "did:plc:example";
        std::string rkey = "3kabc";
        std::string created_at = "2026-09-17T00:00:00Z";
        if (which == std::string("text")) {
            text = "TAMPERED";
        } else if (which == std::string("did")) {
            did = "did:plc:other";
        } else if (which == std::string("rkey")) {
            rkey = "3kdef";
        } else {
            created_at = "2026-09-18T00:00:00Z";
        }
        const JournalMacVerifyResult result =
            verify_journal_mac(mac, key_hex, did, rkey, text, created_at);
        assert(!result.valid);
        assert(result.reason_code == "digest_mismatch");
    }

    /* A wrong key produces a signature mismatch. */
    const JournalMacVerifyResult wrong_key_result =
        verify_journal_mac(mac, "ffffffffffffffff", "did:plc:example", "3kabc",
                           "hello world", "2026-09-17T00:00:00Z");
    assert(!wrong_key_result.valid);
    assert(wrong_key_result.reason_code == "signature_mismatch");

    /* A tampered signature field is a signature mismatch. */
    JournalMac tampered_sig = mac;
    tampered_sig.sig = std::string(64, 'f');
    const JournalMacVerifyResult bad_sig =
        verify_journal_mac(tampered_sig, key_hex, "did:plc:example", "3kabc",
                           "hello world", "2026-09-17T00:00:00Z");
    assert(!bad_sig.valid);
    assert(bad_sig.reason_code == "signature_mismatch");
    std::printf("ok verify rejects tampered inputs\n");
}

void test_verify_rejects_empty_payload() {
    const JournalMac empty;
    const JournalMacVerifyResult result =
        verify_journal_mac(empty, "0011223344556677", "did:plc:example", "3kabc",
                           "hello world", "2026-09-17T00:00:00Z");
    assert(!result.valid);
    assert(result.reason_code == "no_mac");

    /* An empty payload with no key is still "no_mac": there is nothing to
     * verify before the key is even consulted. */
    const JournalMacVerifyResult missing_key =
        verify_journal_mac(empty, "", "did:plc:example", "3kabc", "hello world",
                           "2026-09-17T00:00:00Z");
    assert(!missing_key.valid);
    assert(missing_key.reason_code == "no_mac");
    std::printf("ok verify rejects empty payload\n");
}

} // namespace

int main() {
    test_canonical_known_answer();
    test_digest_known_answer();
    test_signature_known_answer();
    test_short_key_round_trip();
    test_empty_key_disables();
    test_verify_rejects_tampered_inputs();
    test_verify_rejects_empty_payload();
    std::printf("atperson-journal-mac: all tests passed\n");
    return 0;
}