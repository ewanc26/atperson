#include "atproto/jetstream_client.hpp"

#include <cassert>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "wolfram/jetstream.h"

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
        atperson::JetstreamClient client(
            "wss://jetstream.example/subscribe", "",
            {"app.bsky.feed.post"});
        assert(client.cursor().empty());
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
                "not-a-did",
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

    {
        /* More than the four Jetstream v2 kinds must reject up front. */
        std::vector<std::string> kinds{"commit", "identity", "account", "sync",
                                       "commit"};
        bool threw = false;
        try {
            atperson::JetstreamClient client(
                "wss://jetstream.example/subscribe", "did:plc:self", {}, {},
                "", std::move(kinds));
            (void)client;
        } catch (const std::runtime_error &) {
            threw = true;
        }
        assert(threw);
    }

    {
        /* Four distinct kinds are accepted; compression stays off without a
         * dictionary and reports false before the lazy connect. */
        atperson::JetstreamClient client(
            "wss://jetstream.example/subscribe", "did:plc:self", {}, {},
            "", {"commit", "identity", "account", "sync"});
        assert(!client.compressed());
        assert(client.cursor().empty());
    }

    {
        /* A dictionary on a build without libzstd support must reject rather
         * than silently subscribing uncompressed. When the build does support
         * zstd, construction succeeds and compression is armed. */
        const bool supported = wf_jetstream_zstd_supported() != 0;
        bool threw = false;
        try {
            atperson::JetstreamClient client(
                "wss://jetstream.example/subscribe", "did:plc:self", {}, {},
                "", {}, std::string(16u, 'd'));
            (void)client;
        } catch (const std::runtime_error &) {
            threw = true;
        }
        assert(threw != supported);
    }

    return 0;
}
