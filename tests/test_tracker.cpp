// Tests for SQLite storage and the Tracker engine, including that the
// segment tree always agrees with totals computed straight from SQLite.
#include <calor/tracker.hpp>

#include <cstdio>
#include <random>
#include <string>
#include <unistd.h>

#include "test_main.hpp"

using namespace calor;

namespace {

Nutrients kcal(double c, double protein = 0) {
    Nutrients n(kNumNutrients, 0.0);
    n[0] = c;
    n[1] = protein;
    return n;
}

Entry entry(std::int64_t day, double c, const std::string& name = "x") {
    Entry e;
    e.day = day;
    e.name = name;
    e.nutrients = kcal(c);
    return e;
}

const std::int64_t D = *parseDate("2026-11-01");

}  // namespace

TEST(storage_round_trip) {
    Storage s(":memory:");
    Food f;
    f.name = "Oats";
    f.serving = "40 g";
    f.per_serving = kcal(150, 5);
    const auto id = s.addFood(f);
    const auto got = s.getFood(id);
    CHECK(got && got->name == "Oats" && got->serving == "40 g");
    CHECK_NEAR(got->per_serving[1], 5);

    Entry e = entry(D, 300, "Oats");
    e.food_id = id;
    e.meal = "breakfast";
    const auto eid = s.addEntry(e);
    s.addEntry(entry(D, 200));
    const auto day = s.entriesOnDay(D);
    CHECK(day.size() == 2);
    CHECK(day[0].id == eid && day[0].meal == "breakfast" && day[0].food_id == id);
    CHECK_NEAR(s.dayTotal(D)->at(0), 500);
    CHECK(!s.dayTotal(D + 1));

    // Deleting a food keeps the entry (history is preserved).
    CHECK(s.deleteFood(id));
    CHECK(s.getEntry(eid) && !s.getEntry(eid)->food_id);
}

TEST(tracker_add_edit_delete_updates_tree) {
    Tracker t(":memory:");
    const auto a = t.addEntry(entry(D, 1800));
    t.addEntry(entry(D, 400));
    t.addEntry(entry(D + 2, 2500));

    auto s = t.stats(D, D + 6);
    CHECK(s.stats.count == 2);  // two logged days, not three entries
    CHECK_NEAR(s.stats.sum[0], 4700);
    CHECK_NEAR(s.stats.min[0], 2200);
    CHECK(s.max_day[0] == D + 2);
    CHECK(s.min_day[0] == D);

    // Edit moves an entry to another day: both days must update.
    Entry moved = a;
    moved.day = D + 4;
    CHECK(t.updateEntry(moved).has_value());
    s = t.stats(D, D + 6);
    CHECK(s.stats.count == 3);
    CHECK_NEAR(s.stats.min[0], 400);

    CHECK(t.deleteEntry(a.id));
    s = t.stats(D, D + 6);
    CHECK(s.stats.count == 2);
    CHECK(!t.deleteEntry(a.id));
}

TEST(tracker_log_food_scales_servings) {
    Tracker t(":memory:");
    Food f;
    f.name = "Greek yogurt";
    f.per_serving = kcal(100, 10);
    f = t.addFood(f);
    const auto e = t.logFood(D, f.id, 2.5, "snack", "");
    CHECK(e && e->name == "Greek yogurt");
    CHECK_NEAR(e->nutrients[0], 250);
    CHECK_NEAR(e->nutrients[1], 25);
    CHECK(!t.logFood(D, 9999, 1, "", ""));
    CHECK(t.searchFoods("yog", 5).size() == 1);
}

TEST(tracker_grows_window_for_far_dates) {
    Tracker t(":memory:");
    const std::int64_t far = *parseDate("1999-01-01");
    t.addEntry(entry(far, 1234));
    t.addEntry(entry(D + 5000, 999));
    CHECK_NEAR(t.stats(far, far).stats.sum[0], 1234);
    CHECK(t.stats(far, D + 6000).stats.count == 2);
}

TEST(tracker_series_and_rolling) {
    Tracker t(":memory:");
    for (int i = 0; i < 14; ++i) t.addEntry(entry(D + i, 1000 + 100 * i));
    const auto weeks = t.series(D, D + 13, BucketSize::Week);
    CHECK(weeks.size() == 2);
    CHECK(weeks[0].stats.count == 7);
    const auto months = t.series(*parseDate("2026-10-15"), *parseDate("2026-12-02"), BucketSize::Month);
    CHECK(months.size() == 3);
    CHECK(months[1].start == *parseDate("2026-11-01") && months[1].end == *parseDate("2026-11-30"));
    const auto roll = t.rolling(D, D + 2, 3, 0);
    CHECK(roll.size() == 3);
    CHECK_NEAR(*roll[0], 1000);        // only day D in window
    CHECK_NEAR(*roll[2], 1100);        // (1000+1100+1200)/3
    CHECK(!t.rolling(D - 10, D - 10, 3, 0)[0]);
}

TEST(tracker_rejects_bad_entries) {
    Tracker t(":memory:");
    bool threw = false;
    try { t.addEntry(entry(D, -5)); } catch (const std::invalid_argument&) { threw = true; }
    CHECK(threw);
    threw = false;
    try { t.addEntry(entry(D, 5, "")); } catch (const std::invalid_argument&) { threw = true; }
    CHECK(threw);
}

TEST(tree_matches_sqlite_after_random_ops_and_reopen) {
    char path[] = "/tmp/calor_test_XXXXXX";
    const int fd = mkstemp(path);
    CHECK(fd >= 0);
    close(fd);
    std::mt19937 rng(7);
    {
        Tracker t(path);
        std::vector<std::int64_t> ids;
        for (int op = 0; op < 500; ++op) {
            if (ids.empty() || rng() % 3) {
                ids.push_back(t.addEntry(entry(D + static_cast<int>(rng() % 60), rng() % 900)).id);
            } else {
                const auto k = rng() % ids.size();
                t.deleteEntry(ids[k]);
                ids.erase(ids.begin() + static_cast<long>(k));
            }
        }
    }
    // Reopen: the tree is rebuilt from disk and must agree with SQLite.
    Tracker t(path);
    Storage s(path);
    for (std::int64_t d = D; d < D + 60; ++d) {
        const auto direct = s.dayTotal(d);
        const auto viaTree = t.stats(d, d).stats;
        CHECK(viaTree.count == (direct ? 1u : 0u));
        if (direct) CHECK_NEAR(viaTree.sum[0], (*direct)[0]);
    }
    for (const char* suffix : {"", "-wal", "-shm"}) std::remove((std::string(path) + suffix).c_str());
}

CALOR_TEST_MAIN
