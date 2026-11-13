#include <calor/usda_import.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <unordered_map>

namespace calor {

// ---------------- CSV ----------------

CsvReader::CsvReader(const std::string& path) : path_(path) {
    f_ = std::fopen(path.c_str(), "rb");
    if (!f_) throw std::runtime_error("cannot open " + path + ": " + std::strerror(errno));
    // Skip a UTF-8 byte-order mark before parsing, so a quoted first
    // header field is still recognized as quoted.
    unsigned char bom[3] = {0, 0, 0};
    const std::size_t got = std::fread(bom, 1, 3, f_);
    if (!(got == 3 && bom[0] == 0xEF && bom[1] == 0xBB && bom[2] == 0xBF)) std::rewind(f_);
    if (!next(header_)) throw std::runtime_error(path + " is empty");
}

CsvReader::~CsvReader() {
    if (f_) std::fclose(f_);
}

int CsvReader::get() { return std::getc(f_); }

int CsvReader::column(const std::string& name) const {
    for (std::size_t i = 0; i < header_.size(); ++i)
        if (header_[i] == name) return static_cast<int>(i);
    return -1;
}

bool CsvReader::next(std::vector<std::string>& row) {
    row.clear();
    std::string field;
    bool inQuotes = false, any = false;
    for (;;) {
        const int c = get();
        if (inQuotes) {
            if (c == EOF) throw std::runtime_error(path_ + ": unterminated quoted field");
            if (c == '"') {
                const int d = get();
                if (d == '"') field += '"';  // "" is an escaped quote
                else {
                    inQuotes = false;
                    if (d != EOF) std::ungetc(d, f_);
                }
            } else {
                field += static_cast<char>(c);
            }
            continue;
        }
        if (c == EOF) {
            if (!any) return false;
            row.push_back(std::move(field));
            return true;
        }
        any = true;
        if (c == '\n') {
            if (!field.empty() && field.back() == '\r') field.pop_back();
            row.push_back(std::move(field));
            return true;
        }
        if (c == ',') {
            row.push_back(std::move(field));
            field.clear();
        } else if (c == '"' && field.empty()) {
            inQuotes = true;
        } else {
            field += static_cast<char>(c);
        }
    }
}

// ---------------- USDA mapping ----------------

namespace {

// FoodData Central nutrient ids for each Calor nutrient, in priority order.
// Confirmed against an SR Legacy record (fdc_id 167959) and the Foundation
// Foods documentation: Foundation Foods report energy as Atwater Specific
// (2048) or General (2047) factors instead of "Energy" (1008).
struct Source {
    std::vector<int> ids;
    const char* unit;  // expected nutrient.csv unit_name (case-insensitive)
};

const std::array<Source, kNumNutrients>& sources() {
    static const std::array<Source, kNumNutrients> s{{
        {{1008, 2048, 2047}, "KCAL"},  // calories: Energy, then Atwater specific, then general
        {{1003}, "G"},                 // protein_g: Protein
        {{1005, 1050}, "G"},           // carbs_g: by difference, then by summation
        {{1004}, "G"},                 // fat_g: Total lipid (fat)
        {{1079}, "G"},                 // fiber_g: Fiber, total dietary
        {{2000, 1063}, "G"},           // sugar_g: Total Sugars, then Sugars, Total
        {{1093}, "MG"},                // sodium_mg: Sodium, Na
        {{1258}, "G"},                 // sat_fat_g: Fatty acids, total saturated
        {{1051}, "G"},                 // water_ml: Water (g), stored 1 g = 1 ml
    }};
    return s;
}

std::string sourceLabel(const std::string& type) {
    if (type == "sr_legacy_food") return "USDA SR Legacy";
    if (type == "foundation_food") return "USDA Foundation";
    if (type == "survey_fndds_food") return "USDA FNDDS";
    if (type == "branded_food") return "USDA Branded";
    return "USDA " + type;
}

bool iequals(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i)
        if (std::toupper(static_cast<unsigned char>(a[i])) != std::toupper(static_cast<unsigned char>(b[i])))
            return false;
    return true;
}

int require(const CsvReader& r, const std::string& file, const char* col) {
    const int i = r.column(col);
    if (i < 0) throw std::runtime_error(file + " has no column '" + col + "'");
    return i;
}

bool parseInt(const std::string& s, std::int64_t& out) {
    if (s.empty()) return false;
    char* end = nullptr;
    errno = 0;
    const long long v = std::strtoll(s.c_str(), &end, 10);
    if (errno || *end) return false;
    out = v;
    return true;
}

std::string joinPath(const std::string& dir, const char* file) {
    if (dir.empty()) return file;
    const char last = dir.back();
    return (last == '/' || last == '\\') ? dir + file : dir + "/" + file;
}

bool exists(const std::string& path) {
    if (std::FILE* f = std::fopen(path.c_str(), "rb")) {
        std::fclose(f);
        return true;
    }
    return false;
}

}  // namespace

