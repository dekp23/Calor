// Calor: trie for food-name search.
//
// Each word of a food name (a run of letters and digits, lowercased) is a
// key. A query is split into words the same way, and a food matches when
// every query word is a prefix of some word in its name, in any order. So
// "cheddar che" finds "Cheese, cheddar, sharp".
//
// Costs, for a query word of length p:
//   insert     O(total name length)
//   prefix     O(p) to reach the node, then O(results) to collect ids
//   match      one prefix lookup per query word, then a set intersection
// Indexing words (not every suffix) keeps the trie small: its size grows
// with the vocabulary, which stays in the tens of thousands of words even
// for the full USDA SR Legacy set.
#pragma once

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
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

    // Lowercased alphanumeric words of `s`.
    static std::vector<std::string> words(std::string_view s) {
        std::vector<std::string> out;
        std::string cur;
        for (char ch : s) {
            const auto c = static_cast<unsigned char>(ch);
            if (std::isalnum(c)) {
                cur += static_cast<char>(std::tolower(c));
            } else if (!cur.empty()) {
                out.push_back(std::move(cur));
                cur.clear();
            }
        }
        if (!cur.empty()) out.push_back(std::move(cur));
        return out;
    }

    void insert(std::string_view name, std::int64_t id) {
        auto ws = words(name);
        std::sort(ws.begin(), ws.end());
        ws.erase(std::unique(ws.begin(), ws.end()), ws.end());
        for (const auto& w : ws) nodes_[walk(w, true)].ids.push_back(id);
        ++size_;
    }

    void erase(std::string_view name, std::int64_t id) {
        for (const auto& w : words(name)) {
            const int n = walk(w, false);
            if (n < 0) continue;
            auto& ids = nodes_[n].ids;
            ids.erase(std::remove(ids.begin(), ids.end(), id), ids.end());
        }
        if (size_ > 0) --size_;
    }

    // Ids with a word starting with `prefix` (one word), at most `limit`,
    // no duplicates, in alphabetical order of the matched word.
    std::vector<std::int64_t> search(std::string_view prefix, std::size_t limit = 10) const {
        std::vector<std::int64_t> out;
        const auto ws = words(prefix);
        if (ws.empty() || limit == 0) return out;
        const int start = walk(ws.front(), false);
        if (start < 0) return out;
        std::unordered_set<std::int64_t> seen;
        collect(start, limit, out, seen);
        return out;
    }

    // Ids matching every word of `query` as a prefix. At most `cap` ids are
    // gathered per query word, which bounds the work for one-letter words.
    std::vector<std::int64_t> match(std::string_view query, std::size_t cap = 50000) const {
        const auto ws = words(query);
        if (ws.empty()) return {};
        std::vector<std::vector<std::int64_t>> sets;
        for (const auto& w : ws) {
            std::vector<std::int64_t> ids;
            const int n = walk(w, false);
            if (n < 0) return {};  // a word nothing starts with: no match
            std::unordered_set<std::int64_t> seen;
            collect(n, cap, ids, seen);
            std::sort(ids.begin(), ids.end());
            sets.push_back(std::move(ids));
        }
        // Intersect, smallest set first.
        std::sort(sets.begin(), sets.end(), [](const auto& a, const auto& b) { return a.size() < b.size(); });
        std::vector<std::int64_t> acc = std::move(sets.front());
        for (std::size_t i = 1; i < sets.size() && !acc.empty(); ++i) {
            std::vector<std::int64_t> next;
            std::set_intersection(acc.begin(), acc.end(), sets[i].begin(), sets[i].end(), std::back_inserter(next));
            acc = std::move(next);
        }
        return acc;
    }

    std::size_t size() const { return size_; }
    std::size_t nodeCount() const { return nodes_.size(); }

private:
    struct Node {
        std::vector<std::pair<char, int>> next;  // sorted by char
        std::vector<std::int64_t> ids;           // foods with this exact word
    };
    std::vector<Node> nodes_;
    std::size_t size_ = 0;

    // Follow `key` from the root; create nodes if `create`, else -1 when absent.
    int walk(const std::string& key, bool create) {
        int cur = 0;
        for (char c : key) {
            auto& nx = nodes_[cur].next;
            auto it = std::lower_bound(nx.begin(), nx.end(), c,
                                       [](const std::pair<char, int>& p, char ch) { return p.first < ch; });
            if (it != nx.end() && it->first == c) {
                cur = it->second;
            } else {
                if (!create) return -1;
                const int fresh = static_cast<int>(nodes_.size());
                nx.insert(it, {c, fresh});
                nodes_.emplace_back();  // may reallocate: hold indices, not references
                cur = fresh;
            }
        }
        return cur;
    }
    int walk(const std::string& key, bool create) const {
        (void)create;
        int cur = 0;
        for (char c : key) {
            const auto& nx = nodes_[cur].next;
            auto it = std::lower_bound(nx.begin(), nx.end(), c,
                                       [](const std::pair<char, int>& p, char ch) { return p.first < ch; });
            if (it == nx.end() || it->first != c) return -1;
            cur = it->second;
        }
        return cur;
    }

    void collect(int node, std::size_t limit, std::vector<std::int64_t>& out,
                 std::unordered_set<std::int64_t>& seen) const {
        // Iterative DFS in alphabetical order (no deep recursion on long words).
        std::vector<int> stack{node};
        while (!stack.empty() && out.size() < limit) {
            const int n = stack.back();
            stack.pop_back();
            for (std::int64_t id : nodes_[n].ids) {
                if (out.size() >= limit) return;
                if (seen.insert(id).second) out.push_back(id);
            }
            const auto& nx = nodes_[n].next;
            for (auto it = nx.rbegin(); it != nx.rend(); ++it) stack.push_back(it->second);
        }
    }
};

}  // namespace calor
