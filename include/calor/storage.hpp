// Calor: SQLite storage. The database is the source of truth; the segment
// tree is an in-memory index rebuilt from it.
#pragma once

#include <calor/nutrients.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

struct sqlite3;

namespace calor {

using Nutrients = std::vector<double>;  // kNumNutrients values, in kNutrients order

struct Food {
    std::int64_t id = 0;
    std::string name;
    std::string serving;  // e.g. "100 g", "1 cup"
    Nutrients per_serving = Nutrients(kNumNutrients, 0.0);
};

struct Entry {
    std::int64_t id = 0;
    std::int64_t day = 0;                 // days since 1970-01-01
    std::optional<std::int64_t> food_id;  // set when logged from a saved food
    double servings = 1.0;
    std::string name;
    // Totals for this entry (servings already applied). Stored on the entry
    // so editing a food later never rewrites history.
    Nutrients nutrients = Nutrients(kNumNutrients, 0.0);
    std::string meal;  // breakfast / lunch / dinner / snack, free text
    std::string notes;
};

class Storage {
public:
    // Opens (creating if needed) the database at `path`. ":memory:" works.
    explicit Storage(const std::string& path);
    ~Storage();
    Storage(const Storage&) = delete;
    Storage& operator=(const Storage&) = delete;

    // Foods
    std::int64_t addFood(const Food& f);
    bool updateFood(const Food& f);
    bool deleteFood(std::int64_t id);
    std::optional<Food> getFood(std::int64_t id) const;
    std::vector<Food> allFoods() const;

    // Entries
    std::int64_t addEntry(const Entry& e);
    bool updateEntry(const Entry& e);
    bool deleteEntry(std::int64_t id);
    std::optional<Entry> getEntry(std::int64_t id) const;
    std::vector<Entry> entriesOnDay(std::int64_t day) const;

    // Daily totals (what the segment tree indexes)
    std::optional<Nutrients> dayTotal(std::int64_t day) const;  // empty if nothing logged
    std::vector<std::pair<std::int64_t, Nutrients>> allDailyTotals() const;
    std::optional<std::pair<std::int64_t, std::int64_t>> dayRange() const;

    // Bulk insert in one transaction (demo seeding, imports).
    void addEntries(const std::vector<Entry>& es);

    // Run a plain SQL statement (used by the benchmark and migrations).
    void exec(const std::string& sql);
    sqlite3* handle() const { return db_; }

private:
    sqlite3* db_ = nullptr;
    void migrate();
};

}  // namespace calor
