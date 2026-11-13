// Calor benchmark: range-query time for three methods over synthetic data.
//
//   linear   scan a plain array of daily totals          O(n) per query
//   sqlite   indexed SELECT SUM/MIN/MAX ... BETWEEN      O(log n + range) per query
//   segtree  calor::SegmentTree::query                   O(k log n) per query
//
// Two query shapes: "random" (both ends uniform, mean length n/3) and
// "window30" (a 30-day window, the typical chart request). Point updates
// are timed for the tree and SQLite (the array updates in O(1)).
// Each timing is the median of --repeats passes.
// Every tree and SQLite answer is checked against the linear scan.
//
//   benchmark [--sizes 1000,10000,100000,1000000] [--queries 2000]
//             [--budget-ms 1500] [--sweep-days 100000] [--csv results.csv]
//             [--repeats 3] [--seed 212]
#include <calor/nutrients.hpp>
#include <calor/segment_tree.hpp>

#include <sqlite3.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <random>
#include <sstream>
#include <string>
#include <vector>

using namespace calor;
using Clock = std::chrono::steady_clock;

namespace {

constexpr std::size_t K = kNumNutrients;
constexpr double kInf = std::numeric_limits<double>::infinity();

struct Data {
    std::size_t n;
    std::vector<double> v;           // n * K
    std::vector<std::uint8_t> on;    // logged?
};

Data generate(std::size_t n, std::mt19937_64& rng) {
    Data d{n, std::vector<double>(n * K), std::vector<std::uint8_t>(n)};
    std::normal_distribution<double> cal(2200, 300);
    for (std::size_t i = 0; i < n; ++i) {
        d.on[i] = rng() % 10 != 0;  // about 90% of days logged
        const double c = std::max(800.0, std::round(cal(rng)));
        for (std::size_t j = 0; j < K; ++j) d.v[i * K + j] = j == 0 ? c : std::round(c * (0.01 + 0.02 * j));
    }
    return d;
}

// Answer used for correctness checks: count, sum/min/max of calories.
struct Answer {
    std::size_t count = 0;
    double sum = 0, mn = kInf, mx = -kInf;
};

bool same(const Answer& a, const Answer& b) {
    auto eq = [](double x, double y) { return x == y || std::fabs(x - y) <= 1e-6 * (1 + std::fabs(x)); };
    return a.count == b.count && eq(a.sum, b.sum) && eq(a.mn, b.mn) && eq(a.mx, b.mx);
}

// Linear scan computes all K nutrients, same work as the other methods.
Answer linearQuery(const Data& d, std::size_t l, std::size_t r) {
    double sum[K] = {}, mn[K], mx[K];
    std::fill(mn, mn + K, kInf);
    std::fill(mx, mx + K, -kInf);
    std::size_t count = 0;
    for (std::size_t i = l; i <= r; ++i) {
        if (!d.on[i]) continue;
        ++count;
        const double* row = &d.v[i * K];
        for (std::size_t j = 0; j < K; ++j) {
            sum[j] += row[j];
            mn[j] = std::min(mn[j], row[j]);
            mx[j] = std::max(mx[j], row[j]);
        }
    }
    return {count, sum[0], mn[0], mx[0]};
}

class Sql {
public:
    explicit Sql(const Data& d) {
        sqlite3_open(":memory:", &db_);
        std::string cols, qs, aggs;
        for (std::size_t j = 0; j < K; ++j) {
            cols += std::string(", ") + kNutrients[j].key + " REAL";
            qs += ", ?";
            aggs += std::string(", TOTAL(") + kNutrients[j].key + "), MIN(" + kNutrients[j].key + "), MAX(" +
                    kNutrients[j].key + ")";
        }
        exec("CREATE TABLE daily (day INTEGER PRIMARY KEY" + cols + ")");
        exec("BEGIN");
        sqlite3_stmt* ins;
        sqlite3_prepare_v2(db_, ("INSERT INTO daily VALUES (?" + qs + ")").c_str(), -1, &ins, nullptr);
        for (std::size_t i = 0; i < d.n; ++i) {
            if (!d.on[i]) continue;  // unlogged days have no row, as in the real schema
            sqlite3_bind_int64(ins, 1, static_cast<sqlite3_int64>(i));
            for (std::size_t j = 0; j < K; ++j) sqlite3_bind_double(ins, static_cast<int>(j + 2), d.v[i * K + j]);
            sqlite3_step(ins);
            sqlite3_reset(ins);
        }
        sqlite3_finalize(ins);
        exec("COMMIT");
        sqlite3_prepare_v2(db_, ("SELECT COUNT(*)" + aggs + " FROM daily WHERE day BETWEEN ? AND ?").c_str(), -1,
                           &q_, nullptr);
        std::string sets;
        for (std::size_t j = 0; j < K; ++j) sets += std::string(j ? ", " : "") + kNutrients[j].key + " = ?";
        sqlite3_prepare_v2(db_, ("INSERT OR REPLACE INTO daily VALUES (?" + qs + ")").c_str(), -1, &up_, nullptr);
    }
    ~Sql() {
        sqlite3_finalize(q_);
        sqlite3_finalize(up_);
        sqlite3_close(db_);
    }

