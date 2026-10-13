// Database maintenance implementation. See db_maintenance.hpp for the why.
#include "db_maintenance.hpp"
#include "opencode_data.hpp"   // session_table() / message_table() (v1 vs v2)

#include <sqlite3.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <map>
#include <mutex>
#include <set>
#include <thread>

using std::string;
using std::vector;

// ===========================================================================
// small helpers
// ===========================================================================
namespace {

struct Db {
    sqlite3* p = nullptr;
    explicit Db(const string& path, int flags = SQLITE_OPEN_READWRITE, int busy_ms = 5000) {
        if (sqlite3_open_v2(path.c_str(), &p, flags, nullptr) != SQLITE_OK) {
            string msg = p ? sqlite3_errmsg(p) : "out of memory";
            if (p) sqlite3_close(p);
            p = nullptr;
            throw std::runtime_error("cannot open database '" + path + "': " + msg);
        }
        sqlite3_busy_timeout(p, busy_ms);
    }
    ~Db() { if (p) sqlite3_close(p); }
    sqlite3* get() const { return p; }
    sqlite3* release() { sqlite3* r = p; p = nullptr; return r; }
    Db(const Db&) = delete;
    Db& operator=(const Db&) = delete;
};

struct Stmt {
    sqlite3_stmt* p = nullptr;
    Stmt(sqlite3* db, const string& sql) {
        if (sqlite3_prepare_v2(db, sql.c_str(), -1, &p, nullptr) != SQLITE_OK) {
            string msg = sqlite3_errmsg(db);
            throw std::runtime_error(msg + "  [SQL: " + sql + "]");
        }
    }
    ~Stmt() { if (p) sqlite3_finalize(p); }
    operator sqlite3_stmt*() const { return p; }
    Stmt(const Stmt&) = delete;
    Stmt& operator=(const Stmt&) = delete;
};

string text_at(sqlite3_stmt* st, int i) {
    const unsigned char* t = sqlite3_column_text(st, i);
    return t ? reinterpret_cast<const char*>(t) : "";
}

void exec(sqlite3* db, const char* sql) {
    char* err = nullptr;
    if (sqlite3_exec(db, sql, nullptr, nullptr, &err) != SQLITE_OK) {
        string msg = err ? err : sqlite3_errmsg(db);
        if (err) sqlite3_free(err);
        throw std::runtime_error(msg);
    }
}

long long scalar(sqlite3* db, const string& sql) {
    Stmt st(db, sql);
    return sqlite3_step(st) == SQLITE_ROW ? sqlite3_column_int64(st, 0) : 0;
}

bool has_table(sqlite3* db, const char* name) {
    Stmt st(db, "SELECT 1 FROM sqlite_master WHERE type='table' AND name = ?");
    sqlite3_bind_text(st, 1, name, -1, SQLITE_STATIC);
    return sqlite3_step(st) == SQLITE_ROW;
}

void report(const ProgressFn& p, int percent, const string& stage) {
    if (p) p(percent, stage);
}

string quote_ident(const string& s) {
    string out;
    for (char c : s) {
        if (c == '"') out += '"';
        out += c;
    }
    return out;
}

string in_placeholders(size_t n) {
    string s;
    for (size_t i = 0; i < n; ++i) s += (i ? ",?" : "?");
    return s;
}

vector<string> all_tables(sqlite3* db) {
    vector<string> out;
    Stmt st(db, "SELECT name FROM sqlite_master WHERE type='table' AND name NOT LIKE 'sqlite_%'");
    while (sqlite3_step(st) == SQLITE_ROW) out.push_back(text_at(st, 0));
    return out;
}

vector<string> table_columns(sqlite3* db, const string& table) {
    vector<string> out;
    Stmt st(db, "PRAGMA table_info(\"" + quote_ident(table) + "\")");
    while (sqlite3_step(st) == SQLITE_ROW) out.push_back(text_at(st, 1));
    return out;
}

bool has_dbstat(sqlite3* db) {
    try {
        Stmt st(db, "SELECT 1 FROM dbstat LIMIT 1");
        return sqlite3_step(st) == SQLITE_ROW;
    } catch (const std::exception&) {
        return false;
    }
}

// ---------------------------------------------------------------------------
// Minimal JSON string extraction for top-level keys of opencode payloads.
// Anything that does not look exactly as expected reports false, and callers
// treat that as "do not touch this row".
// ---------------------------------------------------------------------------
// Reads the string literal whose opening quote was already consumed: `i` points
// just past it. Returns the position after the closing quote, or npos when the
// value is truncated. Shared by json_find_string() and by the v2 message scan
// below, which has to walk several strings inside one payload.
size_t json_string_at(const string& json, size_t i, string* out) {
    string value;
    while (i < json.size()) {
        char c = json[i];
        if (c == '\\' && i + 1 < json.size()) {
            char e = json[i + 1];
            switch (e) {
            case 'n': value += '\n'; break;
            case 't': value += '\t'; break;
            case 'r': value += '\r'; break;
            case '"': value += '"'; break;
            case '\\': value += '\\'; break;
            case '/': value += '/'; break;
            case 'u': {
                if (i + 5 < json.size()) {
                    unsigned cp = 0;
                    for (int k = 2; k <= 5; ++k) {
                        char h = json[i + (size_t)k];
                        cp <<= 4;
                        if (h >= '0' && h <= '9') cp |= (unsigned)(h - '0');
                        else if (h >= 'a' && h <= 'f') cp |= (unsigned)(h - 'a' + 10);
                        else if (h >= 'A' && h <= 'F') cp |= (unsigned)(h - 'A' + 10);
                    }
                    if (cp && cp < 0x80) {
                        value += (char)cp;
                    } else if (cp >= 0x80 && cp < 0x800) {
                        value += (char)(0xC0 | (cp >> 6));
                        value += (char)(0x80 | (cp & 0x3F));
                    } else if (cp >= 0x800) {
                        value += (char)(0xE0 | (cp >> 12));
                        value += (char)(0x80 | ((cp >> 6) & 0x3F));
                        value += (char)(0x80 | (cp & 0x3F));
                    }
                    i += 4;
                }
                break;
            }
            default: value += e; break;
            }
            i += 2;
            continue;
        }
        if (c == '"') {
            if (out) *out = value;
            return i + 1;
        }
        value += c;
        ++i;
    }
    return string::npos;
}

bool json_find_string(const string& json, const string& key, string* out) {
    string needle = "\"" + key + "\":\"";
    size_t pos = json.find(needle);
    if (pos == string::npos) {
        needle = "\"" + key + "\": \"";
        pos = json.find(needle);
        if (pos == string::npos) return false;
    }
    return json_string_at(json, pos + needle.size(), out) != string::npos;
}

// Reading a 512 byte head of a multi-hundred-MB cell would make sqlite
// materialise the whole thing, so oversized rows are read as "unknown" and are
// therefore never deletion candidates.
const long long kMaxHeadRowBytes = 1024 * 1024;

}  // namespace

