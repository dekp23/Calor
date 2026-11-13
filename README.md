# Calor

A free, open-source calorie and nutrient tracker built for exact data entry and deep visualization, with no AI estimation. CSC212 project.

**Name:** Calor is Latin for "heat" and the root of "calorie." (Name is not final.)

Most trackers (cal.ai style) estimate calories from a photo, which cannot show portion weight, oil or hidden ingredients. In Calor you enter the values, and every chart is computed from what you logged.

## How it works

| Layer | What it is |
| --- | --- |
| Core data structure | Segment tree over day index ([`include/calor/segment_tree.hpp`](include/calor/segment_tree.hpp)). Each node stores the sum, min and max of 9 nutrients plus a count of logged days. Any date range: O(k log n). |
| Food search | Trie over the words in food names ([`include/calor/food_trie.hpp`](include/calor/food_trie.hpp)). Every query word must start some word of the name, in any order, so "cheddar sharp" finds "Cheese, cheddar, sharp". Your own foods rank first. |
| Food data | Optional import of USDA FoodData Central (see below). |
| Storage | SQLite, the source of truth. The tree is rebuilt from it at startup and updated on every edit. |
| Server | C++17 with [cpp-httplib](https://github.com/yhirose/cpp-httplib) (vendored), JSON API, serves the web UI. |
| Frontend | Plain HTML/JS with Chart.js (vendored, works offline). |
| Hosting | Raspberry Pi with SSD, reached over Tailscale. |

Unlogged days are not treated as 0-calorie days: they hold identity values in the tree, so averages and the lowest day only count days with entries.

Benchmark results are in [docs/BENCHMARK.md](docs/BENCHMARK.md). In short: the tree beats a linear scan for ranges longer than about 180 days (128× at 100 years) and stays near 1 µs per query as history grows; a scan is faster for short windows like the last 30 days.

## Build and run

Needs a C++17 compiler, CMake 3.16+ and the SQLite development headers.

```bash
# Debian / Raspberry Pi OS / Ubuntu
sudo apt install build-essential cmake libsqlite3-dev

cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release
cmake --build build-release -j
ctest --test-dir build-release --output-on-failure

./build-release/calor --db calor.db --port 8080
# open http://localhost:8080
```

For a demo with data already in it, add `--seed-demo 120` to fill an empty database with 120 days of synthetic entries (named "Demo breakfast" and so on, so they are easy to tell apart from real data). Use a separate database file for this.

Options: `--db FILE` (default `calor.db`), `--host` (default `0.0.0.0`), `--port` (default `8080`), `--web DIR` (default `web`), `--seed-demo DAYS`.

### On the Raspberry Pi

1. Clone the repo onto the SSD and build as above (a Pi 5 builds in about a minute).
2. Edit the user and paths in [`deploy/calor.service`](deploy/calor.service), then install it so Calor starts on boot:
   ```bash
   sudo cp deploy/calor.service /etc/systemd/system/
   sudo systemctl daemon-reload && sudo systemctl enable --now calor
   ```
3. From any device on your tailnet, open `http://<pi-name>:8080`.
4. Back up the database with `sqlite3 calor.db ".backup calor-backup.db"` and copy the backup to another machine.

## Importing USDA foods

Calor can load the USDA [FoodData Central](https://fdc.nal.usda.gov/) database, so you can search thousands of foods instead of typing values in.

1. Download a CSV zip from [fdc.nal.usda.gov/download-datasets](https://fdc.nal.usda.gov/download-datasets). SR Legacy (6.7 MB, about 7,800 common foods) and Foundation Foods (4.1 MB) are good starting points.
2. Unzip it, then import the folder that holds `food.csv`:
   ```bash
   unzip FoodData_Central_sr_legacy_food_csv_2018-04.zip -d usda-sr
   ./build-release/calor-import-usda --db calor.db usda-sr/FoodData_Central_sr_legacy_food_csv_2018-04
   ```
   The zip may unpack into a subfolder; point the command at the folder that has `food.csv` in it.
3. Restart the server (`sudo systemctl restart calor` on the Pi) so search picks up the new foods.

Details:

- **Serving size:** FoodData Central values are per 100 g, so imported foods have a 100 g serving. Enter 0.5 servings for 50 g, 2.4 for 240 g.
- **Which foods:** by default `sr_legacy_food`, `foundation_food` and `survey_fndds_food`. Sample and acquisition records are skipped. Branded products are opt-in because there are about 450,000 of them: `--types branded_food`. The tool prints every data type it finds, with counts.
- **Nutrient mapping** (FoodData Central nutrient ids): calories from Energy 1008, falling back to Atwater Specific 2048 then General 2047 (Foundation Foods no longer report 1008); protein 1003; carbs 1005, else 1050; fat 1004; fiber 1079; sugar 2000, else 1063; sodium 1093; saturated fat 1258; water 1051 (grams stored as ml). A nutrient that is not reported is stored as 0. Foods with no energy value are skipped.
- **Safety checks:** if `nutrient.csv` is present, the tool checks each mapped id has the expected unit and stops if one does not. Imports never overwrite a food you created with the same name.
- **Re-running** is safe. Foods are matched by `fdc_id` and updated in place. Edits you make to an imported food are overwritten by the next import.
- `--dry-run` reports what would be imported without writing anything.

Measured on a 2-core cloud VM with 15,000 synthetic foods and 1.2 million nutrient rows: the import took 0.6 s, the server used 6.5 MB more memory, and searches answered in 1 to 3 ms.

## API

All responses are JSON. Write requests take form-encoded fields. Dates are `YYYY-MM-DD`. Nutrient fields: `calories`, `protein_g`, `carbs_g`, `fat_g`, `fiber_g`, `sugar_g`, `sodium_mg`, `sat_fat_g`, `water_ml`.

| Method and path | Does |
| --- | --- |
| `GET /api/meta` | Nutrient list, logged date range, tree size and memory |
| `GET /api/foods?q=cheddar%20sharp&limit=10` | Multi-word trie search, ranked (your foods first if `q` is empty). Each food includes `source` and `fdc_id` |
| `POST /api/foods` | Save a food: `name`, `serving`, nutrient values per serving |
| `POST /api/foods/{id}`, `DELETE /api/foods/{id}` | Edit or delete a food (past entries keep their values) |
| `GET /api/entries?date=` | Entries on one day |
| `POST /api/entries` | Log `food_id` + `servings`, or a one-off `name` + nutrient totals; plus `date`, `meal`, `notes` |
| `POST /api/entries/{id}`, `DELETE /api/entries/{id}` | Edit or delete an entry |
| `GET /api/stats?from=&to=` | Sum, average, min, max and the highest and lowest day for each nutrient |
| `GET /api/series?from=&to=&bucket=day\|week\|month` | One stats object per bucket |
| `GET /api/rolling?from=&to=&window=7&nutrient=calories` | Rolling average over logged days |

Errors return `{"error": "..."}` with status 400 (bad input), 404 (no such id) or 409 (duplicate food name).

## Repository layout

```
include/calor/   segment_tree.hpp, food_trie.hpp, nutrients.hpp (fields and dates),
                 storage.hpp, tracker.hpp
src/             storage.cpp (SQLite), tracker.cpp (engine), main.cpp (HTTP server),
                 usda_import.cpp (CSV reader, FoodData Central mapping), import_usda.cpp (CLI)
web/             index.html, app.js, style.css, vendor/ (Chart.js, Archivo font)
tests/           test_core.cpp (tree vs naive scan, trie, dates), test_tracker.cpp,
                 test_usda.cpp, fixtures/usda (includes a real SR Legacy record)
bench/           benchmark.cpp, plot.py, results
deploy/          calor.service (systemd)
docs/            PROJECT_PLAN.md, BENCHMARK.md
```

To track another nutrient, append it to `kNutrients` in [`include/calor/nutrients.hpp`](include/calor/nutrients.hpp) and rebuild. Existing databases get the new column on next start.

## License

MIT, see [LICENSE](LICENSE). Vendored: cpp-httplib (MIT), Chart.js (MIT), Archivo font (SIL OFL 1.1).
