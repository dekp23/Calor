// Calor: segment tree over day index.
//
// Each leaf is one day. A day is either "logged" (it holds a vector of K
// nutrient totals) or "unlogged" (nothing was recorded). Every node stores,
// for each of the K nutrients, the sum, min and max over the logged days in
// its range, plus the count of logged days.
//
// Unlogged days use identity values (sum 0, min +inf, max -inf, count 0), so
// a day with no entries never counts as a 0-calorie day in min/avg.
//
// Layout: perfect binary tree in one flat array. Node 1 is the root, node p
// has children 2p and 2p+1, and leaf i lives at node cap + i, where cap is
// the smallest power of two >= n. Per-nutrient values are stored flat:
// sum_[node * k + j].
//
// Costs (n days, k nutrients):
//   build              O(n k)
//   set / clear        O(k log n)
//   query(l, r)        O(k log n)
//   argMax / argMin    O(log n)
//   memory             about 2 * cap * (3k doubles + 1 count)
#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <stdexcept>
#include <vector>

namespace calor {

struct RangeStats {
    std::size_t count = 0;    // logged days in the range
    std::vector<double> sum;  // per nutrient
    std::vector<double> min;  // +inf when count == 0
    std::vector<double> max;  // -inf when count == 0

    double avg(std::size_t j) const {
        return count == 0 ? 0.0 : sum[j] / static_cast<double>(count);
    }
};

class SegmentTree {
public:
    SegmentTree() = default;

    // n days (leaves), k nutrients per day. All days start unlogged.
    SegmentTree(std::size_t n, std::size_t k) { reset(n, k); }

    void reset(std::size_t n, std::size_t k) {
        if (k == 0) throw std::invalid_argument("SegmentTree: k must be > 0");
        n_ = n;
        k_ = k;
        cap_ = 1;
        while (cap_ < n_) cap_ <<= 1;
        const std::size_t nodes = 2 * cap_;
        sum_.assign(nodes * k_, 0.0);
        min_.assign(nodes * k_, kInf);
        max_.assign(nodes * k_, -kInf);
        cnt_.assign(nodes, 0);
    }

    std::size_t size() const { return n_; }
    std::size_t dims() const { return k_; }

    // Bulk build in O(n k). values is n*k long (row per day); logged[i]
    // says whether day i holds data. Faster than n calls to set().
    void build(const std::vector<double>& values, const std::vector<std::uint8_t>& logged) {
        if (values.size() != n_ * k_ || logged.size() != n_)
            throw std::invalid_argument("SegmentTree::build: size mismatch");
        for (std::size_t i = 0; i < n_; ++i) {
            if (logged[i]) writeLeaf(i, &values[i * k_]);
            else clearLeaf(i);
        }
        for (std::size_t p = cap_ - 1; p >= 1; --p) pull(p);
    }

    // Mark day i as logged with the given k values. O(k log n).
    void set(std::size_t i, const std::vector<double>& vals) {
        checkIndex(i);
        if (vals.size() != k_) throw std::invalid_argument("SegmentTree::set: wrong dims");
        writeLeaf(i, vals.data());
        for (std::size_t p = (cap_ + i) >> 1; p >= 1; p >>= 1) pull(p);
    }

    // Mark day i as unlogged. O(k log n).
    void clear(std::size_t i) {
        checkIndex(i);
        clearLeaf(i);
        for (std::size_t p = (cap_ + i) >> 1; p >= 1; p >>= 1) pull(p);
    }

    bool logged(std::size_t i) const {
        checkIndex(i);
        return cnt_[cap_ + i] != 0;
    }

    // Stats over days [l, r], inclusive. O(k log n).
    RangeStats query(std::size_t l, std::size_t r) const {
        RangeStats s;
        s.sum.assign(k_, 0.0);
        s.min.assign(k_, kInf);
        s.max.assign(k_, -kInf);
        if (n_ == 0 || l > r) return s;
        checkIndex(r);
        // Iterative bottom-up walk over half-open [l, r + 1).
        for (std::size_t a = l + cap_, b = r + 1 + cap_; a < b; a >>= 1, b >>= 1) {
            if (a & 1) absorb(s, a++);
            if (b & 1) absorb(s, --b);
        }
        return s;
    }

