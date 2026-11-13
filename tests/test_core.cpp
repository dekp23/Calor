// Tests for the segment tree (against a naive scan), the trie and dates.
#include <calor/food_trie.hpp>
#include <calor/nutrients.hpp>
#include <calor/segment_tree.hpp>

#include <algorithm>
#include <limits>
#include <optional>
#include <random>

#include "test_main.hpp"

using calor::RangeStats;
using calor::SegmentTree;

namespace {

constexpr double kInf = std::numeric_limits<double>::infinity();

// Reference model: plain arrays, every query is a linear scan.
struct Naive {
    std::size_t n, k;
    std::vector<double> v;
    std::vector<std::uint8_t> on;
    Naive(std::size_t n_, std::size_t k_) : n(n_), k(k_), v(n_ * k_, 0.0), on(n_, 0) {}

    RangeStats query(std::size_t l, std::size_t r) const {
        RangeStats s;
        s.sum.assign(k, 0.0);
        s.min.assign(k, kInf);
        s.max.assign(k, -kInf);
        for (std::size_t i = l; i <= r && i < n; ++i) {
            if (!on[i]) continue;
            ++s.count;
            for (std::size_t j = 0; j < k; ++j) {
                const double x = v[i * k + j];
                s.sum[j] += x;
                s.min[j] = std::min(s.min[j], x);
                s.max[j] = std::max(s.max[j], x);
            }
        }
        return s;
    }

    std::optional<std::size_t> arg(std::size_t l, std::size_t r, std::size_t j, bool mx) const {
        std::optional<std::size_t> best;
        for (std::size_t i = l; i <= r; ++i) {
            if (!on[i]) continue;
            const double x = v[i * k + j];
            if (!best || (mx ? x > v[*best * k + j] : x < v[*best * k + j])) best = i;
        }
        return best;
    }
};

void expectSame(const RangeStats& a, const RangeStats& b) {
    CHECK(a.count == b.count);
    for (std::size_t j = 0; j < a.sum.size(); ++j) {
        CHECK_NEAR(a.sum[j], b.sum[j]);
        CHECK_NEAR(a.min[j], b.min[j]);
        CHECK_NEAR(a.max[j], b.max[j]);
    }
}

}  // namespace

TEST(empty_tree_returns_empty_stats) {
    SegmentTree t(10, 3);
    const RangeStats s = t.query(0, 9);
    CHECK(s.count == 0);
    CHECK(s.sum[0] == 0.0);
    CHECK(s.min[1] == kInf);
    CHECK(s.max[2] == -kInf);
    CHECK(!t.argMax(0, 9, 0));
}

TEST(unlogged_days_do_not_count_as_zero) {
    SegmentTree t(5, 1);
    t.set(1, {2000});
    t.set(3, {1500});
    const RangeStats s = t.query(0, 4);
    CHECK(s.count == 2);
    CHECK_NEAR(s.sum[0], 3500);
    CHECK_NEAR(s.min[0], 1500);  // not 0, even though days 0, 2, 4 are empty
    CHECK_NEAR(s.avg(0), 1750);
}

TEST(set_clear_and_overwrite) {
    SegmentTree t(4, 2);
    t.set(2, {100, 10});
    t.set(2, {300, 30});  // overwrite, not add
    CHECK_NEAR(t.query(0, 3).sum[0], 300);
    t.clear(2);
    CHECK(t.query(0, 3).count == 0);
    CHECK(!t.logged(2));
}

TEST(non_power_of_two_sizes) {
    for (std::size_t n : {1u, 2u, 3u, 5u, 7u, 100u, 1000u}) {
        SegmentTree t(n, 1);
        for (std::size_t i = 0; i < n; ++i) t.set(i, {static_cast<double>(i + 1)});
        const RangeStats s = t.query(0, n - 1);
        CHECK(s.count == n);
        CHECK_NEAR(s.sum[0], n * (n + 1) / 2.0);
        CHECK_NEAR(s.max[0], static_cast<double>(n));
        CHECK(*t.argMax(0, n - 1, 0) == n - 1);
        CHECK(*t.argMin(0, n - 1, 0) == 0);
    }
}

