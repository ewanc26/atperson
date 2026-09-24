#include "thought.hpp"

#include "../control/state.hpp"
#include "../state/lock.hpp"
#include "../thought/store.hpp"
#include "config.hpp"

#include <cstdint>
#include <chrono>
#include <ostream>
#include <stdexcept>
#include <string>

namespace atperson {
namespace cli {
namespace {

std::filesystem::path thoughts_path() {
    return data_dir() / "thoughts.jsonl";
}

/* TID-shaped rkey from the current microsecond clock: 13 base-32
 * characters, matching AT Protocol TIDs. Monotonic enough for a local
 * append-only store; collisions are impossible in practice because two
 * thoughts cannot be appended within the same microsecond under the
 * store lock. */
std::string new_thought_id() {
    static constexpr char alphabet[] = "234567abcdefghijklmnopqrstuvwxyz";
    const auto now = std::chrono::system_clock::now().time_since_epoch();
    const std::uint64_t micros =
        static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(now)
                                       .count());
    std::string id(13u, '2');
    for (std::size_t i = 0u; i < 13u; ++i) {
        id[12u - i] = alphabet[(micros >> (5u * static_cast<unsigned>(i))) & 0x1Fu];
    }
    return id;
}

} // namespace

int run_thought_record(std::ostream &out, std::ostream &err, std::string_view text) {
    if (text.empty()) {
        err << "thought: text is required\n";
        return 2;
    }
    try {
        /* The thought store shares the daemon's writer lock discipline:
         * mutations serialise under a lock so a concurrent drain or
         * daemon cycle never sees a torn append. */
        StateLock lock(data_dir() / ".thought-lock");
        Thought thought;
        thought.id = new_thought_id();
        thought.kind = "reflection";
        thought.text = std::string(text);
        thought.at = control_now_rfc3339();
        append_thought(thoughts_path(), thought);
        out << "recorded thought " << thought.id << " (" << thought.text.size()
            << " char(s))\n";
        return 0;
    } catch (const std::exception &error) {
        err << "thought failed: " << error.what() << '\n';
        return 1;
    }
}

int run_thought_list(std::ostream &out, std::ostream &err, const std::filesystem::path &data_dir,
                     std::string_view limit_arg) {
    try {
        const ThoughtContents contents = load_thoughts(data_dir / "thoughts.jsonl");
        std::size_t limit = contents.thoughts.size();
        if (!limit_arg.empty()) {
            const unsigned long long parsed = std::stoull(std::string(limit_arg));
            limit = static_cast<std::size_t>(parsed);
        }
        const std::size_t start =
            contents.thoughts.size() > limit ? contents.thoughts.size() - limit : 0u;
        for (std::size_t i = start; i < contents.thoughts.size(); ++i) {
            const Thought &thought = contents.thoughts[i];
            out << thought.at << " " << thought.id << " [" << thought.kind << "] "
                << thought.text << '\n';
        }
        return 0;
    } catch (const std::exception &error) {
        err << "thought list failed: " << error.what() << '\n';
        return 1;
    }
}

} // namespace cli
} // namespace atperson