    Answer query(std::size_t l, std::size_t r) {
        sqlite3_bind_int64(q_, 1, static_cast<sqlite3_int64>(l));
        sqlite3_bind_int64(q_, 2, static_cast<sqlite3_int64>(r));
        sqlite3_step(q_);
        Answer a;
        a.count = static_cast<std::size_t>(sqlite3_column_int64(q_, 0));
        a.sum = sqlite3_column_double(q_, 1);
        if (a.count) {
            a.mn = sqlite3_column_double(q_, 2);
            a.mx = sqlite3_column_double(q_, 3);
        }
        sqlite3_reset(q_);
        return a;
    }

    void update(std::size_t i, const double* row) {
        sqlite3_bind_int64(up_, 1, static_cast<sqlite3_int64>(i));
        for (std::size_t j = 0; j < K; ++j) sqlite3_bind_double(up_, static_cast<int>(j + 2), row[j]);
        sqlite3_step(up_);
        sqlite3_reset(up_);
    }

private:
    sqlite3* db_ = nullptr;
    sqlite3_stmt* q_ = nullptr;
    sqlite3_stmt* up_ = nullptr;
    void exec(const std::string& s) { sqlite3_exec(db_, s.c_str(), nullptr, nullptr, nullptr); }
};

struct Result {
    std::size_t n;
    std::string method, shape;
    double ns_per_op;
    std::size_t ops;
};

// Runs fn over the query list until all are done or the time budget runs
// out; returns mean nanoseconds per call.
template <class F>
std::pair<double, std::size_t> timeOnce(const std::vector<std::pair<std::size_t, std::size_t>>& qs,
                                           double budgetMs, F fn) {
    const auto t0 = Clock::now();
    std::size_t done = 0;
    for (const auto& [l, r] : qs) {
        fn(l, r);
        ++done;
        if ((done & 15) == 0 &&
            std::chrono::duration<double, std::milli>(Clock::now() - t0).count() > budgetMs)
            break;
    }
    const double ns = std::chrono::duration<double, std::nano>(Clock::now() - t0).count();
    return {ns / static_cast<double>(done), done};
}

int gRepeats = 3;

// Median of gRepeats timed passes; reduces noise from the OS scheduler.
template <class F>
std::pair<double, std::size_t> timeQueries(const std::vector<std::pair<std::size_t, std::size_t>>& qs,
                                           double budgetMs, F fn) {
    std::vector<std::pair<double, std::size_t>> runs;
    for (int i = 0; i < gRepeats; ++i) runs.push_back(timeOnce(qs, budgetMs / gRepeats, fn));
    std::sort(runs.begin(), runs.end());
    return runs[runs.size() / 2];
}

std::string human(double ns) {
    char buf[32];
    if (ns < 1e3) std::snprintf(buf, sizeof buf, "%.0f ns", ns);
    else if (ns < 1e6) std::snprintf(buf, sizeof buf, "%.1f us", ns / 1e3);
    else std::snprintf(buf, sizeof buf, "%.2f ms", ns / 1e6);
    return buf;
}

}  // namespace