TEST(argmax_returns_earliest_tie) {
    SegmentTree t(8, 1);
    t.set(2, {5});
    t.set(5, {9});
    t.set(6, {9});
    CHECK(*t.argMax(0, 7, 0) == 5);
    CHECK(*t.argMax(6, 7, 0) == 6);
    CHECK(*t.argMin(0, 7, 0) == 2);
    CHECK(*t.argMax(0, 4, 0) == 2);  // max outside range must not leak in
}

TEST(randomized_against_naive_scan) {
    std::mt19937_64 rng(212);
    for (int trial = 0; trial < 30; ++trial) {
        const std::size_t n = 1 + rng() % 300, k = 1 + rng() % 4;
        SegmentTree t(n, k);
        Naive ref(n, k);
        // Start from a bulk build with some days logged.
        std::vector<double> vals(n * k);
        std::vector<std::uint8_t> on(n);
        for (std::size_t i = 0; i < n; ++i) {
            on[i] = rng() % 3 != 0;
            for (std::size_t j = 0; j < k; ++j) vals[i * k + j] = static_cast<double>(rng() % 4000) - 200;
        }
        t.build(vals, on);
        ref.v = vals;
        ref.on = on;
        for (int op = 0; op < 400; ++op) {
            const std::size_t i = rng() % n;
            switch (rng() % 4) {
                case 0: {
                    std::vector<double> row(k);
                    for (auto& x : row) x = static_cast<double>(rng() % 4000);
                    t.set(i, row);
                    std::copy(row.begin(), row.end(), ref.v.begin() + static_cast<long>(i * k));
                    ref.on[i] = 1;
                    break;
                }
                case 1:
                    t.clear(i);
                    ref.on[i] = 0;
                    break;
                default: {
                    std::size_t l = rng() % n, r = rng() % n;
                    if (l > r) std::swap(l, r);
                    expectSame(t.query(l, r), ref.query(l, r));
                    const std::size_t j = rng() % k;
                    CHECK(t.argMax(l, r, j) == ref.arg(l, r, j, true));
                    CHECK(t.argMin(l, r, j) == ref.arg(l, r, j, false));
                }
            }
        }
    }
}

TEST(rejects_bad_input) {
    SegmentTree t(3, 2);
    bool threw = false;
    try { t.set(3, {1, 2}); } catch (const std::out_of_range&) { threw = true; }
    CHECK(threw);
    threw = false;
    try { t.set(0, {1}); } catch (const std::invalid_argument&) { threw = true; }
    CHECK(threw);
}

TEST(trie_prefix_and_word_search) {
    calor::FoodTrie trie;
    trie.insert("Chicken breast", 1);
    trie.insert("Chickpeas", 2);
    trie.insert("Brown rice", 3);
    trie.insert("Rice cake", 4);
    auto r = trie.search("chi");
    CHECK(r.size() == 2);
    CHECK(trie.search("CHICKP") == std::vector<std::int64_t>{2});
    CHECK(trie.search("breast") == std::vector<std::int64_t>{1});  // second word
    const auto rice = trie.search("rice");
    CHECK(rice.size() == 2);  // "brown rice" and "rice cake", no duplicates
    CHECK(trie.search("xyz").empty());
    CHECK(trie.search("r", 1).size() == 1);
    trie.erase("Chickpeas", 2);
    CHECK(trie.search("chi") == std::vector<std::int64_t>{1});
}

TEST(date_round_trip) {
    CHECK(calor::parseDate("1970-01-01") == 0);
    CHECK(calor::parseDate("2026-11-13") == calor::daysFromCivil(2026, 11, 13));
    CHECK(!calor::parseDate("2026-02-30"));
    CHECK(!calor::parseDate("2026-13-01"));
    CHECK(!calor::parseDate("26-11-13"));
    CHECK(calor::parseDate("2024-02-29").has_value());  // leap year
    for (std::int64_t d = -1000; d < 30000; d += 37)
        CHECK(calor::parseDate(calor::formatDate(d)) == d);
}

CALOR_TEST_MAIN
