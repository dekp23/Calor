# Calor

A free, open-source calorie and nutrient tracker built for exact data entry and deep visualization, with no AI estimation. CSC212 project.

**Name:** Calor is Latin for "heat" and the root of "calorie." (Name is not final.)

Most trackers (cal.ai style) estimate calories from a photo, which cannot show portion weight, oil or hidden ingredients. In Calor you enter the values, and every chart is computed from what you logged.

## How it works

| Layer | What it is |
| --- | --- |
| Core data structure | Segment tree over day index ([`include/calor/segment_tree.hpp`](include/calor/segment_tree.hpp)). Each node stores the sum, min and max of 9 nutrients plus a count of logged days. Any date range: O(k log n). |
| Food search | Trie over every word in a food name ([`include/calor/food_trie.hpp`](include/calor/food_trie.hpp)), so "bre" finds "Chicken breast". |
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

## API

All responses are JSON. Write requests take form-encoded fields. Dates are `YYYY-MM-DD`. Nutrient fields: `calories`, `protein_g`, `carbs_g`, `fat_g`, `fiber_g`, `sugar_g`, `sodium_mg`, `sat_fat_g`, `water_ml`.

| Method and path | Does |
| --- | --- |
| `GET /api/meta` | Nutrient list, logged date range, tree size and memory |
| `GET /api/foods?q=chi&limit=10` | Trie prefix search (all foods if `q` is empty) |
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
src/             storage.cpp (SQLite), tracker.cpp (engine), main.cpp (HTTP server)
web/             index.html, app.js, style.css, vendor/ (Chart.js, Archivo font)
tests/           test_core.cpp (tree vs naive scan, trie, dates), test_tracker.cpp
bench/           benchmark.cpp, plot.py, results
deploy/          calor.service (systemd)
docs/            PROJECT_PLAN.md, BENCHMARK.md
```

To track another nutrient, append it to `kNutrients` in [`include/calor/nutrients.hpp`](include/calor/nutrients.hpp) and rebuild. Existing databases get the new column on next start.

## License

MIT, see [LICENSE](LICENSE). Vendored: cpp-httplib (MIT), Chart.js (MIT), Archivo font (SIL OFL 1.1).
