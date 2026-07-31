#pragma once
#include <string>
#include <vector>

namespace kami {
namespace re {

// A compiled regular expression. Construct once, then match/search.
struct Regex {
    Regex(const std::string& pattern);
    ~Regex();
    Regex(const Regex&) = delete;
    Regex& operator=(const Regex&) = delete;

    bool ok() const;
    const std::string& err() const;
    int group_count() const;

    // Anchored match at `start`; fills gs/ge (index 0 = whole match). If
    // fullOnly, requires the match to reach the end of text.
    bool matchAt(const std::string& text, int start, std::vector<int>& gs,
                 std::vector<int>& ge, bool fullOnly);
    // First match at or after `from`; returns its start or -1.
    int search(const std::string& text, int from, std::vector<int>& gs, std::vector<int>& ge);

    struct Impl;
    Impl* impl;
};

} // namespace re
} // namespace kami
