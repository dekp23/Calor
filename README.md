# Calor

A calorie and nutrient tracker built for exact data entry and deep visualization, with no AI estimation. CSC212 project.

**Name:** Calor is Latin for "heat" and the root of "calorie." (Name is not final.)

## Idea

Most trackers (cal.ai style) estimate calories from photos or text. Calor is for people who want to log precise data, track many nutrient fields, and explore it through charts.

## Stack (planned)

| Layer | Choice |
| --- | --- |
| Engine | C++ |
| Core data structure | Segment tree over day index (nodes hold a nutrient vector) |
| Food search | Trie (optional) |
| Database | SQLite (source of truth) |
| Server | C++ HTTP server (Crow or cpp-httplib) |
| Frontend | Browser UI with Chart.js or D3 |
| Hosting | Raspberry Pi with SSD, reached over Tailscale |

## Documentation

See [docs/PROJECT_PLAN.md](docs/PROJECT_PLAN.md) for architecture, data structure choices, database design, hosting setup, and the benchmark plan.

## Status

Planning. No code yet.
