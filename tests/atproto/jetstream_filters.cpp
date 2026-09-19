#include "cli/jetstream_filters.hpp"

#include <cassert>
#include <filesystem>
#include <fstream>
#include <stdexcept>

int main() {
    const auto path = std::filesystem::temp_directory_path() / "atperson-jetstream-filters.txt";
    { std::ofstream out(path); out << "# comment\n app.bsky.feed.post\napp.bsky.feed.post\napp.bsky.feed.like # note\n"; }
    const auto filters = atperson::cli::load_jetstream_collections(path);
    assert(filters.size() == 2u);
    assert(filters[0] == "app.bsky.feed.post");
    assert(filters[1] == "app.bsky.feed.like");
    { std::ofstream out(path); out << "app.bsky.feed.post extra\n"; }
    bool rejected = false;
    try { (void)atperson::cli::load_jetstream_collections(path); }
    catch (const std::runtime_error &) { rejected = true; }
    assert(rejected);
    std::filesystem::remove(path);
    return 0;
}