int main(int argc, char** argv) {
    std::vector<std::size_t> sizes{1000, 10000, 100000, 1000000};
    std::size_t queries = 2000;
    double budgetMs = 1500;
    std::size_t sweepN = 100000;
    std::string csv;
    unsigned seed = 212;
    for (int i = 1; i + 1 < argc; i += 2) {
        const std::string a = argv[i], v = argv[i + 1];
        if (a == "--sizes") {
            sizes.clear();
            std::stringstream ss(v);
            for (std::string t; std::getline(ss, t, ',');) sizes.push_back(std::stoul(t));
        } else if (a == "--queries") queries = std::stoul(v);
        else if (a == "--budget-ms") budgetMs = std::stod(v);
        else if (a == "--csv") csv = v;
        else if (a == "--repeats") gRepeats = std::max(1, std::stoi(v));
        else if (a == "--sweep-days") sweepN = std::stoul(v);
        else if (a == "--seed") seed = static_cast<unsigned>(std::stoul(v));
    }

    std::vector<Result> results;
    std::size_t mismatches = 0;
    std::mt19937_64 rng(seed);
    volatile double sink = 0;

    std::printf("%-9s %-9s %-8s %12s %8s\n", "days", "shape", "method", "per op", "ops");
    for (std::size_t n : sizes) {
        const Data d = generate(n, rng);

        auto t0 = Clock::now();
        SegmentTree tree(n, K);
        tree.build(d.v, d.on);
        const double buildMs = std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
        t0 = Clock::now();
        Sql sql(d);
        const double sqlLoadMs = std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
        std::printf("n=%zu  tree build %.1f ms, %.1f MB; SQLite load %.1f ms\n", n, buildMs,
                    static_cast<double>(tree.memoryBytes()) / 1048576.0, sqlLoadMs);

        for (const std::string shape : {"random", "window30"}) {
            std::vector<std::pair<std::size_t, std::size_t>> qs(queries);
            for (auto& q : qs) {
                if (shape == "random") {
                    q = {rng() % n, rng() % n};
                    if (q.first > q.second) std::swap(q.first, q.second);
                } else {
                    const std::size_t len = std::min<std::size_t>(30, n);
                    const std::size_t l = rng() % (n - len + 1);
                    q = {l, l + len - 1};
                }
            }

            // Correctness on a sample before timing.
            for (std::size_t i = 0; i < std::min<std::size_t>(200, qs.size()); ++i) {
                const auto [l, r] = qs[i];
                const Answer ref = linearQuery(d, l, r);
                const RangeStats s = tree.query(l, r);
                const Answer t{s.count, s.sum[0], s.min[0], s.max[0]};
                if (!same(ref, t) || !same(ref, sql.query(l, r))) ++mismatches;
            }

            auto run = [&](const std::string& method, auto fn) {
                const auto [ns, ops] = timeQueries(qs, budgetMs, fn);
                results.push_back({n, method, shape, ns, ops});
                std::printf("%-9zu %-9s %-8s %12s %8zu\n", n, shape.c_str(), method.c_str(), human(ns).c_str(), ops);
            };
            run("linear", [&](std::size_t l, std::size_t r) { sink = sink + linearQuery(d, l, r).sum; });
            run("sqlite", [&](std::size_t l, std::size_t r) { sink = sink + sql.query(l, r).sum; });
            run("segtree", [&](std::size_t l, std::size_t r) { sink = sink + tree.query(l, r).sum[0]; });
        }

        // Point updates (edit one day's totals).
        std::vector<double> row(K);
        std::vector<std::pair<std::size_t, std::size_t>> ups(queries);
        for (auto& u : ups) u = {rng() % n, 0};
        auto runUp = [&](const std::string& method, auto fn) {
            const auto [ns, ops] = timeQueries(ups, budgetMs, fn);
            results.push_back({n, method, "update", ns, ops});
            std::printf("%-9zu %-9s %-8s %12s %8zu\n", n, "update", method.c_str(), human(ns).c_str(), ops);
        };
        runUp("sqlite", [&](std::size_t i, std::size_t) {
            row[0] = 2000 + static_cast<double>(i % 500);
            sql.update(i, row.data());
        });
        runUp("segtree", [&](std::size_t i, std::size_t) {
            row[0] = 2000 + static_cast<double>(i % 500);
            tree.set(i, row);
        });
        results.push_back({n, "segtree", "build", buildMs * 1e6, 1});
        std::printf("\n");
    }

    // Range-length sweep at one size: where does the tree overtake the scan?
    {
        const std::size_t n = sweepN;
        const Data d = generate(n, rng);
        SegmentTree tree(n, K);
        tree.build(d.v, d.on);
        Sql sql(d);
        std::printf("Range-length sweep, %zu days\n", n);
        for (std::size_t len : {1, 7, 14, 30, 60, 90, 180, 365, 730, 1825, 3650, 10000, 36500}) {
            if (len > n) continue;
            std::vector<std::pair<std::size_t, std::size_t>> qs(queries);
            for (auto& q : qs) {
                const std::size_t l = rng() % (n - len + 1);
                q = {l, l + len - 1};
            }
            for (std::size_t i = 0; i < std::min<std::size_t>(200, qs.size()); ++i) {
                const auto [l, r] = qs[i];
                const RangeStats st = tree.query(l, r);
                if (!same(linearQuery(d, l, r), {st.count, st.sum[0], st.min[0], st.max[0]})) ++mismatches;
            }
            const std::string shape = "len" + std::to_string(len);
            for (const std::string method : {"linear", "sqlite", "segtree"}) {
                std::pair<double, std::size_t> t;
                if (method == "linear") t = timeQueries(qs, budgetMs, [&](std::size_t l, std::size_t r) { sink = sink + linearQuery(d, l, r).sum; });
                else if (method == "sqlite") t = timeQueries(qs, budgetMs, [&](std::size_t l, std::size_t r) { sink = sink + sql.query(l, r).sum; });
                else t = timeQueries(qs, budgetMs, [&](std::size_t l, std::size_t r) { sink = sink + tree.query(l, r).sum[0]; });
                results.push_back({n, method, shape, t.first, t.second});
                std::printf("%-9zu %-9s %-8s %12s %8zu\n", n, shape.c_str(), method.c_str(), human(t.first).c_str(), t.second);
            }
        }
        std::printf("\n");
    }

    std::printf("Correctness: %zu mismatches against the linear scan.\n", mismatches);
    if (!csv.empty()) {
        std::ofstream out(csv);
        out << "days,method,shape,ns_per_op,ops\n";
        for (const auto& r : results) out << r.n << "," << r.method << "," << r.shape << "," << r.ns_per_op << "," << r.ops << "\n";
        std::printf("Wrote %s\n", csv.c_str());
    }
    return mismatches == 0 ? 0 : 1;
}
