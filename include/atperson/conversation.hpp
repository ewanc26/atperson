#ifndef ATPERSON_CONVERSATION_HPP
#define ATPERSON_CONVERSATION_HPP

#include "atperson/core.h"

#include <cstddef>
#include <cstring>
#include <stdexcept>
#include <string>

namespace atperson {

/* Stable conversational identifiers for one observation (issue #24).
 * Planning metadata, not learnable content: the reply root/parent URIs and
 * quote target identify what a post responds to without flattening thread
 * structure into text. Context rides with the durable ledger entry and its
 * snapshot mirror for later planning and audit; it is never trained on.
 * Empty strings mean the identifier is absent (a top-level post, a
 * non-quote, or a parent deleted before the fetch). */
struct ConversationContext {
    std::string reply_root_uri;
    std::string reply_parent_uri;
    std::string quote_uri;
};

/* Convert string context to the fixed-size C struct. Throws
 * std::runtime_error when any URI does not fit
 * ATPERSON_CONTEXT_URI_BYTES - 1 (the C core rejects it, so fail loudly
 * here rather than truncating). */
inline atp_conversation_context to_c_conversation_context(const ConversationContext &context) {
    atp_conversation_context raw = {};
    const auto copy_uri = [](char *dst, std::size_t capacity, const std::string &uri) {
        if (uri.size() >= capacity) {
            return false;
        }
        std::memcpy(dst, uri.data(), uri.size());
        dst[uri.size()] = '\0';
        return true;
    };
    if (!copy_uri(raw.reply_root_uri, sizeof(raw.reply_root_uri), context.reply_root_uri) ||
        !copy_uri(raw.reply_parent_uri, sizeof(raw.reply_parent_uri), context.reply_parent_uri) ||
        !copy_uri(raw.quote_uri, sizeof(raw.quote_uri), context.quote_uri)) {
        throw std::runtime_error("conversation context URI too long");
    }
    return raw;
}

} // namespace atperson

#endif
