// Calor HTTP server: JSON API over the Tracker plus the static web UI.
//
//   calor [--db calor.db] [--host 0.0.0.0] [--port 8080] [--web web]
//         [--seed-demo DAYS]
//
// Requests are form- or query-encoded; responses are JSON.
#include <calor/tracker.hpp>

#include "httplib.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <random>
#include <sstream>
#include <stdexcept>
#include <string>

using namespace calor;

namespace {

// ---- Tiny JSON writer ----

std::string jstr(const std::string& s) {
    std::string out = "\"";
    for (unsigned char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof buf, "\\u%04x", c);
                    out += buf;
                } else {
                    out += static_cast<char>(c);
                }
        }
    }
    return out + "\"";
}

std::string jnum(double v) {
    if (!std::isfinite(v)) return "null";
    char buf[32];
    std::snprintf(buf, sizeof buf, "%.10g", v);
    return buf;
}

std::string jnutrients(const Nutrients& n) {
    std::string s = "{";
    for (std::size_t j = 0; j < kNumNutrients; ++j) {
        if (j) s += ",";
        s += jstr(kNutrients[j].key) + ":" + jnum(n[j]);
    }
    return s + "}";
}

std::string jfood(const Food& f) {
    return "{\"id\":" + std::to_string(f.id) + ",\"name\":" + jstr(f.name) + ",\"serving\":" + jstr(f.serving) +
           ",\"fdc_id\":" + (f.fdc_id ? std::to_string(*f.fdc_id) : "null") + ",\"source\":" + jstr(f.source) +
           ",\"nutrients\":" + jnutrients(f.per_serving) + "}";
}

std::string jentry(const Entry& e) {
    return "{\"id\":" + std::to_string(e.id) + ",\"date\":" + jstr(formatDate(e.day)) +
           ",\"food_id\":" + (e.food_id ? std::to_string(*e.food_id) : "null") +
           ",\"servings\":" + jnum(e.servings) + ",\"name\":" + jstr(e.name) + ",\"meal\":" + jstr(e.meal) +
           ",\"notes\":" + jstr(e.notes) + ",\"nutrients\":" + jnutrients(e.nutrients) + "}";
}

std::string jstats(const RangeStats& s) {
    std::string sum = "{", avg = "{", mn = "{", mx = "{";
    for (std::size_t j = 0; j < kNumNutrients; ++j) {
        const std::string k = (j ? "," : "") + jstr(kNutrients[j].key) + ":";
        sum += k + jnum(s.sum[j]);
        avg += k + (s.count ? jnum(s.avg(j)) : "null");
        mn += k + jnum(s.min[j]);
        mx += k + jnum(s.max[j]);
    }
    return "{\"logged_days\":" + std::to_string(s.count) + ",\"sum\":" + sum + "},\"avg\":" + avg +
           "},\"min\":" + mn + "},\"max\":" + mx + "}}";
}

template <class T, class F>
std::string jarray(const std::vector<T>& v, F f) {
    std::string s = "[";
    for (std::size_t i = 0; i < v.size(); ++i) s += (i ? "," : "") + f(v[i]);
    return s + "]";
}

// ---- Request helpers ----

struct BadRequest : std::invalid_argument {
    using std::invalid_argument::invalid_argument;
};

std::string param(const httplib::Request& r, const char* name, const std::string& def = "") {
    return r.has_param(name) ? r.get_param_value(name) : def;
}

std::string required(const httplib::Request& r, const char* name) {
    if (!r.has_param(name) || r.get_param_value(name).empty())
        throw BadRequest(std::string("missing parameter: ") + name);
    return r.get_param_value(name);
}

double toNumber(const std::string& s, const char* name) {
    try {
        std::size_t used = 0;
        const double v = std::stod(s, &used);
        if (used != s.size() || !std::isfinite(v)) throw std::invalid_argument("");
        return v;
    } catch (const std::exception&) {
        throw BadRequest(std::string("not a number: ") + name);
    }
}

std::int64_t toDay(const std::string& s, const char* name) {
    const auto d = parseDate(s);
    if (!d) throw BadRequest(std::string(name) + " must be a date YYYY-MM-DD");
    return *d;
}