// ===========================================================================
// Space analysis
// ===========================================================================
DbReport analyze_database(const string& db_path, int top_n, const ProgressFn& progress) {
    DbReport r;
    report(progress, 0, "opening database");
    Db db(db_path);

    r.page_size = scalar(db.get(), "PRAGMA page_size");
    r.page_count = scalar(db.get(), "PRAGMA page_count");
    r.freelist_count = scalar(db.get(), "PRAGMA freelist_count");
    r.file_bytes = r.page_size * r.page_count;
    r.free_bytes = r.page_size * r.freelist_count;

    report(progress, 4, "counting rows");
    // opencode v2 renamed these (see opencode_data.hpp): sessions live in
    // `session_v2` and whole messages in `session_message`. `part` only exists in
    // v1 databases, where it holds the individual message parts.
    r.session_rows = scalar(db.get(), "SELECT COUNT(*) FROM \"" + session_table(db.get()) + "\"");
    r.message_rows = scalar(db.get(), "SELECT COUNT(*) FROM \"" + message_table(db.get()) + "\"");
    r.part_rows = has_table(db.get(), "part") ? scalar(db.get(), "SELECT COUNT(*) FROM part") : 0;

    // Per-table footprint. dbstat gives exact page usage but needs
    // SQLITE_ENABLE_DBSTAT_VTAB -- the MinGW sqlite3 3.23 in use here does not
    // have it, so fall back to summing the text columns. That is an estimate,
    // which is plenty to answer "which table eats the disk".
    r.dbstat_available = has_dbstat(db.get());
    r.v2_schema = has_table(db.get(), "session_v2");
    if (r.dbstat_available) {
        report(progress, 10, "scanning table pages (dbstat)");
        Stmt st(db.get(), "SELECT name, SUM(pgsize) FROM dbstat GROUP BY name ORDER BY 2 DESC");
        while (sqlite3_step(st) == SQLITE_ROW) {
            UsageEntry e;
            e.name = text_at(st, 0);
            e.bytes = sqlite3_column_int64(st, 1);
            r.tables.push_back(e);
        }
    } else {
        vector<string> tables = all_tables(db.get());
        int done = 0;
        for (auto& t : tables) {
            report(progress, 10 + (int)(42.0 * done / (double)std::max<size_t>(1, tables.size())),
                   "estimating table content: " + t);
            vector<string> cols = table_columns(db.get(), t);
            string sum_expr;
            for (auto& c : cols) {
                if (!sum_expr.empty()) sum_expr += "+";
                sum_expr += "COALESCE(LENGTH(\"" + quote_ident(c) + "\"),0)";
            }
            ++done;
            if (sum_expr.empty()) continue;
            UsageEntry e;
            e.name = t;
            try {
                Stmt st(db.get(), "SELECT COUNT(*), SUM(" + sum_expr + ") FROM \"" +
                                      quote_ident(t) + "\"");
                if (sqlite3_step(st) == SQLITE_ROW) {
                    e.rows = sqlite3_column_int64(st, 0);
                    e.bytes = sqlite3_column_int64(st, 1);
                }
            } catch (const std::exception&) {
                continue;
            }
            if (e.bytes > 0 || e.rows > 0) r.tables.push_back(e);
        }
        std::sort(r.tables.begin(), r.tables.end(),
                  [](const UsageEntry& a, const UsageEntry& b) { return a.bytes > b.bytes; });
    }
    if ((int)r.tables.size() > top_n) r.tables.resize((size_t)top_n);

    // Event log: the dominant consumer on a real database.
    //
    // Everything below comes from ONE sequential pass folded in C++. Doing this
    // in SQL is a trap on this schema: `event` has two indexes and both start
    // with aggregate_id, so `GROUP BY type` has no index to lean on and falls
    // back to a sorter that carries the `data` column. Measured on the 4.4 GB
    // database this module was written for, `SELECT type, COUNT(*),
    // SUM(LENGTH(data)) FROM event GROUP BY type` takes 70-80 s and throws ~34
    // GB of temp file at the disk (SQLite unlinks the temp files immediately,
    // but Windows only returns the space when the last handle closes, so the
    // drive looks like it is filling up while it runs). The streaming version
    // takes ~15 s, needs a few MB and writes no temp files at all.
    report(progress, 55, "measuring the event log");
    r.event_rows = scalar(db.get(), "SELECT COUNT(*) FROM event");
    const long long planned_rows = r.event_rows > 0 ? r.event_rows : 1;

    std::map<string, std::pair<long long, long long>> by_type;      // type -> rows, bytes
    std::map<string, std::pair<long long, long long>> by_session;   // aggregate -> rows, bytes
    {
        std::set<string> live_sessions;
        {
            Stmt st(db.get(), "SELECT id FROM \"" + session_table(db.get()) + "\"");
            while (sqlite3_step(st) == SQLITE_ROW) live_sessions.insert(text_at(st, 0));
        }

        report(progress, 58, "scanning the event log");
        Stmt st(db.get(), "SELECT aggregate_id, type, COALESCE(LENGTH(data),0) FROM event");
        long long seen = 0;
        while (sqlite3_step(st) == SQLITE_ROW) {
            const string agg = text_at(st, 0);
            const string type = text_at(st, 1);
            const long long bytes = sqlite3_column_int64(st, 2);

            auto& t = by_type[type];
            t.first += 1;
            t.second += bytes;

            auto& s = by_session[agg];
            s.first += 1;
            s.second += bytes;

            if (!live_sessions.count(agg)) {
                r.orphan_event_rows += 1;
                r.orphan_event_bytes += bytes;
            }

            if (((++seen) & 0x3FFF) == 0)
                report(progress, 58 + (int)(28.0 * (double)seen / (double)planned_rows),
                       "scanning the event log " + std::to_string(seen) + " rows");
        }
    }

    r.event_bytes = 0;
    for (auto& kv : by_type) {
        UsageEntry e;
        e.name = kv.first;
        e.rows = kv.second.first;
        e.bytes = kv.second.second;
        r.event_bytes += e.bytes;
        r.event_types.push_back(e);
    }
    std::sort(r.event_types.begin(), r.event_types.end(),
              [](const UsageEntry& a, const UsageEntry& b) { return a.bytes > b.bytes; });
    if ((int)r.event_types.size() > top_n) r.event_types.resize((size_t)top_n);

    report(progress, 88, "ranking sessions by event footprint");
    {
        std::vector<std::pair<long long, const string*>> ranked;
        ranked.reserve(by_session.size());
        for (auto& kv : by_session) ranked.push_back({kv.second.second, &kv.first});
        std::sort(ranked.begin(), ranked.end(),
                  [](const std::pair<long long, const string*>& a,
                     const std::pair<long long, const string*>& b) { return a.first > b.first; });

        int n = 0;
        for (auto& item : ranked) {
            if (n >= top_n) break;
            UsageEntry e;
            e.name = *item.second;
            e.rows = by_session[*item.second].first;
            e.bytes = item.first;
            try {
                Stmt t(db.get(), "SELECT title FROM \"" + session_table(db.get()) +
                                         "\" WHERE id = ?");
                sqlite3_bind_text(t, 1, e.name.c_str(), -1, SQLITE_TRANSIENT);
                if (sqlite3_step(t) == SQLITE_ROW) e.extra = utf8_ellipsize(text_at(t, 0), 40);
            } catch (const std::exception&) {
            }
            r.sessions.push_back(e);
            ++n;
        }
    }
    report(progress, 94, "looking for oversized rows");
    try {
        Stmt st(db.get(), "SELECT aggregate_id, type, LENGTH(data) FROM event"
                          " WHERE LENGTH(data) > 10000000 ORDER BY 3 DESC LIMIT 5");
        while (sqlite3_step(st) == SQLITE_ROW) {
            UsageEntry e;
            e.name = "single row of " + text_at(st, 1) + "   " + utf8_ellipsize(text_at(st, 0), 32);
            e.bytes = sqlite3_column_int64(st, 2);
            e.rows = 1;
            r.big_items.push_back(e);
        }
    } catch (const std::exception&) {
    }
    for (auto& s : r.sessions) {
        if (s.bytes < 100LL * 1024 * 1024) continue;
        UsageEntry e;
        e.name = "session events " + s.name;
        e.bytes = s.bytes;
        e.rows = s.rows;
        r.big_items.push_back(e);
    }

    report(progress, 100, "analysis complete");
    return r;
}

