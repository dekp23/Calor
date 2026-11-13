#include <calor/storage.hpp>

#include <sqlite3.h>

#include <set>
#include <stdexcept>

namespace calor {
namespace {

// Small RAII wrapper over a prepared statement.
class Stmt {
public:
    Stmt(sqlite3* db, const std::string& sql) : db_(db) {
        if (sqlite3_prepare_v2(db, sql.c_str(), -1, &s_, nullptr) != SQLITE_OK)
            throw std::runtime_error(std::string("SQLite prepare: ") + sqlite3_errmsg(db) + " in: " + sql);
    }
    ~Stmt() { sqlite3_finalize(s_); }
    Stmt(const Stmt&) = delete;
    Stmt& operator=(const Stmt&) = delete;

    Stmt& bind(int i, std::int64_t v) { check(sqlite3_bind_int64(s_, i, v)); return *this; }
    Stmt& bind(int i, double v) { check(sqlite3_bind_double(s_, i, v)); return *this; }
    Stmt& bind(int i, const std::string& v) {
        check(sqlite3_bind_text(s_, i, v.c_str(), static_cast<int>(v.size()), SQLITE_TRANSIENT));
        return *this;
    }
    Stmt& bindNull(int i) { check(sqlite3_bind_null(s_, i)); return *this; }

    // Returns true while there is a row to read.
    bool step() {
        const int rc = sqlite3_step(s_);
        if (rc == SQLITE_ROW) return true;
        if (rc == SQLITE_DONE) return false;
        throw std::runtime_error(std::string("SQLite step: ") + sqlite3_errmsg(db_));
    }
    void run() { while (step()) {} }
    void reset() {
        sqlite3_reset(s_);
        sqlite3_clear_bindings(s_);
    }

