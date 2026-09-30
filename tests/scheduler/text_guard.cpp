/* Output guard for autonomous text: each rule, the false positives that would
 * make a guard useless (a denylist term inside a longer word), the repeat
 * window, and the denylist file format. Pure and offline. */

#include "scheduler/text_guard.hpp"

#include <cassert>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <unistd.h>

namespace {

using namespace atperson;

constexpr std::int64_t kNow = 1789639200;

TextRefusal refusal(std::string_view text, const TextGuardConfig &config = {},
                    const std::vector<RecentText> &recent = {}) {
    return check_output_text(text, config, recent, kNow).refusal;
}

void test_plain_text_is_allowed() {
    assert(refusal("the silver moon over the quiet harbour") == TextRefusal::None);
    assert(refusal("beta") == TextRefusal::None);
    /* A lone @ or # with nothing attached is not a mention or a tag. */
    assert(refusal("a @ b") == TextRefusal::None);
    assert(refusal("number # seven") == TextRefusal::None);
    /* Non-ASCII text is fine. */
    assert(refusal("caf\xC3\xA9 \xE6\x9C\x88") == TextRefusal::None);
}

void test_structural_rules() {
    assert(refusal("") == TextRefusal::Empty);
    assert(refusal("   \t\n") == TextRefusal::Empty);

    for (const char *url : {"see https://example.com", "http://x", "go to www.example.org",
                            "visit example.com now", "the site bsky.app rocks", "https",
                            "read the WWW today"}) {
        assert(refusal(url) == TextRefusal::Url);
    }
    /* Ordinary punctuation is not a domain. */
    assert(refusal("wait. what") == TextRefusal::None);
    assert(refusal("a.b") == TextRefusal::None); /* the "tld" is one letter */

    assert(refusal("hello @alice") == TextRefusal::Mention);
    assert(refusal("cc @alice.bsky.social") == TextRefusal::Url || refusal("cc @alice.bsky.social") == TextRefusal::Mention);
    assert(refusal("did:plc:abc") == TextRefusal::Mention);
    assert(refusal("#moon") == TextRefusal::Hashtag);
    assert(refusal("so #blessed") == TextRefusal::Hashtag);
}

void test_length_and_encoding() {
    TextGuardConfig config;
    config.max_codepoints = 10;
    assert(refusal("0123456789", config) == TextRefusal::None);
    assert(refusal("01234567890", config) == TextRefusal::TooLong);
    /* The limit counts characters, not bytes: ten 3-byte characters fit. */
    std::string wide;
    for (int i = 0; i < 10; ++i) {
        wide += "\xE6\x9C\x88";
    }
    assert(wide.size() == 30u && refusal(wide, config) == TextRefusal::None);
    assert(refusal(wide + "\xE6\x9C\x88", config) == TextRefusal::TooLong);

    /* Not valid UTF-8: lone continuation, truncated sequence, overlong, surrogate. */
    for (const char *bad : {"\x80", "abc\xE6\x9C", "\xC0\x80", "\xED\xA0\x80", "\xFF"}) {
        assert(refusal(bad) == TextRefusal::InvalidUtf8);
    }
    assert(refusal(std::string_view("ok\0text", 7)) == TextRefusal::None ||
           refusal(std::string_view("ok\0text", 7)) != TextRefusal::InvalidUtf8);
}

void test_denylist_matches_whole_words_only() {
    TextGuardConfig config;
    config.denied_terms = {"ass", "bad word", "x-y"};
    /* A term inside a longer word is not a match: "class" and "assess" are fine. */
    assert(refusal("a first class assessment", config) == TextRefusal::None);
    assert(refusal("what an ass", config) == TextRefusal::DeniedTerm);
    assert(refusal("ASS", config) == TextRefusal::DeniedTerm);          /* case-insensitive */
    assert(refusal("bad, ass, worse", config) == TextRefusal::DeniedTerm); /* punctuation-delimited */
    /* A phrase (a term with a space) matches as a phrase, not as its words. */
    assert(refusal("a bad word here", config) == TextRefusal::DeniedTerm);
    assert(refusal("a bad and lovely word", config) == TextRefusal::None);
    assert(refusal("the x-y thing", config) == TextRefusal::DeniedTerm);
    const TextVerdict verdict = check_output_text("what an ass", config, {}, kNow);
    assert(verdict.detail == "ass"); /* the reason names the term for the operator */
    /* A non-ASCII neighbour keeps a term from matching inside a longer word. */
    assert(refusal("ass\xC3\xA9", config) == TextRefusal::None);
}

void test_repeat_detection_window_and_normalisation() {
    TextGuardConfig config;
    config.repeat_window_seconds = 3600;
    const std::vector<RecentText> recent = {{"The Silver  Moon!", kNow - 100}};
    assert(refusal("the silver moon", config, recent) == TextRefusal::RepeatedText);
    assert(refusal("THE SILVER MOON...", config, recent) == TextRefusal::RepeatedText);
    assert(refusal("the silver moons", config, recent) == TextRefusal::None);
    /* Exactly on the boundary still counts; beyond it does not. */
    assert(refusal("the silver moon", config, {{"the silver moon", kNow - 3600}}) ==
           TextRefusal::RepeatedText);
    assert(refusal("the silver moon", config, {{"the silver moon", kNow - 3601}}) ==
           TextRefusal::None);
    config.repeat_window_seconds = 0; /* disabled */
    assert(refusal("the silver moon", config, recent) == TextRefusal::None);

    assert(normalize_for_repeat("  A,  b!!  c ") == "a b c");
    assert(normalize_for_repeat("") == "");
    assert(normalize_for_repeat("...") == "");
}

void test_rule_order_is_stable() {
    /* When several rules apply the earlier one is reported. */
    TextGuardConfig config;
    config.denied_terms = {"bad"};
    assert(refusal("bad https://x.com #tag @a", config) == TextRefusal::Url);
    assert(refusal("bad #tag @a", config) == TextRefusal::Mention);
    assert(refusal("bad #tag", config) == TextRefusal::Hashtag);
    assert(refusal("bad", config) == TextRefusal::DeniedTerm);
    assert(std::string(text_refusal_name(TextRefusal::RepeatedText)) == "repeated_text");
    assert(std::string(text_refusal_name(TextRefusal::None)) == "ok");
}

void test_denylist_file() {
    const auto dir = std::filesystem::temp_directory_path() /
                     ("atperson-textguard-" + std::to_string(::getpid()));
    std::filesystem::create_directories(dir);
    const auto file = dir / "denylist.txt";
    assert(load_denylist(file).empty()); /* missing file = empty list */
    {
        std::ofstream out(file);
        out << "# never say these\n\n  Alpha  \nbeta gamma # trailing comment\r\n\t\nDELTA\n";
    }
    const std::vector<std::string> terms = load_denylist(file);
    assert((terms == std::vector<std::string>{"alpha", "beta gamma", "delta"}));
    std::filesystem::remove_all(dir);
}

} // namespace

int main() {
    test_plain_text_is_allowed();
    test_structural_rules();
    test_length_and_encoding();
    test_denylist_matches_whole_words_only();
    test_repeat_detection_window_and_normalisation();
    test_rule_order_is_stable();
    test_denylist_file();
    std::puts("text guard tests passed");
    return 0;
}