// ===========================================================================
// Orphan events: rows whose aggregate (session) no longer exists
// ===========================================================================
ReclaimResult delete_orphan_events(const string& db_path, const ProgressFn& progress) {
    ReclaimResult out;
    Db db(db_path);
    out.file_before = file_size(db_path);

    report(progress, 2, "listing surviving sessions");
    std::set<string> live;
    {
        Stmt st(db.get(), "SELECT id FROM \"" + session_table(db.get()) + "\"");
        while (sqlite3_step(st) == SQLITE_ROW) live.insert(text_at(st, 0));
    }

    report(progress, 5, "listing event aggregates");
    vector<string> aggregates;
    {
        Stmt st(db.get(), "SELECT DISTINCT aggregate_id FROM event");
        while (sqlite3_step(st) == SQLITE_ROW) {
            string id = text_at(st, 0);
            if (!live.count(id)) aggregates.push_back(id);
        }
    }

    if (aggregates.empty()) {
        report(progress, 100, "no orphan events to clean");
        out.file_after = out.file_before;
        return out;
    }

    // Delete per aggregate id (indexed), in batches: progress stays visible and
    // the write lock is not held for one huge transaction.
    const size_t kBatch = 40;
    exec(db.get(), "BEGIN IMMEDIATE");
    try {
        for (size_t i = 0; i < aggregates.size(); i += kBatch) {
            size_t n = std::min(kBatch, aggregates.size() - i);
            string ph = in_placeholders(n);
            {
                Stmt sum(db.get(), "SELECT COALESCE(SUM(LENGTH(data)),0) FROM event"
                                   " WHERE aggregate_id IN (" + ph + ")");
                for (size_t k = 0; k < n; ++k)
                    sqlite3_bind_text(sum, (int)k + 1, aggregates[i + k].c_str(), -1,
                                      SQLITE_TRANSIENT);
                if (sqlite3_step(sum) == SQLITE_ROW) out.bytes += sqlite3_column_int64(sum, 0);
            }
            Stmt del(db.get(), "DELETE FROM event WHERE aggregate_id IN (" + ph + ")");
            for (size_t k = 0; k < n; ++k)
                sqlite3_bind_text(del, (int)k + 1, aggregates[i + k].c_str(), -1, SQLITE_TRANSIENT);
            if (sqlite3_step(del) != SQLITE_DONE)
                throw std::runtime_error(string("delete failed: ") + sqlite3_errmsg(db.get()));
            out.rows += sqlite3_changes(db.get());
            report(progress, 8 + (int)(88.0 * (i + n) / (double)aggregates.size()),
                   "deleting orphan events " + std::to_string(std::min(i + n, aggregates.size())) + "/" +
                       std::to_string(aggregates.size()) + " aggregates");
        }
        exec(db.get(), "COMMIT");
    } catch (...) {
        sqlite3_exec(db.get(), "ROLLBACK", nullptr, nullptr, nullptr);
        throw;
    }

    out.file_after = file_size(db_path);
    report(progress, 100, "done (run VACUUM to return the space to the filesystem)");
    return out;
}

