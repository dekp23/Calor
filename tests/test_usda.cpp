// Tests for the USDA FoodData Central importer and ranked food search.
#include <calor/tracker.hpp>
#include <calor/usda_import.hpp>

#include <cstdio>
#include <fstream>
#include <string>
#include <unistd.h>

#include "test_main.hpp"

using namespace calor;

#ifndef CALOR_FIXTURES
#error "CALOR_FIXTURES must point at tests/fixtures"
#endif

namespace {

const std::string kDir = std::string(CALOR_FIXTURES) + "/usda";

const Food* find(const UsdaResult& r, std::int64_t fdc) {
    for (const Food& f : r.foods)
        if (f.fdc_id == fdc) return &f;
    return nullptr;
}

std::string tempDir() {
    char tmpl[] = "/tmp/calor_usda_XXXXXX";
    const char* d = mkdtemp(tmpl);
    return d ? d : "";
}

void write(const std::string& path, const std::string& text) { std::ofstream(path) << text; }

}  // namespace

TEST(csv_reader_handles_quotes_bom_crlf) {
    CsvReader r(kDir + "/food.csv");
    CHECK(r.column("fdc_id") == 0);  // BOM stripped, quotes removed
    CHECK(r.column("description") == 2);
    std::vector<std::string> row;
    int n = 0;
    std::string quoted;
    while (r.next(row)) {
        ++n;
        CHECK(row.size() == 5);  // CR never leaks into the last field
        if (row[0] == "9000004") quoted = row[2];
        CHECK(row.back().find('\r') == std::string::npos);
    }
    CHECK(n == 6);
    CHECK(quoted == "Test \"quoted\" food, with comma\nand line break");
}

TEST(real_sr_legacy_record_maps_exactly) {
    const UsdaResult r = readUsda(kDir);
    CHECK(r.checked_units);
    const Food* f = find(r, 167959);
    CHECK(f != nullptr);
    if (!f) return;
    CHECK(f->name == "Snacks, popcorn, air-popped");
    CHECK(f->serving == "100 g");
    CHECK(f->source == "USDA SR Legacy");
    // Published FoodData Central values per 100 g.
    CHECK_NEAR(f->per_serving[0], 387);    // Energy, kcal (not the kJ row)
    CHECK_NEAR(f->per_serving[1], 12.9);   // Protein
    CHECK_NEAR(f->per_serving[2], 77.8);   // Carbohydrate, by difference
    CHECK_NEAR(f->per_serving[3], 4.54);   // Total lipid (fat)
    CHECK_NEAR(f->per_serving[4], 14.5);   // Fiber
    CHECK_NEAR(f->per_serving[5], 0.87);   // Total Sugars
    CHECK_NEAR(f->per_serving[6], 8);      // Sodium, mg
    CHECK_NEAR(f->per_serving[7], 0.637);  // Saturated fat
    CHECK_NEAR(f->per_serving[8], 3.32);   // Water, g -> ml
}

TEST(type_filter_energy_fallback_and_skips) {
    const UsdaResult r = readUsda(kDir);
    CHECK(r.seen_types.at("sub_sample_food") == 1);
    CHECK(find(r, 9000002) == nullptr);  // excluded data type
    CHECK(find(r, 9000005) == nullptr);  // branded is opt-in
    CHECK(find(r, 9000003) == nullptr);  // no energy reported
    CHECK(r.skipped_no_energy == 1);
    const Food* foundation = find(r, 9000001);
    CHECK(foundation && foundation->source == "USDA Foundation");
    if (foundation) CHECK_NEAR(foundation->per_serving[0], 118);  // Atwater specific beats general
    const Food* quoted = find(r, 9000004);
    CHECK(quoted && quoted->per_serving[3] == 0.0);  // blank amount = not reported
    CHECK(r.foods.size() == 3);
}

TEST(branded_opt_in_adds_brand) {
    UsdaOptions o;
    o.data_types = {"branded_food"};
    const UsdaResult r = readUsda(kDir, o);
    CHECK(r.foods.size() == 1);
    CHECK(!r.foods.empty() && r.foods[0].name == "Test branded bar (TestBrand)");
}

