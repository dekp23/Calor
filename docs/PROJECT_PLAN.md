# Calor: Project Plan (CSC212)

## Overview

Calor is a calorie and nutrient tracker built for exact data entry and deep visualization, with no AI estimation. It is a CSC212 project: a C++ engine, a web frontend, and one data structure that was not covered in class.

- **Name:** Calor, Latin for "heat" and the root of "calorie." In Spanish, Portuguese and Catalan it also means weather heat. Other brands use the name (Calor Gas in the UK, a French appliance maker), so check app stores and domains before publishing anything.
- **Differentiator:** users log precise, detailed data and explore it through charts, unlike photo-based or AI-estimate trackers (cal.ai style).
- **Course constraints:** C++ must be used for the data structure; other languages are allowed elsewhere. The instructor has not covered the trie, Fenwick tree or skip list, so all three are allowed.

## Architecture

```
Browser UI (Chart.js or D3, any tailnet device)
        |  Tailscale
        v
+------------------ Raspberry Pi with SSD ------------------+
|  C++ HTTP server (Crow or cpp-httplib; serves UI + JSON)   |
|        |  range queries            |  food search          |
|        v                           v                       |
|  Segment tree (in-memory)     Trie (optional)              |
|        ^  built from SQLite at startup   ^  from foods     |
|        |                           |                       |
|  SQLite file on the SSD (source of truth)                  |
+------------------------------------------------------------+
```

Requests arrive over Tailscale. The C++ server answers range queries from the tree and food searches from the trie. Each new or edited entry is written to SQLite first, then applied to the tree. Both structures are rebuilt from SQLite at startup.

## Data structure

The core is a segment tree over day index, where each node stores a vector of nutrient totals. One structure answers sum, min, max and average over any date range in O(log n), which drives most charts.

| Structure | Used for | Query cost | Notes |
| --- | --- | --- | --- |
| Segment tree (primary) | Sum, min, max over date ranges; rolling averages; best and worst days | O(log n) | Most flexible; nodes hold a nutrient vector |
| Fenwick tree | Range sums only | O(log n) | Very compact; simpler fallback if min/max is not needed |
| Trie | Food-name prefix search | O(prefix length) | Not covered in class, so allowed |
| Skip list | Sorted entry log | O(log n) expected | Niche alternative to a balanced BST |
| Interval tree | Meals or fasting windows with durations | O(log n + k) | Only if time spans are tracked |
| Sparse table | Range min/max on unchanging data | O(1) | Poor fit: past entries get edited |
| Bloom filter | "Have I logged this food before?" | O(1) | Probabilistic; false positives need handling |

The tree is justified only if it beats a SQL range query or a naive scan at some scale, so the benchmark plan below is part of the design.

## Database

Use SQLite as the source of truth, with the segment tree as an in-memory index built from it. PostgreSQL would work but adds a server process, memory use and setup with no performance benefit for two users.

| Option | Fit | Trade-off |
| --- | --- | --- |
| SQLite (recommended) | Single file, C API links into the C++ binary, trivial backups | One writer at a time, irrelevant for two users |
| PostgreSQL | Real server database, many concurrent writers, advanced features | Running service on the Pi, libpqxx client, user and permission setup |
| DuckDB | Very fast aggregations | Less common for transactional logging; newer C++ API |
| CSV / JSON files | Fine for a prototype | Hand-writing what SQLite gives for free |

How the layers work together:

1. On startup, load daily totals from SQLite and build the tree in O(n).
2. On each new or edited entry, write to SQLite, then update the tree in O(log n).
3. Chart queries (ranges, rolling averages, extremes) hit the tree, not SQL.

Schema sketch:

- `foods`: id, name, per-serving nutrients
- `entries`: id, date/time, food_id, quantity, notes
- `nutrients` (optional): lets new tracked fields be added without altering the schema

Turn on WAL mode (`PRAGMA journal_mode=WAL`). The USDA FoodData Central dataset is free, downloads as CSV, and can populate `foods` (and the trie) with real nutrition data.

## Hosting on the Raspberry Pi

Hardware: two Pi 5s and one Pi 4, with an SSD. Tailscale gives private, encrypted access from any of your devices with no port forwarding. Both of you reach the app at the Pi's Tailscale name (for example `http://calor:8080` via MagicDNS).

Setup checklist:

- [ ] Run `uname -m`; `aarch64` means a 64-bit OS
- [ ] Mount the SSD by UUID in `/etc/fstab`, not `/dev/sda1`
- [ ] Keep the app and SQLite file on the SSD
- [ ] Run the server as a systemd service so it starts on boot
- [ ] Share only the Pi's node with your partner (Tailscale node sharing), or invite them to the tailnet
- [ ] Optional: `tailscale serve` for HTTPS with a real certificate
- [ ] Back up the database file to another machine (cron plus rsync over Tailscale)

Demo day: classmates and the instructor are not on your tailnet. Either enable `tailscale funnel` for one port during the demo (turn it off afterward) or demo live from a laptop on the tailnet. Keep a fallback, such as a recorded demo, in case campus Wi-Fi blocks devices.

## Features and visualizations

Every visualization below is a range query the segment tree can answer directly.

- Daily, weekly and monthly calorie and nutrient trends
- Macro breakdown (protein, carbs, fat) for any date range
- Rolling averages, such as a 7-day average
- Highest and lowest days in a chosen window
- Food search by prefix (trie)
- Extra tracked fields beyond calories, supported by the flexible `nutrients` table

Frontend: a browser UI using Chart.js or D3, served by the C++ HTTP server. Alternatives if needed: a C++ engine exposed to Python via pybind11 with Streamlit or matplotlib, or a CLI that exports JSON for a separate page to render.

## Benchmark plan

SQL can already run `SELECT SUM(...) WHERE date BETWEEN ...` quickly with an index, so expect the question "why a tree?" At small sizes SQL may win. The report should say so and show where the tree pulls ahead.

1. Generate synthetic data at several sizes (for example 1k, 10k, 100k and 1M days or entries).
2. Time range queries three ways: naive linear scan, indexed SQL, and the segment tree.
3. Time point updates, since the tree must stay in sync with edits.
4. Plot query time against data size and report the crossover point honestly.

## Open questions

- [x] Covered in class? None of the trie, Fenwick tree or skip list, so all three are allowed.
- [x] Interface requirement? None; any interface works.
- [x] Pi hardware? Two Pi 5s and one Pi 4.
- [ ] Name: Calor is not final, and no domain or app-store listing is needed yet.

## Stretch goal: App Store

The web UI cannot be submitted as-is. Two routes: wrap it in a native shell (for example Capacitor), or write a native SwiftUI client against the same HTTP API. Keeping the C++ engine behind an HTTP API supports both. Because the data lives on the Pi, the phone still needs Tailscale to reach it, and Apple may reject apps that are only a thin web wrapper. An Apple Developer Program membership is also required (about $99 per year; verify current pricing).

## Next step

Scaffold the repository with the C++ segment tree, a SQLite layer, a minimal HTTP endpoint, a chart page and a systemd unit file.