// ===========================================================================
// Event log compaction: keep only the newest snapshot of each message part
// ===========================================================================
namespace {

struct RowInfo {
    long long rowid;
    long long seq;
    long long bytes;
    int group;          // index into the group list, -1 = unreadable
};

}  // namespace

CompactionPlan plan_event_compaction(const string& db_path, const ProgressFn& progress) {
    CompactionPlan plan;
    Db db(db_path);

    report(progress, 2, "counting part update events");
    plan.total_rows = scalar(db.get(), "SELECT COUNT(*) FROM event"
                                       " WHERE type = 'message.part.updated.1'");
    if (plan.total_rows == 0) {
        report(progress, 100, "nothing to compact");
        return plan;
    }

    // One pass, keeping only (rowid, seq, bytes, group) per row: a second scan
    // over 190k large rows would double the cost for nothing.
    vector<RowInfo> rows;
    rows.reserve((size_t)plan.total_rows);
    std::map<string, int> group_index;   // part id -> group
    vector<string> group_name;

    report(progress, 5, "reading events (only the head of oversized rows)");
    {
        Stmt st(db.get(), "SELECT rowid, seq, LENGTH(data),"
                          " CASE WHEN LENGTH(data) < 1048576 THEN SUBSTR(data,1,512) ELSE '' END"
                          " FROM event WHERE type = 'message.part.updated.1'");
        long long seen = 0;
        int rc;
        while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
            RowInfo ri;
            ri.rowid = sqlite3_column_int64(st, 0);
            ri.seq = sqlite3_column_int64(st, 1);
            ri.bytes = sqlite3_column_int64(st, 2);
            ri.group = -1;
            string head = text_at(st, 3);
            string part_id;
            if (!head.empty() && json_find_string(head, "id", &part_id) && !part_id.empty()) {
                auto it = group_index.find(part_id);
                if (it == group_index.end()) {
                    int idx = (int)group_name.size();
                    group_index[part_id] = idx;
                    group_name.push_back(part_id);
                    ri.group = idx;
                } else {
                    ri.group = it->second;
                }
            } else {
                plan.skipped_rows++;
            }
            plan.total_bytes += ri.bytes;
            rows.push_back(ri);
            ++seen;
            if ((seen & 0x7FF) == 0)
                report(progress, 5 + (int)(70.0 * seen / (double)plan.total_rows),
                       "reading events " + std::to_string(seen) + "/" + std::to_string(plan.total_rows));
        }
        if (rc != SQLITE_DONE)
            throw std::runtime_error(string("scan failed: ") + sqlite3_errmsg(db.get()));
    }

    report(progress, 80, "finding the newest version of each part");
    plan.parts = (long long)group_name.size();
    vector<long long> keep_seq((size_t)plan.parts, -1);
    vector<long long> keep_rowid((size_t)plan.parts, 0);
    for (auto& ri : rows) {
        if (ri.group < 0) continue;
        size_t g = (size_t)ri.group;
        if (ri.seq > keep_seq[g] || (ri.seq == keep_seq[g] && ri.rowid > keep_rowid[g])) {
            keep_seq[g] = ri.seq;
            keep_rowid[g] = ri.rowid;
        }
    }

    report(progress, 88, "collecting redundant versions");
    vector<long long> worst_rows;
    for (auto& ri : rows) {
        if (ri.group < 0) continue;
        if (ri.rowid == keep_rowid[(size_t)ri.group]) continue;   // the newest survives
        plan.removable_rowids.push_back(ri.rowid);
        plan.removable_bytes += ri.bytes;
    }
    plan.removable_rows = (long long)plan.removable_rowids.size();

    // Worst offenders, so the user can see what caused the growth.
    worst_rows.assign((size_t)plan.parts, 0);
    vector<long long> worst_bytes((size_t)plan.parts, 0);
    for (auto& ri : rows) {
        if (ri.group < 0) continue;
        worst_rows[(size_t)ri.group] += 1;
        worst_bytes[(size_t)ri.group] += ri.bytes;
    }
    vector<std::pair<long long, int>> byrows;
    for (size_t i = 0; i < worst_rows.size(); ++i) byrows.push_back({worst_rows[i], (int)i});
    std::sort(byrows.begin(), byrows.end(),
              [](const std::pair<long long, int>& a, const std::pair<long long, int>& b) {
                  return a.first > b.first;
              });
    for (size_t i = 0; i < byrows.size() && i < 8; ++i) {
        UsageEntry e;
        e.name = group_name[(size_t)byrows[i].second];
        e.rows = byrows[i].first;
        e.bytes = worst_bytes[(size_t)byrows[i].second];
        plan.worst_parts.push_back(e);
    }

    report(progress, 100, "analysis complete (nothing was modified)");
    return plan;
}