UsdaResult readUsda(const std::string& dir, const UsdaOptions& opts) {
    UsdaResult res;
    const auto& src = sources();

    // Which nutrient id feeds which Calor field, and at what priority.
    struct Slot {
        std::size_t field;
        std::size_t rank;
    };
    std::unordered_map<std::int64_t, Slot> slotOf;
    std::size_t maxRank = 0;
    for (std::size_t j = 0; j < kNumNutrients; ++j)
        for (std::size_t r = 0; r < src[j].ids.size(); ++r) {
            slotOf[src[j].ids[r]] = {j, r};
            maxRank = std::max(maxRank, r + 1);
        }

    // 1. nutrient.csv (optional): guard against a changed id mapping.
    const std::string nutrientPath = joinPath(dir, "nutrient.csv");
    if (exists(nutrientPath)) {
        CsvReader r(nutrientPath);
        const int cId = require(r, "nutrient.csv", "id"), cUnit = require(r, "nutrient.csv", "unit_name");
        const int cName = r.column("name");
        std::vector<std::string> row;
        while (r.next(row)) {
            std::int64_t id;
            if (static_cast<int>(row.size()) <= std::max(cId, cUnit) || !parseInt(row[cId], id)) continue;
            auto it = slotOf.find(id);
            if (it == slotOf.end()) continue;
            if (!iequals(row[cUnit], src[it->second.field].unit))
                throw std::runtime_error("nutrient.csv: id " + std::to_string(id) + " (" +
                                         (cName >= 0 ? row[cName] : std::string("?")) + ") has unit " +
                                         row[cUnit] + ", expected " + src[it->second.field].unit +
                                         ". The FoodData Central nutrient ids may have changed.");
        }
        res.checked_units = true;
    }

    // 2. food.csv: which foods to import.
    const std::vector<std::string>& want = opts.data_types;
    struct Pending {
        std::string name, type;
        // values[field * maxRank + rank], NaN = not reported
        std::vector<double> values;
        bool bad = false;
    };
    std::vector<Pending> pending;
    std::unordered_map<std::int64_t, std::size_t> indexOf;
    std::vector<std::int64_t> fdcIds;
    {
        CsvReader r(joinPath(dir, "food.csv"));
        const int cId = require(r, "food.csv", "fdc_id"), cType = require(r, "food.csv", "data_type"),
                  cDesc = require(r, "food.csv", "description");
        const int need = std::max({cId, cType, cDesc});
        std::vector<std::string> row;
        while (r.next(row)) {
            if (static_cast<int>(row.size()) <= need) continue;
            ++res.seen_types[row[cType]];
            if (std::find(want.begin(), want.end(), row[cType]) == want.end()) continue;
            std::int64_t id;
            if (!parseInt(row[cId], id) || row[cDesc].empty()) continue;
            indexOf[id] = pending.size();
            fdcIds.push_back(id);
            pending.push_back({row[cDesc], row[cType],
                               std::vector<double>(kNumNutrients * maxRank, std::nan("")), false});
        }
    }

    // 2b. branded_food.csv (optional): add the brand to branded names.
    const std::string brandedPath = joinPath(dir, "branded_food.csv");
    if (exists(brandedPath) && std::find(want.begin(), want.end(), "branded_food") != want.end()) {
        CsvReader r(brandedPath);
        const int cId = r.column("fdc_id"), cOwner = r.column("brand_owner"), cName = r.column("brand_name");
        std::vector<std::string> row;
        if (cId >= 0 && (cOwner >= 0 || cName >= 0)) {
            while (r.next(row)) {
                std::int64_t id;
                if (static_cast<int>(row.size()) <= cId || !parseInt(row[cId], id)) continue;
                auto it = indexOf.find(id);
                if (it == indexOf.end()) continue;
                std::string brand;
                if (cName >= 0 && cName < static_cast<int>(row.size())) brand = row[cName];
                if (brand.empty() && cOwner >= 0 && cOwner < static_cast<int>(row.size())) brand = row[cOwner];
                if (!brand.empty()) pending[it->second].name += " (" + brand + ")";
            }
        }
    }

    // 3. food_nutrient.csv: the amounts, per 100 g.
    {
        CsvReader r(joinPath(dir, "food_nutrient.csv"));
        const int cFood = require(r, "food_nutrient.csv", "fdc_id"),
                  cNut = require(r, "food_nutrient.csv", "nutrient_id"),
                  cAmt = require(r, "food_nutrient.csv", "amount");
        const int need = std::max({cFood, cNut, cAmt});
        std::vector<std::string> row;
        while (r.next(row)) {
            ++res.nutrient_rows;
            if (static_cast<int>(row.size()) <= need) continue;
            std::int64_t nid, fid;
            if (!parseInt(row[cNut], nid)) continue;
            auto slot = slotOf.find(nid);
            if (slot == slotOf.end() || !parseInt(row[cFood], fid)) continue;
            auto it = indexOf.find(fid);
            if (it == indexOf.end()) continue;
            Pending& p = pending[it->second];
            const std::string& a = row[cAmt];
            if (a.empty()) continue;  // not reported
            char* end = nullptr;
            const double v = std::strtod(a.c_str(), &end);
            if (*end || !std::isfinite(v) || v < 0) {
                p.bad = true;
                continue;
            }
            p.values[slot->second.field * maxRank + slot->second.rank] = v;
        }
    }

    // 4. Resolve each field to its highest-priority reported value.
    for (std::size_t i = 0; i < pending.size(); ++i) {
        Pending& p = pending[i];
        if (p.bad) { ++res.skipped_bad_value; continue; }
        Food f;
        f.name = p.name;
        f.serving = "100 g";
        f.fdc_id = fdcIds[i];
        f.source = sourceLabel(p.type);
        bool hasEnergy = false;
        for (std::size_t j = 0; j < kNumNutrients; ++j) {
            double v = 0;  // not reported counts as 0
            for (std::size_t rank = 0; rank < src[j].ids.size(); ++rank) {
                const double x = p.values[j * maxRank + rank];
                if (!std::isnan(x)) {
                    v = x;
                    if (j == 0) hasEnergy = true;
                    break;
                }
            }
            f.per_serving[j] = v;
        }
        if (!hasEnergy) { ++res.skipped_no_energy; continue; }
        ++res.by_type[p.type];
        res.foods.push_back(std::move(f));
    }
    return res;
}

}  // namespace calor
