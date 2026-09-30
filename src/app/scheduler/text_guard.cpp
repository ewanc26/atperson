#include "text_guard.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <stdexcept>
#include <system_error>

namespace atperson {
namespace {

bool is_word_byte(unsigned char c) noexcept {
    /* Bytes >= 0x80 belong to multi-byte characters: treat them as word
     * characters so a term never matches inside a non-ASCII word. */
    return std::isalnum(c) != 0 || c == '_' || c >= 0x80u;
}

std::string lower_ascii(std::string_view text) {
    std::string out(text);
    for (char &c : out) {
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c - 'A' + 'a');
        }
    }
    return out;
}

/* Number of code points, or nullopt when the bytes are not valid UTF-8. */
bool utf8_codepoints(std::string_view text, std::size_t &count) {
    count = 0;
    std::size_t i = 0;
    while (i < text.size()) {
        const auto lead = static_cast<unsigned char>(text[i]);
        std::size_t length = 0;
        std::uint32_t code = 0;
        if (lead < 0x80u) {
            length = 1;
            code = lead;
        } else if ((lead & 0xE0u) == 0xC0u) {
            length = 2;
            code = lead & 0x1Fu;
        } else if ((lead & 0xF0u) == 0xE0u) {
            length = 3;
            code = lead & 0x0Fu;
        } else if ((lead & 0xF8u) == 0xF0u) {
            length = 4;
            code = lead & 0x07u;
        } else {
            return false;
        }
        if (i + length > text.size()) {
            return false;
        }
        for (std::size_t k = 1; k < length; ++k) {
            const auto next = static_cast<unsigned char>(text[i + k]);
            if ((next & 0xC0u) != 0x80u) {
                return false;
            }
            code = (code << 6u) | (next & 0x3Fu);
        }
        /* Overlong encodings, surrogates and out-of-range values are invalid. */
        if ((length == 2 && code < 0x80u) || (length == 3 && code < 0x800u) ||
            (length == 4 && code < 0x10000u) || (code >= 0xD800u && code <= 0xDFFFu) ||
            code > 0x10FFFFu) {
            return false;
        }
        i += length;
        ++count;
    }
    return true;
}

bool has_word_at(std::string_view text, std::size_t pos, std::string_view word) {
    if (text.compare(pos, word.size(), word) != 0) {
        return false;
    }
    const bool left_ok = pos == 0 || !is_word_byte(static_cast<unsigned char>(text[pos - 1]));
    const std::size_t end = pos + word.size();
    const bool right_ok =
        end >= text.size() || !is_word_byte(static_cast<unsigned char>(text[end]));
    return left_ok && right_ok;
}

bool contains_word(std::string_view lowered, std::string_view word) {
    if (word.empty()) {
        return false;
    }
    for (std::size_t pos = lowered.find(word); pos != std::string_view::npos;
         pos = lowered.find(word, pos + 1)) {
        if (has_word_at(lowered, pos, word)) {
            return true;
        }
    }
    return false;
}

/* word.tld shaped: a run of [a-z0-9-] , a dot, then 2+ letters ending the word. */
bool has_domain_shape(std::string_view lowered) {
    for (std::size_t dot = lowered.find('.'); dot != std::string_view::npos;
         dot = lowered.find('.', dot + 1)) {
        if (dot == 0 || dot + 1 >= lowered.size()) {
            continue;
        }
        const auto before = static_cast<unsigned char>(lowered[dot - 1]);
        if (!(std::isalnum(before) != 0 || before == '-')) {
            continue;
        }
        std::size_t end = dot + 1;
        while (end < lowered.size() && std::isalpha(static_cast<unsigned char>(lowered[end])) != 0) {
            ++end;
        }
        const bool ends_word =
            end >= lowered.size() || !is_word_byte(static_cast<unsigned char>(lowered[end]));
        if (end - (dot + 1) >= 2 && ends_word) {
            return true;
        }
    }
    return false;
}

bool followed_by_word(std::string_view text, char marker) {
    for (std::size_t pos = text.find(marker); pos != std::string_view::npos;
         pos = text.find(marker, pos + 1)) {
        if (pos + 1 < text.size() && is_word_byte(static_cast<unsigned char>(text[pos + 1]))) {
            return true;
        }
    }
    return false;
}