ReclaimResult apply_event_compaction(const string& db_path, const CompactionPlan& plan,
                                     const ProgressFn& progress) {
    ReclaimResult out;
    Db db(db_path);
    out.file_before = file_size(db_path);

    if (plan.removable_rowids.empty()) {
        out.file_after = out.file_before;
        report(progress, 100, "nothing to remove");
        return out;
    }

    const size_t kBatch = 400;
    exec(db.get(), "BEGIN IMMEDIATE");
    try {
        for (size_t i = 0; i < plan.removable_rowids.size(); i += kBatch) {
            size_t n = std::min(kBatch, plan.removable_rowids.size() - i);
            Stmt del(db.get(), "DELETE FROM event WHERE rowid IN (" + in_placeholders(n) + ")");
            for (size_t k = 0; k < n; ++k)
                sqlite3_bind_int64(del, (int)k + 1, plan.removable_rowids[i + k]);
            if (sqlite3_step(del) != SQLITE_DONE)
                throw std::runtime_error(string("delete failed: ") + sqlite3_errmsg(db.get()));
            out.rows += sqlite3_changes(db.get());
            report(progress, (int)(95.0 * (i + n) / (double)plan.removable_rowids.size()),
                   "removing redundant versions " + std::to_string(out.rows) + "/" +
                       std::to_string(plan.removable_rowids.size()));
        }
        exec(db.get(), "COMMIT");
    } catch (...) {
        sqlite3_exec(db.get(), "ROLLBACK", nullptr, nullptr, nullptr);
        throw;
    }
    out.bytes = plan.removable_bytes;
    out.file_after = file_size(db_path);
    report(progress, 100, "done (run VACUUM to release the disk space)");
    return out;
}

