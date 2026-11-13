// calor-import-usda: load USDA FoodData Central foods into a Calor database.
//
//   calor-import-usda --db calor.db [--types sr_legacy_food,foundation_food]
//                     [--dry-run] DIR
//
// DIR is an unzipped FoodData Central CSV download (the folder holding
// food.csv and food_nutrient.csv). Safe to run again: foods are matched by
// fdc_id and updated in place. Restart the server afterwards so its search
// index picks up the new foods.
#include <calor/tracker.hpp>
#include <calor/usda_import.hpp>

#include <chrono>
#include <iostream>
#include <sstream>

int main(int argc, char** argv) {
    std::string db = "calor.db", dir;
    calor::UsdaOptions opts;
    bool dryRun = false;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--db" && i + 1 < argc) db = argv[++i];
        else if (a == "--types" && i + 1 < argc) {
            opts.data_types.clear();
            std::stringstream ss(argv[++i]);
            for (std::string t; std::getline(ss, t, ',');)
                if (!t.empty()) opts.data_types.push_back(t);
        } else if (a == "--dry-run") dryRun = true;
        else if (!a.empty() && a[0] != '-' && dir.empty()) dir = a;
        else {
            std::cerr << "usage: calor-import-usda [--db FILE] [--types t1,t2] [--dry-run] DIR\n"
                         "  DIR: unzipped FoodData Central CSV download\n"
                         "  default types: sr_legacy_food,foundation_food,survey_fndds_food\n"
                         "  add branded_food to --types for branded products (very large)\n";
            return a == "--help" || a == "-h" ? 0 : 2;
        }
    }
    if (dir.empty()) {
        std::cerr << "missing DIR (run with --help)\n";
        return 2;
    }

    try {
        const auto t0 = std::chrono::steady_clock::now();
        const calor::UsdaResult r = calor::readUsda(dir, opts);
        const double readSec = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();

        std::cout << "Read " << r.nutrient_rows << " nutrient rows in " << readSec << " s.\n";
        std::cout << "Food types in food.csv:\n";
        for (const auto& [type, n] : r.seen_types) {
            const auto kept = r.by_type.count(type) ? r.by_type.at(type) : 0;
            std::cout << "  " << type << ": " << n << (kept ? "  (importing " + std::to_string(kept) + ")" : "")
                      << "\n";
        }
        if (r.skipped_no_energy) std::cout << "Skipped " << r.skipped_no_energy << " foods with no calorie value.\n";
        if (r.skipped_bad_value) std::cout << "Skipped " << r.skipped_bad_value << " foods with an invalid amount.\n";
        std::cout << (r.checked_units ? "Checked nutrient units against nutrient.csv.\n"
                                      : "nutrient.csv not found; units were not checked.\n");
        if (r.foods.empty()) {
            std::cout << "Nothing to import. Check --types against the types listed above.\n";
            return 1;
        }
        if (dryRun) {
            std::cout << "Dry run: would import " << r.foods.size() << " foods into " << db << ".\n";
            return 0;
        }

        calor::Tracker tracker(db);
        const calor::ImportCounts c = tracker.importFoods(r.foods);
        std::cout << "Imported into " << db << ": " << c.inserted << " new, " << c.updated << " updated";
        if (c.name_conflicts)
            std::cout << ", " << c.name_conflicts << " skipped because a food with the same name already exists";
        std::cout << ".\nRestart the Calor server to search the new foods.\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "error: " << e.what() << "\n";
        return 1;
    }
}