std::int64_t toId(const std::string& s) {
    try {
        return std::stoll(s);
    } catch (const std::exception&) {
        throw BadRequest("bad id");
    }
}

// Reads every nutrient key that is present; missing keys default to 0.
Nutrients readNutrients(const httplib::Request& r) {
    Nutrients n(kNumNutrients, 0.0);
    for (std::size_t j = 0; j < kNumNutrients; ++j) {
        const std::string v = param(r, kNutrients[j].key);
        if (!v.empty()) n[j] = toNumber(v, kNutrients[j].key);
    }
    return n;
}

std::size_t nutrientIndex(const std::string& key) {
    for (std::size_t j = 0; j < kNumNutrients; ++j)
        if (key == kNutrients[j].key) return j;
    throw BadRequest("unknown nutrient: " + key);
}

void sendJson(httplib::Response& res, const std::string& body, int status = 200) {
    res.status = status;
    res.set_content(body, "application/json");
}

void notFound(httplib::Response& res) { sendJson(res, "{\"error\":\"not found\"}", 404); }

std::int64_t todayDay() {
    using namespace std::chrono;
    return duration_cast<hours>(system_clock::now().time_since_epoch()).count() / 24;
}

// Synthetic demo data, clearly labeled as such, for presentations.
void seedDemo(Tracker& t, int days) {
    if (t.loggedRange()) {
        std::cout << "Database already has entries; skipping --seed-demo.\n";
        return;
    }
    std::mt19937 rng(212);
    std::normal_distribution<double> noise(0.0, 1.0);
    const char* meals[] = {"breakfast", "lunch", "dinner", "snack"};
    const double share[] = {0.25, 0.32, 0.35, 0.08};
    std::vector<Entry> es;
    const std::int64_t today = todayDay();
    for (int i = days - 1; i >= 0; --i) {
        if (rng() % 10 == 0) continue;  // some days not logged, like real use
        const std::int64_t day = today - i;
        const double target = 2200 + 250 * noise(rng) + (civilFromDays(day).d % 7 == 0 ? 400 : 0);
        for (int m = 0; m < 4; ++m) {
            const double cal = std::max(50.0, target * share[m] * (1 + 0.15 * noise(rng)));
            const double p = cal * (0.20 + 0.03 * noise(rng)) / 4, f = cal * 0.30 / 9;
            const double c = std::max(0.0, (cal - 4 * p - 9 * f) / 4);
            Entry e;
            e.day = day;
            e.name = std::string("Demo ") + meals[m];
            e.meal = meals[m];
            e.notes = "synthetic demo data";
            e.nutrients = {cal, p, c, f, c * 0.12, c * 0.25, cal * 1.4, f * 0.35, 400 + 150 * std::fabs(noise(rng))};
            for (double& v : e.nutrients) v = std::round(std::max(0.0, v) * 10) / 10;
            es.push_back(std::move(e));
        }
    }
    t.addEntriesBulk(es);
    std::cout << "Seeded " << es.size() << " synthetic entries over " << days << " days.\n";
}

}  // namespace