// ===========================================================================
// VACUUM / backup / busy check
// ===========================================================================
namespace {

struct VacuumCtx {
    const ProgressFn* progress;
    long long steps;
};

int vacuum_progress_cb(void* ctx) {
    VacuumCtx* c = (VacuumCtx*)ctx;
    c->steps += 1;
    if (c->progress && *c->progress) {
        // VACUUM's VM step count is not a page percentage; this is a bounded
        // pseudo-progress that slows towards the end rather than pretending to
        // be exact.
        int pct = 5 + (int)((1.0 - 1.0 / (1.0 + (double)c->steps / 20000.0)) * 90.0);
        if (pct > 95) pct = 95;
        (*c->progress)(pct, "running VACUUM (minutes on a large database)");
    }
    return 0;
}

}  // namespace

bool vacuum_database(const string& db_path, ReclaimResult* out, const ProgressFn& progress) {
    if (out) *out = ReclaimResult();
    try {
        report(progress, 2, "opening database");
        Db db(db_path);
        long long before = file_size(db_path);
        VacuumCtx ctx{&progress, 0};
        sqlite3_progress_handler(db.get(), 20000, vacuum_progress_cb, &ctx);
        report(progress, 5, "running VACUUM (needs temp space about the size of the database)");
        exec(db.get(), "VACUUM");
        sqlite3_progress_handler(db.get(), 0, nullptr, nullptr);
        long long after = file_size(db_path);
        if (out) {
            out->ok = true;
            out->file_before = before;
            out->file_after = after;
            out->bytes = before > after ? before - after : 0;
        }
        report(progress, 100, "VACUUM finished");
        return true;
    } catch (const std::exception& e) {
        if (out) {
            out->ok = false;
            out->error = e.what();
        }
        report(progress, 100, string("VACUUM failed: ") + e.what());
        return false;
    }
}

bool backup_database(const string& db_path, const string& dest, long long* bytes_copied,
                     string* error) {
    if (error) error->clear();
    if (bytes_copied) *bytes_copied = 0;
    try {
        Db src(db_path, SQLITE_OPEN_READONLY);
        // The backup API replaces the destination database wholesale (and
        // truncates it), so an existing file needs no special handling.
        Db dst(dest, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE);
        sqlite3_backup* bk = sqlite3_backup_init(dst.get(), "main", src.get(), "main");
        if (!bk)
            throw std::runtime_error(string("backup init failed: ") + sqlite3_errmsg(dst.get()));
        int rc;
        do {
            rc = sqlite3_backup_step(bk, 512);
        } while (rc == SQLITE_OK || rc == SQLITE_BUSY);
        int finish_code = sqlite3_backup_finish(bk);
        sqlite3_close(dst.release());   // release so the guard cannot close twice
        if (rc != SQLITE_DONE || finish_code != SQLITE_OK)
            throw std::runtime_error(string("backup failed (") + std::to_string(rc) + ")");
        if (bytes_copied) *bytes_copied = file_size(dest);
        return true;
    } catch (const std::exception& e) {
        if (error) *error = e.what();
        return false;
    }
}

bool database_is_busy(const string& db_path) {
    sqlite3* db = nullptr;
    if (sqlite3_open_v2(db_path.c_str(), &db, SQLITE_OPEN_READWRITE, nullptr) != SQLITE_OK) {
        if (db) sqlite3_close(db);
        return true;
    }
    sqlite3_busy_timeout(db, 200);
    char* err = nullptr;
    bool busy = sqlite3_exec(db, "BEGIN IMMEDIATE", nullptr, nullptr, &err) != SQLITE_OK;
    if (err) sqlite3_free(err);
    if (!busy) sqlite3_exec(db, "ROLLBACK", nullptr, nullptr, nullptr);
    sqlite3_close(db);
    return busy;
}

// ===========================================================================
// Session preview and export
// ===========================================================================
namespace {

// opencode v2 keeps one row per message in `session_message`; the readable text
// lives in the `content` array, whose entries have the same {"type","text"}
// shape as v1 parts. Pull every `"type":"text"` entry's string out, in order.
string v2_message_text(const string& data) {
    string out;
    size_t pos = 0;
    while (out.size() < 1500) {
        size_t marker = data.find("\"type\":\"text\"", pos);
        if (marker == string::npos) marker = data.find("\"type\": \"text\"", pos);
        if (marker == string::npos) break;
        size_t k = data.find("\"text\":", marker);
        if (k == string::npos) break;
        k += 7;
        while (k < data.size() && data[k] == ' ') ++k;
        if (k >= data.size() || data[k] != '"') {
            pos = marker + 1;
            continue;
        }
        string s;
        size_t end = json_string_at(data, k + 1, &s);
        if (end == string::npos) break;
        if (!out.empty()) out += '\n';
        out += s;
        pos = end;
    }
    return out;
}

}  // namespace

