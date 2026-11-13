// Calor: import foods from a USDA FoodData Central CSV download.
//
// Download a CSV zip from https://fdc.nal.usda.gov/download-datasets,
// unzip it, and point the importer at the folder holding food.csv,
// food_nutrient.csv and (optionally) nutrient.csv. Values in FoodData
// Central are per 100 g, so every imported food has a 100 g serving.
#pragma once

#include <calor/storage.hpp>

#include <cstdint>
#include <cstdio>
#include <map>
#include <string>
#include <vector>

namespace calor {

struct UsdaOptions {
    // food.data_type values to import. Branded foods (about 450,000) are
    // opt-in; the default covers whole and prepared foods.
    std::vector<std::string> data_types{"sr_legacy_food", "foundation_food", "survey_fndds_food"};
};

struct UsdaResult {
    std::vector<Food> foods;                         // ready for Tracker::importFoods
    std::map<std::string, std::size_t> by_type;      // foods kept, per data_type
    std::map<std::string, std::size_t> seen_types;   // every data_type in food.csv, with counts
    std::size_t skipped_no_energy = 0;               // no calorie value at all
    std::size_t skipped_bad_value = 0;               // negative or non-numeric amount
    std::size_t nutrient_rows = 0;                   // food_nutrient.csv rows read
    bool checked_units = false;                      // nutrient.csv was present and matched
};

// Reads the CSVs in `dir`. Throws std::runtime_error when a required file
// or column is missing, or when nutrient.csv shows a mapped id with an
// unexpected unit (which would mean the id mapping no longer holds).
UsdaResult readUsda(const std::string& dir, const UsdaOptions& opts = {});

// Minimal RFC 4180 CSV reader: quoted fields, "" escapes, commas and line
// breaks inside quotes, CRLF, and a UTF-8 byte-order mark on the header.
class CsvReader {
public:
    explicit CsvReader(const std::string& path);
    ~CsvReader();
    CsvReader(const CsvReader&) = delete;
    CsvReader& operator=(const CsvReader&) = delete;

    const std::vector<std::string>& header() const { return header_; }
    // Column index by header name; -1 if absent.
    int column(const std::string& name) const;
    // Reads the next record into `row`; false at end of file.
    bool next(std::vector<std::string>& row);

private:
    std::FILE* f_ = nullptr;
    std::vector<std::string> header_;
    std::string path_;
    int get();
};

}  // namespace calor
