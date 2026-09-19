#include "atproto/jetstream_client.hpp"

#include <cassert>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

int main() {
    {
        atperson::JetstreamClient client(
            "wss://jetstream.example/subscribe",
            "did:plc:self",
            {"app.bsky.feed.post"},
            {},
            "1726200000123456");
        assert(client.cursor() == "1726200000123456");
    }

    {
        bool threw = false;
        try {
            atperson::JetstreamClient client(
                "wss://jetstream.example/subscribe",
                "did:plc:self",
                {"app.bsky.feed.post"},
                {},
                "not-a-decimal-cursor");
            (void)client;
        } catch (const std::runtime_error &) {
            threw = true;
        }
        assert(threw);
    }

    {
        bool threw = false;
        try {
            atperson::JetstreamClient client(
                "wss://jetstream.example/subscribe",
                "",
                {"app.bsky.feed.post"});
            (void)client;
        } catch (const std::runtime_error &) {
            threw = true;
        }
        assert(threw);
    }

    {
        std::vector<std::string> collections(101u, "app.bsky.feed.post");
        bool threw = false;
        try {
            atperson::JetstreamClient client(
                "wss://jetstream.example/subscribe",
                "did:plc:self",
                std::move(collections));
            (void)client;
        } catch (const std::runtime_error &) {
            threw = true;
        }
        assert(threw);
    }

    return 0;
}