vector<PreviewMessage> load_session_preview(const string& db_path, const string& session_id,
                                            int limit) {
    Db db(db_path);
    // opencode v2 keeps whole messages in `session_message` (type is
    // user/assistant/system); v1 kept individual parts in `part` with the role
    // on the parent message's JSON.
    const bool v2 = has_table(db.get(), "session_message");

    // Newest first, then flipped below, so "limit" means "the last N".
    vector<PreviewMessage> reversed;

    if (v2) {
        Stmt st(db.get(), "SELECT type, time_created, data FROM session_message"
                          " WHERE session_id = ? ORDER BY seq DESC LIMIT ?");
        sqlite3_bind_text(st, 1, session_id.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(st, 2, limit);
        while (sqlite3_step(st) == SQLITE_ROW) {
            PreviewMessage m;
            m.role = text_at(st, 0);
            m.time_created = sqlite3_column_int64(st, 1);
            string data = text_at(st, 2);
            m.kind = m.role.empty() ? "message" : m.role;
            m.text = utf8_ellipsize(v2_message_text(data), 300);
            if (m.text.empty()) {
                // Reasoning-only or tool-only turn: say why there is no text.
                string finish;
                json_find_string(data, "finish", &finish);
                m.text = finish.empty() ? "(no text)" : ("[" + finish + "]");
            }
            reversed.push_back(m);
        }
    } else {
        Stmt st(db.get(),
                "SELECT p.data, p.time_created, m.data FROM part p"
                " LEFT JOIN message m ON m.id = p.message_id"
                " WHERE p.session_id = ?"
                " ORDER BY p.time_created DESC, p.id DESC LIMIT ?");
        sqlite3_bind_text(st, 1, session_id.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(st, 2, limit);
        while (sqlite3_step(st) == SQLITE_ROW) {
            PreviewMessage m;
            m.time_created = sqlite3_column_int64(st, 1);
            string data = text_at(st, 0);
            string role;
            if (json_find_string(text_at(st, 2), "role", &role)) m.role = role;
            string type;
            json_find_string(data, "type", &type);
            m.kind = type.empty() ? "part" : type;
            if (type == "text") {
                string text;
                if (json_find_string(data, "text", &text)) m.text = utf8_ellipsize(text, 300);
            } else if (type == "tool") {
                string tool;
                json_find_string(data, "tool", &tool);
                m.text = tool.empty() ? "[tool]" : ("[tool] " + tool);
            } else if (!type.empty()) {
                m.text = "[" + type + "]";
            }
            if (m.text.empty()) m.text = "(no text)";
            reversed.push_back(m);
        }
    }
    std::reverse(reversed.begin(), reversed.end());
    return reversed;
}

namespace {

string json_escape(const string& s) {
    string out;
    out.reserve(s.size() + 16);
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
                snprintf(buf, sizeof(buf), "\\u%04x", c);
                out += buf;
            } else {
                out += (char)c;
            }
        }
    }
    return out;
}

void dump_table(FILE* fp, sqlite3* db, const string& table, const string& where_col,
                const string& value, bool* first_row) {
    vector<string> cols = table_columns(db, table);
    if (cols.empty()) return;
    string sql = "SELECT * FROM \"" + quote_ident(table) + "\"";
    if (!where_col.empty()) sql += " WHERE \"" + quote_ident(where_col) + "\" = ?";
    Stmt st(db, sql);
    if (!where_col.empty()) sqlite3_bind_text(st, 1, value.c_str(), -1, SQLITE_TRANSIENT);
    while (sqlite3_step(st) == SQLITE_ROW) {
        if (!*first_row) fprintf(fp, ",\n");
        *first_row = false;
        fprintf(fp, "    {");
        for (int i = 0; i < (int)cols.size(); ++i) {
            if (i) fprintf(fp, ", ");
            fprintf(fp, "\"%s\": ", json_escape(cols[(size_t)i]).c_str());
            int t = sqlite3_column_type(st, i);
            if (t == SQLITE_NULL) fprintf(fp, "null");
            else if (t == SQLITE_INTEGER || t == SQLITE_FLOAT) fprintf(fp, "%s", text_at(st, i).c_str());
            else fprintf(fp, "\"%s\"", json_escape(text_at(st, i)).c_str());
        }
        fprintf(fp, "}");
    }
}

}  // namespace