    // Earliest logged day in [l, r] where nutrient j reaches the range max
    // (or min). Empty if no day in the range is logged. O(log n).
    std::optional<std::size_t> argMax(std::size_t l, std::size_t r, std::size_t j) const {
        return argExtreme(l, r, j, true);
    }
    std::optional<std::size_t> argMin(std::size_t l, std::size_t r, std::size_t j) const {
        return argExtreme(l, r, j, false);
    }

    // Approximate heap use in bytes, for the benchmark report.
    std::size_t memoryBytes() const {
        return (sum_.capacity() + min_.capacity() + max_.capacity()) * sizeof(double) +
               cnt_.capacity() * sizeof(std::uint32_t);
    }

private:
    static constexpr double kInf = std::numeric_limits<double>::infinity();

    std::size_t n_ = 0, k_ = 0, cap_ = 0;
    std::vector<double> sum_, min_, max_;
    std::vector<std::uint32_t> cnt_;

    void checkIndex(std::size_t i) const {
        if (i >= n_) throw std::out_of_range("SegmentTree: day index out of range");
    }

    void writeLeaf(std::size_t i, const double* v) {
        const std::size_t base = (cap_ + i) * k_;
        for (std::size_t j = 0; j < k_; ++j) {
            sum_[base + j] = v[j];
            min_[base + j] = v[j];
            max_[base + j] = v[j];
        }
        cnt_[cap_ + i] = 1;
    }

    void clearLeaf(std::size_t i) {
        const std::size_t base = (cap_ + i) * k_;
        for (std::size_t j = 0; j < k_; ++j) {
            sum_[base + j] = 0.0;
            min_[base + j] = kInf;
            max_[base + j] = -kInf;
        }
        cnt_[cap_ + i] = 0;
    }

    void pull(std::size_t p) {
        const std::size_t a = 2 * p * k_, b = (2 * p + 1) * k_, o = p * k_;
        for (std::size_t j = 0; j < k_; ++j) {
            sum_[o + j] = sum_[a + j] + sum_[b + j];
            min_[o + j] = min_[a + j] < min_[b + j] ? min_[a + j] : min_[b + j];
            max_[o + j] = max_[a + j] > max_[b + j] ? max_[a + j] : max_[b + j];
        }
        cnt_[p] = cnt_[2 * p] + cnt_[2 * p + 1];
    }

    void absorb(RangeStats& s, std::size_t node) const {
        const std::size_t o = node * k_;
        for (std::size_t j = 0; j < k_; ++j) {
            s.sum[j] += sum_[o + j];
            if (min_[o + j] < s.min[j]) s.min[j] = min_[o + j];
            if (max_[o + j] > s.max[j]) s.max[j] = max_[o + j];
        }
        s.count += cnt_[node];
    }

    std::optional<std::size_t> argExtreme(std::size_t l, std::size_t r, std::size_t j,
                                          bool wantMax) const {
        if (n_ == 0 || l > r) return std::nullopt;
        checkIndex(r);
        if (j >= k_) throw std::out_of_range("SegmentTree: nutrient index out of range");
        const RangeStats s = query(l, r);
        if (s.count == 0) return std::nullopt;
        const double target = wantMax ? s.max[j] : s.min[j];
        const long long found = descend(1, 0, cap_ - 1, l, r, j, target, wantMax);
        if (found < 0) return std::nullopt;  // unreachable when count > 0
        return static_cast<std::size_t>(found);
    }

    // Top-down search for the leftmost leaf in [l, r] whose value equals
    // target. Subtrees that cannot contain target are pruned, so only
    // O(log n) nodes are expanded.
    long long descend(std::size_t node, std::size_t nl, std::size_t nr, std::size_t l,
                      std::size_t r, std::size_t j, double target, bool wantMax) const {
        if (nr < l || nl > r) return -1;
        const double v = wantMax ? max_[node * k_ + j] : min_[node * k_ + j];
        if (wantMax ? v < target : v > target) return -1;
        if (nl == nr) return static_cast<long long>(nl);
        const std::size_t mid = nl + (nr - nl) / 2;
        const long long left = descend(2 * node, nl, mid, l, r, j, target, wantMax);
        if (left >= 0) return left;
        return descend(2 * node + 1, mid + 1, nr, l, r, j, target, wantMax);
    }
};

}  // namespace calor