int main(int argc, char** argv) {
    std::string db = "calor.db", host = "0.0.0.0", web = "web";
    int port = 8080, seed = 0;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&]() -> std::string {
            if (i + 1 >= argc) { std::cerr << "missing value for " << a << "\n"; std::exit(2); }
            return argv[++i];
        };
        if (a == "--db") db = next();
        else if (a == "--host") host = next();
        else if (a == "--port") port = std::stoi(next());
        else if (a == "--web") web = next();
        else if (a == "--seed-demo") seed = std::stoi(next());
        else {
            std::cerr << "usage: calor [--db FILE] [--host H] [--port P] [--web DIR] [--seed-demo DAYS]\n";
            return a == "--help" || a == "-h" ? 0 : 2;
        }
    }

    Tracker tracker(db);
    if (seed > 0) seedDemo(tracker, seed);

    httplib::Server svr;

    svr.set_exception_handler([](const httplib::Request&, httplib::Response& res, std::exception_ptr ep) {
        try {
            std::rethrow_exception(ep);
        } catch (const std::invalid_argument& e) {
            sendJson(res, "{\"error\":" + jstr(e.what()) + "}", 400);
        } catch (const std::exception& e) {
            const std::string msg = e.what();
            // Unique-name violation on foods is a user error, not a crash.
            const int status = msg.find("UNIQUE") != std::string::npos ? 409 : 500;
            sendJson(res, "{\"error\":" + jstr(status == 409 ? "a food with that name already exists" : msg) + "}",
                     status);
        }
    });

    // ---- Meta ----
    svr.Get("/api/meta", [&](const httplib::Request&, httplib::Response& res) {
        std::string ns = "[";
        for (std::size_t j = 0; j < kNumNutrients; ++j)
            ns += std::string(j ? "," : "") + "{\"key\":" + jstr(kNutrients[j].key) + ",\"label\":" +
                  jstr(kNutrients[j].label) + ",\"unit\":" + jstr(kNutrients[j].unit) + "}";
        ns += "]";
        const auto range = tracker.loggedRange();
        const auto info = tracker.info();
        sendJson(res, "{\"nutrients\":" + ns + ",\"first_logged\":" +
                          (range ? jstr(formatDate(range->first)) : "null") + ",\"last_logged\":" +
                          (range ? jstr(formatDate(range->second)) : "null") + ",\"tree\":{\"first_day\":" +
                          jstr(formatDate(info.base_day)) + ",\"days\":" + std::to_string(info.days) +
                          ",\"memory_bytes\":" + std::to_string(info.memory_bytes) + "},\"foods\":" +
                          std::to_string(info.foods) + "}");
    });

    // ---- Foods ----
    svr.Get("/api/foods", [&](const httplib::Request& req, httplib::Response& res) {
        const std::string q = param(req, "q");
        const auto limit = static_cast<std::size_t>(std::clamp(toNumber(param(req, "limit", "20"), "limit"), 1.0, 500.0));
        const auto foods = q.empty() ? tracker.listFoods(limit) : tracker.searchFoods(q, limit);
        sendJson(res, jarray(foods, jfood));
    });

    svr.Post("/api/foods", [&](const httplib::Request& req, httplib::Response& res) {
        Food f;
        f.name = required(req, "name");
        f.serving = param(req, "serving");
        f.per_serving = readNutrients(req);
        sendJson(res, jfood(tracker.addFood(f)), 201);
    });

    svr.Post(R"(/api/foods/(\d+))", [&](const httplib::Request& req, httplib::Response& res) {
        Food f;
        f.id = toId(req.matches[1]);
        f.name = required(req, "name");
        f.serving = param(req, "serving");
        f.per_serving = readNutrients(req);
        if (auto out = tracker.updateFood(f)) sendJson(res, jfood(*out));
        else notFound(res);
    });

    svr.Delete(R"(/api/foods/(\d+))", [&](const httplib::Request& req, httplib::Response& res) {
        if (tracker.deleteFood(toId(req.matches[1]))) sendJson(res, "{\"ok\":true}");
        else notFound(res);
    });

    // ---- Entries ----
    svr.Get("/api/entries", [&](const httplib::Request& req, httplib::Response& res) {
        sendJson(res, jarray(tracker.entriesOnDay(toDay(required(req, "date"), "date")), jentry));
    });

    // Either food_id (+ servings) to log a saved food, or name + nutrient
    // values for a one-off entry.
    svr.Post("/api/entries", [&](const httplib::Request& req, httplib::Response& res) {
        const std::int64_t day = toDay(required(req, "date"), "date");
        const std::string meal = param(req, "meal"), notes = param(req, "notes");
        const double servings = toNumber(param(req, "servings", "1"), "servings");
        if (!param(req, "food_id").empty()) {
            if (auto e = tracker.logFood(day, toId(param(req, "food_id")), servings, meal, notes))
                sendJson(res, jentry(*e), 201);
            else
                notFound(res);
            return;
        }
        Entry e;
        e.day = day;
        e.name = required(req, "name");
        e.servings = servings;
        e.meal = meal;
        e.notes = notes;
        e.nutrients = readNutrients(req);
        sendJson(res, jentry(tracker.addEntry(e)), 201);
    });

    svr.Post(R"(/api/entries/(\d+))", [&](const httplib::Request& req, httplib::Response& res) {
        Entry e;
        e.id = toId(req.matches[1]);
        e.day = toDay(required(req, "date"), "date");
        e.name = required(req, "name");
        e.servings = toNumber(param(req, "servings", "1"), "servings");
        e.meal = param(req, "meal");
        e.notes = param(req, "notes");
        e.nutrients = readNutrients(req);
        if (!param(req, "food_id").empty()) e.food_id = toId(param(req, "food_id"));
        if (auto out = tracker.updateEntry(e)) sendJson(res, jentry(*out));
        else notFound(res);
    });

    svr.Delete(R"(/api/entries/(\d+))", [&](const httplib::Request& req, httplib::Response& res) {
        if (tracker.deleteEntry(toId(req.matches[1]))) sendJson(res, "{\"ok\":true}");
        else notFound(res);
    });

    // ---- Analytics (segment tree) ----
    svr.Get("/api/stats", [&](const httplib::Request& req, httplib::Response& res) {
        const auto s = tracker.stats(toDay(required(req, "from"), "from"), toDay(required(req, "to"), "to"));
        std::string best = "{", worst = "{";
        for (std::size_t j = 0; j < kNumNutrients; ++j) {
            const std::string k = (j ? "," : "") + jstr(kNutrients[j].key) + ":";
            best += k + (s.max_day[j] ? jstr(formatDate(*s.max_day[j])) : "null");
            worst += k + (s.min_day[j] ? jstr(formatDate(*s.min_day[j])) : "null");
        }
        sendJson(res, "{\"from\":" + jstr(formatDate(s.from)) + ",\"to\":" + jstr(formatDate(s.to)) +
                          ",\"days\":" + std::to_string(s.to - s.from + 1) + ",\"stats\":" + jstats(s.stats) +
                          ",\"max_day\":" + best + "},\"min_day\":" + worst + "}}");
    });

    svr.Get("/api/series", [&](const httplib::Request& req, httplib::Response& res) {
        const std::string b = param(req, "bucket", "day");
        BucketSize size;
        if (b == "day") size = BucketSize::Day;
        else if (b == "week") size = BucketSize::Week;
        else if (b == "month") size = BucketSize::Month;
        else throw BadRequest("bucket must be day, week or month");
        const auto buckets =
            tracker.series(toDay(required(req, "from"), "from"), toDay(required(req, "to"), "to"), size);
        sendJson(res, jarray(buckets, [](const Bucket& k) {
                     return "{\"start\":" + jstr(formatDate(k.start)) + ",\"end\":" + jstr(formatDate(k.end)) +
                            ",\"stats\":" + jstats(k.stats) + "}";
                 }));
    });

    svr.Get("/api/rolling", [&](const httplib::Request& req, httplib::Response& res) {
        const std::int64_t from = toDay(required(req, "from"), "from");
        const auto window = static_cast<std::size_t>(toNumber(param(req, "window", "7"), "window"));
        const auto vals = tracker.rolling(from, toDay(required(req, "to"), "to"), window,
                                          nutrientIndex(param(req, "nutrient", "calories")));
        std::string s = "[";
        for (std::size_t i = 0; i < vals.size(); ++i)
            s += std::string(i ? "," : "") + "{\"date\":" + jstr(formatDate(from + static_cast<std::int64_t>(i))) +
                 ",\"avg\":" + (vals[i] ? jnum(*vals[i]) : "null") + "}";
        sendJson(res, s + "]");
    });

    if (!svr.set_mount_point("/", web))
        std::cerr << "warning: web directory '" << web << "' not found; only the API is served\n";

    std::cout << "Calor listening on http://" << host << ":" << port << "  (db: " << db << ")\n";
    if (!svr.listen(host, port)) {
        std::cerr << "could not listen on " << host << ":" << port << "\n";
        return 1;
    }
}
