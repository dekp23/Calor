// Calor: the engine. Writes go to SQLite first, then the affected day's
// total is recomputed and pushed into the segment tree. Reads for charts
// come from the tree; reads for individual entries come from SQLite.
// All public methods are thread-safe (one mutex; the HTTP server is
// multithreaded).
#pragma once

#include <calor/food_trie.hpp>
#include <calor/segment_tree.hpp>
#include <calor/storage.hpp>

#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace calor {

struct DayStats {
    std::int64_t from = 0, to = 0;  // inclusive day range asked for
    RangeStats stats;
    // Per nutrient: day with the highest / lowest total (empty if nothing logged).
    std::vector<std::optional<std::int64_t>> max_day, min_day;
};

struct Bucket {
    std::int64_t start = 0, end = 0;  // inclusive days
    RangeStats stats;
};

enum class BucketSize { Day, Week, Month };

class Tracker {
public:
    explicit Tracker(const std::string& dbPath);

    // Foods
    Food addFood(Food f);
    std::optional<Food> updateFood(const Food& f);
    bool deleteFood(std::int64_t id);
    std::optional<Food> getFood(std::int64_t id);
    // Every query word must prefix a word of the name. Ranked: your own
    // foods first, then names starting with the first query word, then
    // shorter names.
    std::vector<Food> searchFoods(const std::string& query, std::size_t limit);
    ImportCounts importFoods(const std::vector<Food>& foods);  // bulk upsert by fdc_id
    std::vector<Food> allFoods();
    std::vector<Food> listFoods(std::size_t limit);

    // Entries
    Entry addEntry(Entry e);  // e.nutrients are totals for the entry
    std::optional<Entry> logFood(std::int64_t day, std::int64_t foodId, double servings,
                                 const std::string& meal, const std::string& notes);
    std::optional<Entry> updateEntry(const Entry& e);
    bool deleteEntry(std::int64_t id);
    std::vector<Entry> entriesOnDay(std::int64_t day);
    void addEntriesBulk(const std::vector<Entry>& es);  // one transaction, one rebuild

    // Analytics (segment tree)
    DayStats stats(std::int64_t from, std::int64_t to);
    std::vector<Bucket> series(std::int64_t from, std::int64_t to, BucketSize size);
    // Average of nutrient j over the `window` days ending at each day in
    // [from, to], counting only logged days. Empty when none were logged.
    std::vector<std::optional<double>> rolling(std::int64_t from, std::int64_t to,
                                               std::size_t window, std::size_t j);
    std::optional<std::pair<std::int64_t, std::int64_t>> loggedRange();

    struct Info {
        std::int64_t base_day;
        std::size_t days;
        std::size_t memory_bytes;
        std::size_t foods;
    };
    Info info();

private:
    std::mutex mu_;
    Storage db_;
    SegmentTree tree_;
    FoodTrie trie_;
    struct FoodKey {
        std::string lower;  // lowercased name, for ranking
        bool user;          // added by the user (not imported)
    };
    std::unordered_map<std::int64_t, FoodKey> keys_;
    void indexFood(const Food& f);
    std::int64_t base_ = 0;  // day number of tree leaf 0

    void rebuildLocked(std::int64_t lo, std::int64_t hi);
    void ensureCovers(std::int64_t day);
    void refreshDay(std::int64_t day);
    void rebuildTrie();
    RangeStats queryDays(std::int64_t from, std::int64_t to) const;
};

}  // namespace calor
