// Calor: trie for food-name search.
//
// Every word start in a food name is inserted, so "chi" finds
// "Chicken breast" and "bre" finds it too. Matching is case-insensitive.
// Lookup walks the prefix in O(p) for a prefix of length p, then collects
// up to `limit` ids from that subtree in alphabetical order.
#pragma once

#include <cctype>
#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace calor {

class FoodTrie {
public:
    FoodTrie() { nodes_.emplace_back(); }

    void clear() {
        nodes_.clear();
        nodes_.emplace_back();
        size_ = 0;
    }

    // Index `name` under `id`. Each word start becomes a searchable key.
    void insert(std::string_view name, std::int64_t id) {
        const std::string low = lower(name);
        for (std::size_t i = 0; i < low.size(); ++i) {
            const bool wordStart = i == 0 || !std::isalnum(static_cast<unsigned char>(low[i - 1]));
            if (wordStart && std::isalnum(static_cast<unsigned char>(low[i])))
                insertKey(std::string_view(low).substr(i), id);
        }
        ++size_;
    }

    // Remove `name`'s keys for `id` (used when a food is renamed or deleted).
    void erase(std::string_view name, std::int64_t id) {
        const std::string low = lower(name);
        for (std::size_t i = 0; i < low.size(); ++i) {
            const bool wordStart = i == 0 || !std::isalnum(static_cast<unsigned char>(low[i - 1]));
            if (!wordStart || !std::isalnum(static_cast<unsigned char>(low[i]))) continue;
            int cur = 0;
            bool found = true;
            for (char c : std::string_view(low).substr(i)) {
                auto it = nodes_[cur].next.find(c);
                if (it == nodes_[cur].next.end()) { found = false; break; }
                cur = it->second;
            }
            if (!found) continue;
            auto& ids = nodes_[cur].ids;
            for (auto it = ids.begin(); it != ids.end(); ++it)
                if (*it == id) { ids.erase(it); break; }
        }
        if (size_ > 0) --size_;
    }

    // Ids whose name has a word starting with `prefix`, at most `limit`,
    // without duplicates, in alphabetical order of the matched key.
    std::vector<std::int64_t> search(std::string_view prefix, std::size_t limit = 10) const {
        std::vector<std::int64_t> out;
        if (limit == 0) return out;
        int cur = 0;
        for (char c : lower(prefix)) {
            auto it = nodes_[cur].next.find(c);
            if (it == nodes_[cur].next.end()) return out;
            cur = it->second;
        }
        std::unordered_set<std::int64_t> seen;
        collect(cur, limit, out, seen);
        return out;
    }

    std::size_t size() const { return size_; }
    std::size_t nodeCount() const { return nodes_.size(); }

private:
    struct Node {
        std::map<char, int> next;       // ordered, so results come out alphabetically
        std::vector<std::int64_t> ids;  // foods whose key ends here
    };
    std::vector<Node> nodes_;
    std::size_t size_ = 0;

    static std::string lower(std::string_view s) {
        std::string out(s);
        for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return out;
    }

    void insertKey(std::string_view key, std::int64_t id) {
        int cur = 0;
        for (char c : key) {
            auto it = nodes_[cur].next.find(c);
            if (it == nodes_[cur].next.end()) {
                const int fresh = static_cast<int>(nodes_.size());
                nodes_.emplace_back();  // may reallocate: index, don't hold refs
                nodes_[cur].next.emplace(c, fresh);
                cur = fresh;
            } else {
                cur = it->second;
            }
        }
        nodes_[cur].ids.push_back(id);
    }

    void collect(int node, std::size_t limit, std::vector<std::int64_t>& out,
                 std::unordered_set<std::int64_t>& seen) const {
        for (std::int64_t id : nodes_[node].ids) {
            if (out.size() >= limit) return;
            if (seen.insert(id).second) out.push_back(id);
        }
        for (const auto& [c, child] : nodes_[node].next) {
            if (out.size() >= limit) return;
            collect(child, limit, out, seen);
        }
    }
};

}  // namespace calor