bool term_matches(std::string_view lowered, std::string_view term) {
    const bool wordy = std::all_of(term.begin(), term.end(), [](char c) {
        return is_word_byte(static_cast<unsigned char>(c));
    });
    if (wordy) {
        return contains_word(lowered, term);
    }
    return lowered.find(term) != std::string_view::npos; /* phrase */
}

} // namespace

const char *text_refusal_name(TextRefusal refusal) noexcept {
    switch (refusal) {
    case TextRefusal::None: return "ok";
    case TextRefusal::Empty: return "empty";
    case TextRefusal::InvalidUtf8: return "invalid_utf8";
    case TextRefusal::TooLong: return "too_long";
    case TextRefusal::Url: return "url";
    case TextRefusal::Mention: return "mention";
    case TextRefusal::Hashtag: return "hashtag";
    case TextRefusal::DeniedTerm: return "denied_term";
    case TextRefusal::RepeatedText: return "repeated_text";
    }
    return "unknown";
}

std::string normalize_for_repeat(std::string_view text) {
    std::string out;
    bool pending_space = false;
    for (const char raw : text) {
        const auto c = static_cast<unsigned char>(raw);
        if (is_word_byte(c)) {
            if (pending_space && !out.empty()) {
                out.push_back(' ');
            }
            pending_space = false;
            out.push_back(c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : raw);
        } else {
            pending_space = true; /* whitespace and punctuation collapse to one gap */
        }
    }
    return out;
}

TextVerdict check_output_text(std::string_view text, const TextGuardConfig &config,
                              const std::vector<RecentText> &recent, std::int64_t now_epoch) {
    auto refuse = [](TextRefusal reason, std::string detail = {}) {
        return TextVerdict{reason, std::move(detail)};
    };

    const bool blank = std::all_of(text.begin(), text.end(), [](char c) {
        return std::isspace(static_cast<unsigned char>(c)) != 0;
    });
    if (blank) {
        return refuse(TextRefusal::Empty);
    }
    std::size_t codepoints = 0;
    if (!utf8_codepoints(text, codepoints)) {
        return refuse(TextRefusal::InvalidUtf8);
    }
    if (codepoints > config.max_codepoints) {
        return refuse(TextRefusal::TooLong, std::to_string(codepoints) + " > " +
                                                std::to_string(config.max_codepoints));
    }

    const std::string lowered = lower_ascii(text);
    if (lowered.find("://") != std::string::npos || lowered.find("www.") != std::string::npos ||
        contains_word(lowered, "http") || contains_word(lowered, "https") ||
        contains_word(lowered, "www") || has_domain_shape(lowered)) {
        return refuse(TextRefusal::Url);
    }
    if (followed_by_word(text, '@') || lowered.find("did:") != std::string::npos) {
        return refuse(TextRefusal::Mention);
    }
    if (followed_by_word(text, '#')) {
        return refuse(TextRefusal::Hashtag);
    }
    for (const std::string &term : config.denied_terms) {
        if (!term.empty() && term_matches(lowered, term)) {
            return refuse(TextRefusal::DeniedTerm, term);
        }
    }

    if (config.repeat_window_seconds > 0) {
        const std::string normalized = normalize_for_repeat(text);
        for (const RecentText &earlier : recent) {
            if (now_epoch - earlier.at_epoch <= config.repeat_window_seconds &&
                normalize_for_repeat(earlier.text) == normalized) {
                return refuse(TextRefusal::RepeatedText);
            }
        }
    }
    return {};
}

std::vector<std::string> load_denylist(const std::filesystem::path &path) {
    std::vector<std::string> terms;
    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) {
        return terms;
    }
    std::ifstream input(path);
    if (!input) {
        throw std::runtime_error("cannot read output denylist " + path.string());
    }
    std::string line;
    while (std::getline(input, line)) {
        const std::size_t hash = line.find('#');
        if (hash != std::string::npos) {
            line.resize(hash);
        }
        const auto first = line.find_first_not_of(" \t\r");
        if (first == std::string::npos) {
            continue;
        }
        const auto last = line.find_last_not_of(" \t\r");
        terms.push_back(lower_ascii(std::string_view(line).substr(first, last - first + 1)));
    }
    return terms;
}

} // namespace atperson
