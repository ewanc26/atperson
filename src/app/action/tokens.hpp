#ifndef ATPERSON_ACTION_TOKENS_HPP
#define ATPERSON_ACTION_TOKENS_HPP

// Shared evidence-token collection for the C++ runtime: the distinct tokens
// of one text under the current schema, in first-appearance order.
//
// Several runtime paths need exactly this reduction over the shared
// public tokenizer (src/core/tokenize.c) — `journal map` (#56), experience
// drives (#148) and action expectations (#149). One owner avoids diverging
// byte-scanner copies. Tokenization never mutates the graph or interns
// vocabulary; this is a pure read-only reduction.
//
// The bounded variant stops the tokenizer scan once the cap is reached, so
// callers that want a small, stable evidence snapshot never pay to scan an
// arbitrarily long input.
//
// Ownership: returns owned vectors of owned strings; no allocations leak.
// Failure modes: none beyond allocation failure.

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace atperson {
namespace action {

/* The distinct tokens of `text`, in first-appearance order. */
[[nodiscard]] std::vector<std::string> distinct_tokens(std::string_view text);

/* The distinct tokens of `text`, in first-appearance order, collecting at
 * most `max_tokens`. An empty text or a zero cap yields an empty vector. */
[[nodiscard]] std::vector<std::string> distinct_tokens(std::string_view text,
                                                       std::size_t max_tokens);

} // namespace action
} // namespace atperson

#endif