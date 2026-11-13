// Calor: the nutrient fields every food and entry carries, and date helpers.
#pragma once

#include <array>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <string>
#include <string_view>

namespace calor {

struct NutrientInfo {
    const char* key;    // column / JSON name
    const char* label;  // display name
    const char* unit;
};

// Order matters: index j in every nutrient vector and segment tree node.
// To track a new field, append it here; the SQLite schema is generated
// from this list (existing databases get the column added on open).
inline constexpr std::array<NutrientInfo, 9> kNutrients{{
    {"calories", "Calories", "kcal"},
    {"protein_g", "Protein", "g"},
    {"carbs_g", "Carbohydrates", "g"},
    {"fat_g", "Fat", "g"},
    {"fiber_g", "Fiber", "g"},
    {"sugar_g", "Sugar", "g"},
    {"sodium_mg", "Sodium", "mg"},
    {"sat_fat_g", "Saturated fat", "g"},
    {"water_ml", "Water", "ml"},
}};
inline constexpr std::size_t kNumNutrients = kNutrients.size();

// ---- Dates as day numbers (days since 1970-01-01, proleptic Gregorian) ----
// Algorithms from Howard Hinnant, "chrono-Compatible Low-Level Date
// Algorithms" (public domain).

inline std::int64_t daysFromCivil(std::int64_t y, unsigned m, unsigned d) {
    y -= m <= 2;
    const std::int64_t era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + static_cast<std::int64_t>(doe) - 719468;
}

struct CivilDate {
    std::int64_t y;
    unsigned m, d;
};

inline CivilDate civilFromDays(std::int64_t z) {
    z += 719468;
    const std::int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    const unsigned doe = static_cast<unsigned>(z - era * 146097);
    const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const std::int64_t y = static_cast<std::int64_t>(yoe) + era * 400;
    const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const unsigned mp = (5 * doy + 2) / 153;
    const unsigned d = doy - (153 * mp + 2) / 5 + 1;
    const unsigned m = mp < 10 ? mp + 3 : mp - 9;
    return {y + (m <= 2), m, d};
}

// Parses "YYYY-MM-DD" strictly; rejects impossible dates like 2026-02-30.
inline std::optional<std::int64_t> parseDate(std::string_view s) {
    if (s.size() != 10 || s[4] != '-' || s[7] != '-') return std::nullopt;
    auto num = [&](std::size_t from, std::size_t len) -> std::optional<int> {
        int v = 0;
        for (std::size_t i = from; i < from + len; ++i) {
            if (s[i] < '0' || s[i] > '9') return std::nullopt;
            v = v * 10 + (s[i] - '0');
        }
        return v;
    };
    const auto y = num(0, 4), m = num(5, 2), d = num(8, 2);
    if (!y || !m || !d || *m < 1 || *m > 12 || *d < 1 || *d > 31) return std::nullopt;
    const std::int64_t day = daysFromCivil(*y, static_cast<unsigned>(*m), static_cast<unsigned>(*d));
    const CivilDate back = civilFromDays(day);
    if (back.m != static_cast<unsigned>(*m) || back.d != static_cast<unsigned>(*d))
        return std::nullopt;
    return day;
}

inline std::string formatDate(std::int64_t day) {
    const CivilDate c = civilFromDays(day);
    char buf[48];
    std::snprintf(buf, sizeof buf, "%04lld-%02u-%02u", static_cast<long long>(c.y), c.m, c.d);
    return buf;
}

}  // namespace calor