TEST(unit_mismatch_is_refused) {
    const std::string d = tempDir();
    CHECK(!d.empty());
    write(d + "/nutrient.csv", "id,name,unit_name,nutrient_nbr\n1008,Energy,kJ,208\n");
    write(d + "/food.csv", "fdc_id,data_type,description\n1,sr_legacy_food,X\n");
    write(d + "/food_nutrient.csv", "id,fdc_id,nutrient_id,amount\n1,1,1008,100\n");
    bool threw = false;
    try { readUsda(d); } catch (const std::runtime_error& e) { threw = std::string(e.what()).find("1008") != std::string::npos; }
    CHECK(threw);
    for (const char* f : {"/nutrient.csv", "/food.csv", "/food_nutrient.csv"}) std::remove((d + f).c_str());
    rmdir(d.c_str());
}

TEST(missing_column_is_reported) {
    const std::string d = tempDir();
    write(d + "/food.csv", "id,data_type,description\n1,sr_legacy_food,X\n");
    write(d + "/food_nutrient.csv", "id,fdc_id,nutrient_id,amount\n");
    bool threw = false;
    try { readUsda(d); } catch (const std::runtime_error& e) { threw = std::string(e.what()).find("fdc_id") != std::string::npos; }
    CHECK(threw);
    for (const char* f : {"/food.csv", "/food_nutrient.csv"}) std::remove((d + f).c_str());
    rmdir(d.c_str());
}

TEST(import_is_idempotent_and_protects_user_foods) {
    Tracker t(":memory:");
    Food mine;
    mine.name = "Test foundation food, raw";  // same name as an import
    mine.per_serving[0] = 50;
    mine = t.addFood(mine);

    const UsdaResult r = readUsda(kDir);
    ImportCounts c = t.importFoods(r.foods);
    CHECK(c.inserted == 2);
    CHECK(c.name_conflicts == 1);
    CHECK_NEAR(t.getFood(mine.id)->per_serving[0], 50);  // untouched

    c = t.importFoods(r.foods);  // second run updates, adds nothing
    CHECK(c.inserted == 0 && c.updated == 2);
    CHECK(t.allFoods().size() == 3);

    // Logging an imported food: servings are multiples of 100 g.
    const auto popcorn = t.searchFoods("popcorn air", 5);
    CHECK(popcorn.size() == 1);
    if (!popcorn.empty()) {
        const auto e = t.logFood(*parseDate("2026-11-13"), popcorn[0].id, 0.25, "snack", "");
        CHECK(e && calortest::near(e->nutrients[0], 96.75));
    }
}

TEST(search_ranks_user_foods_and_prefix_matches_first) {
    Tracker t(":memory:");
    std::vector<Food> imported;
    const char* names[] = {"Cheese, cheddar, sharp", "Cheese, cheddar", "Crackers, cheese, cheddar flavor",
                           "Soup, cheddar cheese, canned"};
    std::int64_t fdc = 1;
    for (const char* n : names) {
        Food f;
        f.name = n;
        f.serving = "100 g";
        f.fdc_id = fdc++;
        f.source = "USDA SR Legacy";
        f.per_serving[0] = 400;
        imported.push_back(f);
    }
    t.importFoods(imported);
    Food mine;
    mine.name = "My cheddar omelette";
    mine.per_serving[0] = 350;
    t.addFood(mine);

    // Each query word may match any name word, including the same one:
    // "che" prefixes "cheddar", so the omelette matches too.
    CHECK(t.searchFoods("cheddar che", 10).size() == 5);
    CHECK(t.searchFoods("cheddar soup", 10).size() == 1);
    const auto r2 = t.searchFoods("cheddar", 10);
    CHECK(r2.size() == 5);
    CHECK(!r2.empty() && r2[0].name == "My cheddar omelette");  // your foods first
    const auto r3 = t.searchFoods("cheese ched", 10);
    CHECK(r3.size() >= 2 && r3[0].name == "Cheese, cheddar");  // starts with first word, then shortest
}

CALOR_TEST_MAIN