    std::int64_t i64(int c) const { return sqlite3_column_int64(s_, c); }
    double dbl(int c) const { return sqlite3_column_double(s_, c); }
    bool isNull(int c) const { return sqlite3_column_type(s_, c) == SQLITE_NULL; }
    std::string text(int c) const {
        const auto* p = sqlite3_column_text(s_, c);
        return p ? std::string(reinterpret_cast<const char*>(p)) : std::string();
    }

private:
    sqlite3* db_;
    sqlite3_stmt* s_ = nullptr;
    void check(int rc) {
        if (rc != SQLITE_OK) throw std::runtime_error(std::string("SQLite bind: ") + sqlite3_errmsg(db_));
    }
};

// "calories, protein_g, ..." with an optional prefix/suffix per column.
std::string nutrientCols(const std::string& pre = "", const std::string& post = "") {
    std::string s;
    for (std::size_t j = 0; j < kNumNutrients; ++j) {
        if (j) s += ", ";
        s += pre + kNutrients[j].key + post;
    }
    return s;
}

std::string placeholders(std::size_t n) {
    std::string s;
    for (std::size_t i = 0; i < n; ++i) s += i ? ", ?" : "?";
    return s;
}

void checkDims(const Nutrients& n) {
    if (n.size() != kNumNutrients) throw std::invalid_argument("nutrient vector has wrong length");
}

const std::string kFoodCols = "id, name, serving, fdc_id, source, " + nutrientCols();
const std::string kEntryCols = "id, day, food_id, servings, name, meal, notes, " + nutrientCols();

Food readFood(const Stmt& st) {
    Food f;
    f.id = st.i64(0);
    f.name = st.text(1);
    f.serving = st.text(2);
    if (!st.isNull(3)) f.fdc_id = st.i64(3);
    f.source = st.text(4);
    for (std::size_t j = 0; j < kNumNutrients; ++j) f.per_serving[j] = st.dbl(5 + static_cast<int>(j));
    return f;
}

Entry readEntry(const Stmt& st) {
    Entry e;
    e.id = st.i64(0);
    e.day = st.i64(1);
    if (!st.isNull(2)) e.food_id = st.i64(2);
    e.servings = st.dbl(3);
    e.name = st.text(4);
    e.meal = st.text(5);
    e.notes = st.text(6);
    for (std::size_t j = 0; j < kNumNutrients; ++j) e.nutrients[j] = st.dbl(7 + static_cast<int>(j));
    return e;
}

// Binds day..notes + nutrients starting at parameter 1; returns next index.
int bindEntry(Stmt& st, const Entry& e) {
    checkDims(e.nutrients);
    int i = 1;
    st.bind(i++, e.day);
    if (e.food_id) st.bind(i++, *e.food_id); else st.bindNull(i++);
    st.bind(i++, e.servings);
    st.bind(i++, e.name);
    st.bind(i++, e.meal);
    st.bind(i++, e.notes);
    for (double v : e.nutrients) st.bind(i++, v);
    return i;
}

}  // namespace

Storage::Storage(const std::string& path) {
    if (sqlite3_open(path.c_str(), &db_) != SQLITE_OK) {
        const std::string msg = db_ ? sqlite3_errmsg(db_) : "out of memory";
        sqlite3_close(db_);
        throw std::runtime_error("cannot open database " + path + ": " + msg);
    }
    sqlite3_busy_timeout(db_, 5000);
    exec("PRAGMA journal_mode=WAL");
    exec("PRAGMA foreign_keys=ON");
    migrate();
}

Storage::~Storage() { sqlite3_close(db_); }

void Storage::exec(const std::string& sql) {
    char* err = nullptr;
    if (sqlite3_exec(db_, sql.c_str(), nullptr, nullptr, &err) != SQLITE_OK) {
        const std::string msg = err ? err : "unknown error";
        sqlite3_free(err);
        throw std::runtime_error("SQLite: " + msg + " in: " + sql);
    }
}

void Storage::migrate() {
    exec("CREATE TABLE IF NOT EXISTS foods ("
         " id INTEGER PRIMARY KEY,"
         " name TEXT NOT NULL UNIQUE COLLATE NOCASE,"
         " serving TEXT NOT NULL DEFAULT '')");
    exec("CREATE TABLE IF NOT EXISTS entries ("
         " id INTEGER PRIMARY KEY,"
         " day INTEGER NOT NULL,"
         " food_id INTEGER REFERENCES foods(id) ON DELETE SET NULL,"
         " servings REAL NOT NULL DEFAULT 1,"
         " name TEXT NOT NULL,"
         " meal TEXT NOT NULL DEFAULT '',"
         " notes TEXT NOT NULL DEFAULT '',"
         " created_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP)");
    exec("CREATE INDEX IF NOT EXISTS idx_entries_day ON entries(day)");

    // Columns added after the first release; ALTER on older databases.
    auto addColumn = [&](const char* table, const char* col, const char* type) {
        Stmt info(db_, std::string("PRAGMA table_info(") + table + ")");
        while (info.step())
            if (info.text(1) == col) return;
        exec(std::string("ALTER TABLE ") + table + " ADD COLUMN " + col + " " + type);
    };
    addColumn("foods", "fdc_id", "INTEGER");
    addColumn("foods", "source", "TEXT NOT NULL DEFAULT ''");
    exec("CREATE UNIQUE INDEX IF NOT EXISTS idx_foods_fdc ON foods(fdc_id) WHERE fdc_id IS NOT NULL");

    // Add any nutrient column that is missing, so appending to kNutrients
    // upgrades existing databases in place.
    for (const char* table : {"foods", "entries"}) {
        std::set<std::string> have;
        Stmt info(db_, std::string("PRAGMA table_info(") + table + ")");
        while (info.step()) have.insert(info.text(1));
        for (const auto& n : kNutrients)
            if (!have.count(n.key))
                exec(std::string("ALTER TABLE ") + table + " ADD COLUMN " + n.key + " REAL NOT NULL DEFAULT 0");
    }
}

// ---- Foods ----

std::int64_t Storage::addFood(const Food& f) {
    checkDims(f.per_serving);
    Stmt st(db_, "INSERT INTO foods (name, serving, fdc_id, source, " + nutrientCols() + ") VALUES (?, ?, ?, ?, " +
                     placeholders(kNumNutrients) + ")");
    int i = 1;
    st.bind(i++, f.name);
    st.bind(i++, f.serving);
    if (f.fdc_id) st.bind(i++, *f.fdc_id); else st.bindNull(i++);
    st.bind(i++, f.source);
    for (double v : f.per_serving) st.bind(i++, v);
    st.run();
    return sqlite3_last_insert_rowid(db_);
}

ImportCounts Storage::upsertFoods(const std::vector<Food>& foods) {
    ImportCounts c;
    exec("BEGIN");
    try {
        Stmt upd(db_, "UPDATE foods SET name = ?, serving = ?, source = ?, " + nutrientCols("", " = ?") +
                          " WHERE fdc_id = ?");
        Stmt ins(db_, "INSERT OR IGNORE INTO foods (name, serving, fdc_id, source, " + nutrientCols() +
                          ") VALUES (?, ?, ?, ?, " + placeholders(kNumNutrients) + ")");
        for (const Food& f : foods) {
            checkDims(f.per_serving);
            if (!f.fdc_id) throw std::invalid_argument("upsertFoods needs fdc_id");
            upd.reset();
            int i = 1;
            upd.bind(i++, f.name);
            upd.bind(i++, f.serving);
            upd.bind(i++, f.source);
            for (double v : f.per_serving) upd.bind(i++, v);
            upd.bind(i, *f.fdc_id);
            try {
                upd.run();
            } catch (const std::runtime_error&) {
                // Renamed upstream to a name another food already has: keep ours.
                ++c.name_conflicts;
                continue;
            }
            if (sqlite3_changes(db_) > 0) { ++c.updated; continue; }
            ins.reset();
            i = 1;
            ins.bind(i++, f.name);
            ins.bind(i++, f.serving);
            ins.bind(i++, *f.fdc_id);
            ins.bind(i++, f.source);
            for (double v : f.per_serving) ins.bind(i++, v);
            ins.run();
            if (sqlite3_changes(db_) > 0) ++c.inserted;
            else ++c.name_conflicts;
        }
        exec("COMMIT");
    } catch (...) {
        exec("ROLLBACK");
        throw;
    }
    return c;
}

bool Storage::updateFood(const Food& f) {
    checkDims(f.per_serving);
    Stmt st(db_, "UPDATE foods SET name = ?, serving = ?, " + nutrientCols("", " = ?") + " WHERE id = ?");
    int i = 1;
    st.bind(i++, f.name);
    st.bind(i++, f.serving);
    for (double v : f.per_serving) st.bind(i++, v);
    st.bind(i, f.id).run();
    return sqlite3_changes(db_) > 0;
}

bool Storage::deleteFood(std::int64_t id) {
    Stmt(db_, "DELETE FROM foods WHERE id = ?").bind(1, id).run();
    return sqlite3_changes(db_) > 0;
}

std::optional<Food> Storage::getFood(std::int64_t id) const {
    Stmt st(db_, "SELECT " + kFoodCols + " FROM foods WHERE id = ?");
    st.bind(1, id);
    if (!st.step()) return std::nullopt;
    return readFood(st);
}

std::vector<Food> Storage::listFoods(std::size_t limit) const {
    Stmt st(db_, "SELECT " + kFoodCols + " FROM foods ORDER BY fdc_id IS NOT NULL, name LIMIT ?");
    st.bind(1, static_cast<std::int64_t>(limit));
    std::vector<Food> out;
    while (st.step()) out.push_back(readFood(st));
    return out;
}

std::vector<Food> Storage::allFoods() const {
    Stmt st(db_, "SELECT " + kFoodCols + " FROM foods ORDER BY name");
    std::vector<Food> out;
    while (st.step()) out.push_back(readFood(st));
    return out;
}

// ---- Entries ----

std::int64_t Storage::addEntry(const Entry& e) {
    Stmt st(db_, "INSERT INTO entries (day, food_id, servings, name, meal, notes, " + nutrientCols() +
                     ") VALUES (?, ?, ?, ?, ?, ?, " + placeholders(kNumNutrients) + ")");
    bindEntry(st, e);
    st.run();
    return sqlite3_last_insert_rowid(db_);
}

void Storage::addEntries(const std::vector<Entry>& es) {
    exec("BEGIN");
    try {
        Stmt st(db_, "INSERT INTO entries (day, food_id, servings, name, meal, notes, " + nutrientCols() +
                         ") VALUES (?, ?, ?, ?, ?, ?, " + placeholders(kNumNutrients) + ")");
        for (const Entry& e : es) {
            st.reset();
            bindEntry(st, e);
            st.run();
        }
        exec("COMMIT");
    } catch (...) {
        exec("ROLLBACK");
        throw;
    }
}

bool Storage::updateEntry(const Entry& e) {
    Stmt st(db_, "UPDATE entries SET day = ?, food_id = ?, servings = ?, name = ?, meal = ?, notes = ?, " +
                     nutrientCols("", " = ?") + " WHERE id = ?");
    const int next = bindEntry(st, e);
    st.bind(next, e.id).run();
    return sqlite3_changes(db_) > 0;
}

bool Storage::deleteEntry(std::int64_t id) {
    Stmt(db_, "DELETE FROM entries WHERE id = ?").bind(1, id).run();
    return sqlite3_changes(db_) > 0;
}

std::optional<Entry> Storage::getEntry(std::int64_t id) const {
    Stmt st(db_, "SELECT " + kEntryCols + " FROM entries WHERE id = ?");
    st.bind(1, id);
    if (!st.step()) return std::nullopt;
    return readEntry(st);
}

std::vector<Entry> Storage::entriesOnDay(std::int64_t day) const {
    Stmt st(db_, "SELECT " + kEntryCols + " FROM entries WHERE day = ? ORDER BY id");
    st.bind(1, day);
    std::vector<Entry> out;
    while (st.step()) out.push_back(readEntry(st));
    return out;
}

// ---- Daily totals ----

std::optional<Nutrients> Storage::dayTotal(std::int64_t day) const {
    Stmt st(db_, "SELECT COUNT(*), " + nutrientCols("TOTAL(", ")") + " FROM entries WHERE day = ?");
    st.bind(1, day);
    st.step();
    if (st.i64(0) == 0) return std::nullopt;
    Nutrients n(kNumNutrients);
    for (std::size_t j = 0; j < kNumNutrients; ++j) n[j] = st.dbl(1 + static_cast<int>(j));
    return n;
}

std::vector<std::pair<std::int64_t, Nutrients>> Storage::allDailyTotals() const {
    Stmt st(db_, "SELECT day, " + nutrientCols("TOTAL(", ")") + " FROM entries GROUP BY day ORDER BY day");
    std::vector<std::pair<std::int64_t, Nutrients>> out;
    while (st.step()) {
        Nutrients n(kNumNutrients);
        for (std::size_t j = 0; j < kNumNutrients; ++j) n[j] = st.dbl(1 + static_cast<int>(j));
        out.emplace_back(st.i64(0), std::move(n));
    }
    return out;
}

std::optional<std::pair<std::int64_t, std::int64_t>> Storage::dayRange() const {
    Stmt st(db_, "SELECT MIN(day), MAX(day) FROM entries");
    st.step();
    if (st.isNull(0)) return std::nullopt;
    return std::make_pair(st.i64(0), st.i64(1));
}

}  // namespace calor
