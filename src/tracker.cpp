#include <calor/tracker.hpp>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <limits>
#include <stdexcept>

namespace calor {
namespace {

std::int64_t todayDay() {
    using namespace std::chrono;
    return duration_cast<hours>(system_clock::now().time_since_epoch()).count() / 24;
}

// Slack kept around the logged range so ordinary use never forces a rebuild.
constexpr std::int64_t kPastPad = 31;
constexpr std::int64_t kFuturePad = 366;

RangeStats emptyStats() {
    RangeStats s;
    s.sum.assign(kNumNutrients, 0.0);
    s.min.assign(kNumNutrients, std::numeric_limits<double>::infinity());
    s.max.assign(kNumNutrients, -std::numeric_limits<double>::infinity());
    return s;
}

void validate(const Entry& e) {
    if (e.name.empty()) throw std::invalid_argument("entry name is required");
    if (e.nutrients.size() != kNumNutrients) throw std::invalid_argument("wrong nutrient count");
    if (!(e.servings > 0)) throw std::invalid_argument("servings must be positive");
    for (double v : e.nutrients)
        if (!(v >= 0) || v > 1e7) throw std::invalid_argument("nutrient values must be between 0 and 1e7");
}

void validate(const Food& f) {
    if (f.name.empty()) throw std::invalid_argument("food name is required");
    if (f.per_serving.size() != kNumNutrients) throw std::invalid_argument("wrong nutrient count");
    for (double v : f.per_serving)
        if (!(v >= 0) || v > 1e7) throw std::invalid_argument("nutrient values must be between 0 and 1e7");
}

}  // namespace

Tracker::Tracker(const std::string& dbPath) : db_(dbPath) {
    std::lock_guard<std::mutex> lock(mu_);
    const std::int64_t today = todayDay();
    const auto range = db_.dayRange();
    const std::int64_t lo = range ? std::min(range->first, today) : today;
    const std::int64_t hi = range ? std::max(range->second, today) : today;
    rebuildLocked(lo - kPastPad, hi + kFuturePad);
    rebuildTrie();
}

// Rebuild the tree to cover days [lo, hi] from SQLite's daily totals. O(n k).
void Tracker::rebuildLocked(std::int64_t lo, std::int64_t hi) {
    base_ = lo;
    const auto n = static_cast<std::size_t>(hi - lo + 1);
    std::vector<double> values(n * kNumNutrients, 0.0);
    std::vector<std::uint8_t> logged(n, 0);
    for (const auto& [day, totals] : db_.allDailyTotals()) {
        if (day < lo || day > hi) continue;
        const auto i = static_cast<std::size_t>(day - lo);
        logged[i] = 1;
        std::copy(totals.begin(), totals.end(), values.begin() + static_cast<long>(i * kNumNutrients));
    }
    tree_.reset(n, kNumNutrients);
    tree_.build(values, logged);
}

void Tracker::ensureCovers(std::int64_t day) {
    const std::int64_t end = base_ + static_cast<std::int64_t>(tree_.size()) - 1;
    if (day >= base_ && day <= end) return;
    // Rare: a day outside the indexed window. Grow the window and rebuild.
    rebuildLocked(std::min(base_, day - kPastPad), std::max(end, day + kFuturePad));
}

void Tracker::refreshDay(std::int64_t day) {
    ensureCovers(day);
    const auto i = static_cast<std::size_t>(day - base_);
    if (auto total = db_.dayTotal(day)) tree_.set(i, *total);
    else tree_.clear(i);
}

void Tracker::indexFood(const Food& f) {
    trie_.insert(f.name, f.id);
    std::string low = f.name;
    for (char& c : low) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    keys_[f.id] = {std::move(low), !f.fdc_id};
}

void Tracker::rebuildTrie() {
    trie_.clear();
    keys_.clear();
    for (const Food& f : db_.allFoods()) indexFood(f);
}

// ---- Foods ----

Food Tracker::addFood(Food f) {
    validate(f);
    std::lock_guard<std::mutex> lock(mu_);
    f.id = db_.addFood(f);
    indexFood(f);
    return f;
}

std::optional<Food> Tracker::updateFood(const Food& f) {
    validate(f);
    std::lock_guard<std::mutex> lock(mu_);
    const auto old = db_.getFood(f.id);
    if (!old || !db_.updateFood(f)) return std::nullopt;
    trie_.erase(old->name, f.id);
    Food stored = *db_.getFood(f.id);  // keeps fdc_id/source
    indexFood(stored);
    return stored;
}

bool Tracker::deleteFood(std::int64_t id) {
    std::lock_guard<std::mutex> lock(mu_);
    const auto old = db_.getFood(id);
    if (!old || !db_.deleteFood(id)) return false;  // entries keep their own copy
    trie_.erase(old->name, id);
    keys_.erase(id);
    return true;
}

std::optional<Food> Tracker::getFood(std::int64_t id) {
    std::lock_guard<std::mutex> lock(mu_);
    return db_.getFood(id);
}

std::vector<Food> Tracker::searchFoods(const std::string& query, std::size_t limit) {
    std::lock_guard<std::mutex> lock(mu_);
    std::vector<std::int64_t> ids = trie_.match(query);
    const auto words = FoodTrie::words(query);
    const std::string first = words.empty() ? "" : words.front();
    struct Ranked {
        bool user, starts;
        std::size_t len;
        const std::string* name;
        std::int64_t id;
    };
    std::vector<Ranked> ranked;
    ranked.reserve(ids.size());
    for (std::int64_t id : ids) {
        auto it = keys_.find(id);
        if (it == keys_.end()) continue;
        const std::string& n = it->second.lower;
        ranked.push_back({it->second.user, n.compare(0, first.size(), first) == 0, n.size(), &n, id});
    }
    const std::size_t k = std::min(limit, ranked.size());
    std::partial_sort(ranked.begin(), ranked.begin() + static_cast<long>(k), ranked.end(),
                      [](const Ranked& a, const Ranked& b) {
                          if (a.user != b.user) return a.user;
                          if (a.starts != b.starts) return a.starts;
                          if (a.len != b.len) return a.len < b.len;
                          return *a.name < *b.name;
                      });
    std::vector<Food> out;
    for (std::size_t i = 0; i < k; ++i)
        if (auto f = db_.getFood(ranked[i].id)) out.push_back(std::move(*f));
    return out;
}

ImportCounts Tracker::importFoods(const std::vector<Food>& foods) {
    for (const Food& f : foods) validate(f);
    std::lock_guard<std::mutex> lock(mu_);
    const ImportCounts c = db_.upsertFoods(foods);
    rebuildTrie();
    return c;
}

std::vector<Food> Tracker::listFoods(std::size_t limit) {
    std::lock_guard<std::mutex> lock(mu_);
    return db_.listFoods(limit);
}

std::vector<Food> Tracker::allFoods() {
    std::lock_guard<std::mutex> lock(mu_);
    return db_.allFoods();
}

// ---- Entries ----

Entry Tracker::addEntry(Entry e) {
    validate(e);
    std::lock_guard<std::mutex> lock(mu_);
    e.id = db_.addEntry(e);
    refreshDay(e.day);
    return e;
}

std::optional<Entry> Tracker::logFood(std::int64_t day, std::int64_t foodId, double servings,
                                      const std::string& meal, const std::string& notes) {
    const auto food = getFood(foodId);
    if (!food) return std::nullopt;
    Entry e;
    e.day = day;
    e.food_id = foodId;
    e.servings = servings;
    e.name = food->name;
    e.meal = meal;
    e.notes = notes;
    for (std::size_t j = 0; j < kNumNutrients; ++j) e.nutrients[j] = food->per_serving[j] * servings;
    return addEntry(e);
}

std::optional<Entry> Tracker::updateEntry(const Entry& e) {
    validate(e);
    std::lock_guard<std::mutex> lock(mu_);
    const auto old = db_.getEntry(e.id);
    if (!old || !db_.updateEntry(e)) return std::nullopt;
    refreshDay(old->day);
    if (e.day != old->day) refreshDay(e.day);  // entry moved to another day
    return e;
}

bool Tracker::deleteEntry(std::int64_t id) {
    std::lock_guard<std::mutex> lock(mu_);
    const auto old = db_.getEntry(id);
    if (!old || !db_.deleteEntry(id)) return false;
    refreshDay(old->day);
    return true;
}

std::vector<Entry> Tracker::entriesOnDay(std::int64_t day) {
    std::lock_guard<std::mutex> lock(mu_);
    return db_.entriesOnDay(day);
}

void Tracker::addEntriesBulk(const std::vector<Entry>& es) {
    for (const Entry& e : es) validate(e);
    std::lock_guard<std::mutex> lock(mu_);
    db_.addEntries(es);
    const auto range = db_.dayRange();
    if (!range) return;
    const std::int64_t end = base_ + static_cast<std::int64_t>(tree_.size()) - 1;
    rebuildLocked(std::min(base_, range->first - kPastPad), std::max(end, range->second + kFuturePad));
}

// ---- Analytics ----

// Stats over [from, to]; days outside the indexed window are unlogged by
// construction, so the range is clamped instead of rebuilding.
RangeStats Tracker::queryDays(std::int64_t from, std::int64_t to) const {
    const std::int64_t end = base_ + static_cast<std::int64_t>(tree_.size()) - 1;
    const std::int64_t lo = std::max(from, base_), hi = std::min(to, end);
    if (lo > hi) return emptyStats();
    return tree_.query(static_cast<std::size_t>(lo - base_), static_cast<std::size_t>(hi - base_));
}

DayStats Tracker::stats(std::int64_t from, std::int64_t to) {
    if (from > to) throw std::invalid_argument("from must be <= to");
    std::lock_guard<std::mutex> lock(mu_);
    DayStats out;
    out.from = from;
    out.to = to;
    out.stats = queryDays(from, to);
    out.max_day.assign(kNumNutrients, std::nullopt);
    out.min_day.assign(kNumNutrients, std::nullopt);
    const std::int64_t end = base_ + static_cast<std::int64_t>(tree_.size()) - 1;
    const std::int64_t lo = std::max(from, base_), hi = std::min(to, end);
    if (out.stats.count == 0 || lo > hi) return out;
    const auto l = static_cast<std::size_t>(lo - base_), r = static_cast<std::size_t>(hi - base_);
    for (std::size_t j = 0; j < kNumNutrients; ++j) {
        if (auto i = tree_.argMax(l, r, j)) out.max_day[j] = base_ + static_cast<std::int64_t>(*i);
        if (auto i = tree_.argMin(l, r, j)) out.min_day[j] = base_ + static_cast<std::int64_t>(*i);
    }
    return out;
}

std::vector<Bucket> Tracker::series(std::int64_t from, std::int64_t to, BucketSize size) {
    if (from > to) throw std::invalid_argument("from must be <= to");
    if (to - from > 366 * 50) throw std::invalid_argument("range too large");
    std::lock_guard<std::mutex> lock(mu_);
    std::vector<Bucket> out;
    std::int64_t start = from;
    while (start <= to) {
        std::int64_t end = start;
        if (size == BucketSize::Week) {
            end = start + 6;
        } else if (size == BucketSize::Month) {
            const CivilDate c = civilFromDays(start);
            const std::int64_t next = c.m == 12 ? daysFromCivil(c.y + 1, 1, 1) : daysFromCivil(c.y, c.m + 1, 1);
            end = next - 1;
        }
        end = std::min(end, to);
        out.push_back({start, end, queryDays(start, end)});  // one O(k log n) query per bucket
        start = end + 1;
    }
    return out;
}

std::vector<std::optional<double>> Tracker::rolling(std::int64_t from, std::int64_t to,
                                                    std::size_t window, std::size_t j) {
    if (from > to) throw std::invalid_argument("from must be <= to");
    if (window == 0 || window > 3660) throw std::invalid_argument("window must be 1..3660");
    if (j >= kNumNutrients) throw std::invalid_argument("unknown nutrient");
    if (to - from > 366 * 50) throw std::invalid_argument("range too large");
    std::lock_guard<std::mutex> lock(mu_);
    std::vector<std::optional<double>> out;
    for (std::int64_t d = from; d <= to; ++d) {
        const RangeStats s = queryDays(d - static_cast<std::int64_t>(window) + 1, d);
        out.push_back(s.count ? std::optional<double>(s.avg(j)) : std::nullopt);
    }
    return out;
}

std::optional<std::pair<std::int64_t, std::int64_t>> Tracker::loggedRange() {
    std::lock_guard<std::mutex> lock(mu_);
    return db_.dayRange();
}

Tracker::Info Tracker::info() {
    std::lock_guard<std::mutex> lock(mu_);
    return {base_, tree_.size(), tree_.memoryBytes(), trie_.size()};
}

}  // namespace calor