bool export_sessions_json(const string& db_path, const vector<string>& session_ids,
                          const string& out_dir, vector<string>* written, string* error) {
    if (error) error->clear();
    try {
        if (!is_dir(out_dir))
            if (!make_dir(out_dir))
                throw std::runtime_error("cannot create output directory: " + out_dir);
        Db db(db_path, SQLITE_OPEN_READONLY);

        for (auto& id : session_ids) {
            string safe;
            for (char c : id)
                safe += (std::isalnum((unsigned char)c) || c == '-' || c == '_') ? c : '_';
            string path = path_join(out_dir, safe + ".json");
            FILE* fp = _wfopen(utf8_to_wide(path).c_str(), L"wb");
            if (!fp) throw std::runtime_error("cannot write " + path);
            fprintf(fp, "{\n  \"session_id\": \"%s\",\n  \"session\": [\n",
                    json_escape(id).c_str());
            bool first = true;
            dump_table(fp, db.get(), "session", "id", id, &first);
            fprintf(fp, "\n  ],\n  \"messages\": [\n");
            first = true;
            dump_table(fp, db.get(), "message", "session_id", id, &first);
            fprintf(fp, "\n  ],\n  \"parts\": [\n");
            first = true;
            dump_table(fp, db.get(), "part", "session_id", id, &first);
            fprintf(fp, "\n  ]\n}\n");
            fclose(fp);
            if (written) written->push_back(path);
        }
        return true;
    } catch (const std::exception& e) {
        if (error) *error = e.what();
        return false;
    }
}

// ===========================================================================
// Background job
// ===========================================================================
const char* job_type_name(JobType type) {
    switch (type) {
    case JobType::Report: return "database report";
    case JobType::CleanupOrphans: return "clean orphan events";
    case JobType::CompactEvents: return "compact event log";
    case JobType::Vacuum: return "VACUUM";
    case JobType::Backup: return "backup database";
    default: return "idle";
    }
}

struct MaintenanceJob::Impl {
    mutable std::mutex m;
    std::thread worker;
    JobStatus st;
};

MaintenanceJob::MaintenanceJob() : impl_(new Impl()) {}

MaintenanceJob::~MaintenanceJob() {
    wait();
    delete impl_;
}

bool MaintenanceJob::running() const {
    std::lock_guard<std::mutex> lk(impl_->m);
    return impl_->st.running;
}

JobStatus MaintenanceJob::status() const {
    std::lock_guard<std::mutex> lk(impl_->m);
    return impl_->st;
}

void MaintenanceJob::wait() {
    if (impl_->worker.joinable()) impl_->worker.join();
}

bool MaintenanceJob::start(JobType type, const string& db_path, const string& arg1) {
    if (running()) return false;
    wait();

    {
        std::lock_guard<std::mutex> lk(impl_->m);
        impl_->st = JobStatus();
        impl_->st.type = type;
        impl_->st.running = true;
        impl_->st.stage = string(job_type_name(type)) + "…";
    }

    impl_->worker = std::thread([this, type, db_path, arg1]() {
        Impl* im = impl_;
        ProgressFn progress = [im](int pct, const string& stage) {
            std::lock_guard<std::mutex> lk(im->m);
            im->st.percent = pct;
            im->st.stage = stage;
        };
        clock_t t0 = clock();
        JobStatus result;
        bool ok = true;
        try {
            switch (type) {
            case JobType::Report:
                result.report = analyze_database(db_path, 12, progress);
                break;
            case JobType::CleanupOrphans:
                result.result = delete_orphan_events(db_path, progress);
                break;
            case JobType::CompactEvents:
                result.plan = plan_event_compaction(db_path, progress);
                if (!arg1.empty() && result.plan.removable_rows > 0) {
                    progress(99, "backing up to " + arg1);
                    long long copied = 0;
                    string err;
                    if (!backup_database(db_path, arg1, &copied, &err))
                        throw std::runtime_error("backup failed: " + err);
                    progress(99, "removing redundant versions");
                    result.result = apply_event_compaction(db_path, result.plan, progress);
                }
                break;
            case JobType::Vacuum:
                ok = vacuum_database(db_path, &result.result, progress);
                break;
            case JobType::Backup: {
                long long copied = 0;
                string err;
                if (!backup_database(db_path, arg1, &copied, &err))
                    throw std::runtime_error(err);
                result.result.ok = true;
                result.result.bytes = copied;
                result.result.file_after = copied;
                break;
            }
            default:
                break;
            }
        } catch (const std::exception& e) {
            ok = false;
            result.result.ok = false;
            result.result.error = e.what();
            result.error = e.what();
        }

        std::lock_guard<std::mutex> lk(im->m);
        im->st.report = result.report;
        im->st.plan = result.plan;
        im->st.result = result.result;
        im->st.ok = ok;
        im->st.error = result.error.empty() ? result.result.error : result.error;
        im->st.percent = 100;
        im->st.finished = true;
        im->st.running = false;
        im->st.elapsed = (double)(clock() - t0) / CLOCKS_PER_SEC;
    });
    return true;
}
