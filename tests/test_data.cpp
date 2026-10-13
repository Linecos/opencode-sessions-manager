// Tests for the OpenCode Session Manager data layer.
//
// These exercise the parts that are easy to get wrong and impossible to notice
// by clicking around: non-ASCII paths, the schema-driven cascade on session
// delete, locking, orphan-diff cleanup and UTF-8 safe truncation.
//
// Build (see build_tests.bat):
//   g++ -std=c++17 -O2 -I"C:\MinGW\opt\include" tests\test_data.cpp opencode_data.cpp
//       -L"C:\MinGW\opt\lib" -lsqlite3 -luser32 -lshell32 -lole32 -o dist\test_data.exe
//
// Run:
//   dist\test_data.exe [scratch-dir]

#include <sqlite3.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN 1
#include <windows.h>
#endif

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "opencode_data.hpp"
#include "db_maintenance.hpp"
#include "session_view.hpp"

using std::string;

static int g_pass = 0;
static int g_fail = 0;

#define CHECK(cond, msg)                                                            \
    do {                                                                            \
        if (cond) {                                                                 \
            ++g_pass;                                                               \
            printf("  ok    %s\n", msg);                                            \
        } else {                                                                    \
            ++g_fail;                                                               \
            printf("  FAIL  %s   (%s:%d)\n", msg, __FILE__, __LINE__);              \
        }                                                                           \
    } while (0)

#define SECTION(name) printf("\n[%s]\n", name)

// ---------------------------------------------------------------------------
// helpers
// ---------------------------------------------------------------------------
static void db_exec(sqlite3* db, const string& sql) {
    char* err = nullptr;
    if (sqlite3_exec(db, sql.c_str(), nullptr, nullptr, &err) != SQLITE_OK) {
        printf("  !! sql error: %s\n", err ? err : "?");
        if (err) sqlite3_free(err);
    }
}

static void db_open(const string& path, sqlite3** db) {
    if (sqlite3_open(path.c_str(), db) != SQLITE_OK) {
        printf("  !! cannot open %s\n", path.c_str());
        *db = nullptr;
    }
}

static int db_scalar(sqlite3* db, const string& sql) {
    sqlite3_stmt* st = nullptr;
    int value = -1;
    if (sqlite3_prepare_v2(db, sql.c_str(), -1, &st, nullptr) == SQLITE_OK) {
        if (sqlite3_step(st) == SQLITE_ROW) value = sqlite3_column_int(st, 0);
    }
    sqlite3_finalize(st);
    return value;
}

static void write_file(const string& path, const string& content) {
    // _wfopen, not fopen: fopen() would mangle a non-ASCII path.
    FILE* f = _wfopen(utf8_to_wide(path).c_str(), L"wb");
    if (f) {
        fwrite(content.data(), 1, content.size(), f);
        fclose(f);
    } else {
        printf("  !! cannot write %s\n", path.c_str());
    }
}

// ---------------------------------------------------------------------------
// fixture: a miniature opencode data directory
// ---------------------------------------------------------------------------
// Mirrors the shape of a real opencode database: most child tables carry a
// session_id column but declare NO foreign key, while a couple of others do
// declare ON DELETE CASCADE. Deleting a session has to clean up both kinds.
static const char* SCHEMA =
    "CREATE TABLE project (id TEXT PRIMARY KEY, worktree TEXT, name TEXT);"
    "CREATE TABLE session (id TEXT PRIMARY KEY,"
    "        project_id TEXT REFERENCES project(id), title TEXT,"
    "        time_created INTEGER, time_updated INTEGER);"
    // message/part mirror the real columns (JSON payload in `data`)
    "CREATE TABLE message (id TEXT PRIMARY KEY, session_id TEXT,"
    "        time_created INTEGER, time_updated INTEGER, data TEXT);"
    "CREATE TABLE part (id TEXT PRIMARY KEY, session_id TEXT, message_id TEXT,"
    "        time_created INTEGER, time_updated INTEGER, data TEXT);"
    // a grandchild that only knows about its message
    "CREATE TABLE attachment (id TEXT PRIMARY KEY,"
    "        message_id TEXT, url TEXT);"
    // and two tables that do declare the cascade
    "CREATE TABLE todo (id TEXT PRIMARY KEY,"
    "        session_id TEXT REFERENCES session(id) ON DELETE CASCADE, name TEXT);"
    "CREATE TABLE session_share (id TEXT PRIMARY KEY,"
    "        session_id TEXT REFERENCES session(id) ON DELETE CASCADE, url TEXT);"
    // the append-only event log: the real space hog, with full part snapshots
    "CREATE TABLE event (id TEXT PRIMARY KEY, aggregate_id TEXT, seq INTEGER,"
    "        type TEXT, data TEXT);"
    "CREATE UNIQUE INDEX event_aggregate_seq_idx ON event (aggregate_id, seq);"
    "CREATE TABLE event_sequence (aggregate_id TEXT PRIMARY KEY, seq INTEGER);";

struct Fixture {
    string root;         // scratch root (contains non-ASCII characters on purpose)
    string db;           // <root>/opencode.db
    string snapshot;     // <root>/storage/快照目录
    string diff;         // <root>/storage/session_diff
};

static Fixture build_fixture(const string& scratch) {
    Fixture fx;
    fx.root = path_join(scratch, "中文 数据 目录");
    fx.db = path_join(fx.root, "opencode.db");
    fx.snapshot = path_join(path_join(fx.root, "storage"), "快照目录");
    fx.diff = path_join(path_join(fx.root, "storage"), "session_diff");

    // start from a clean slate
    set_delete_to_recycle(false);          // keep the Recycle Bin out of the test
    if (path_exists(fx.root)) remove_tree(fx.root);

    make_dir(scratch);
    make_dir(fx.root);
    make_dir(path_join(fx.root, "storage"));
    make_dir(fx.snapshot);
    make_dir(fx.diff);

    sqlite3* db = nullptr;
    db_open(fx.db, &db);
    if (!db) return fx;
    db_exec(db, SCHEMA);

    // two projects; one of them ("ghost") is deleted from project but a session
    // still points at it -- load_sessions must still list that session.
    db_exec(db, "INSERT INTO project VALUES ('proj-live', 'E:/code/示例项目', '示例项目');");
    db_exec(db, "INSERT INTO project VALUES ('proj-gone', 'E:/code/gone', 'gone');");

    db_exec(db,
            "INSERT INTO session VALUES ('ses-aaa', 'proj-live', '第一个会话', 1700000000000, 1700000900000);");
    db_exec(db,
            "INSERT INTO session VALUES ('ses-bbb', 'proj-live', 'second', 1700001000000, 1700001900000);");
    db_exec(db,
            "INSERT INTO session VALUES ('ses-ccc', 'proj-gone', 'orphaned project', 1700002000000, 1700002900000);");

    db_exec(db, "INSERT INTO message VALUES ('msg-1','ses-aaa',1000,1100,'{\"role\":\"user\"}');");
    db_exec(db, "INSERT INTO message VALUES ('msg-2','ses-aaa',2000,2100,'{\"role\":\"assistant\"}');");
    db_exec(db, "INSERT INTO message VALUES ('msg-3','ses-bbb',3000,3100,'{\"role\":\"user\"}');");
    db_exec(db, "INSERT INTO message VALUES ('msg-4','ses-ccc',4000,4100,'{\"role\":\"user\"}');");
    db_exec(db,
            "INSERT INTO part VALUES ('part-1','ses-aaa','msg-1',1000,1100,"
            "'{\"type\":\"text\",\"text\":\"hello from part 1\"}');");
    db_exec(db,
            "INSERT INTO part VALUES ('part-2','ses-aaa','msg-2',2000,2100,"
            "'{\"type\":\"tool\",\"tool\":\"read\"}');");
    db_exec(db,
            "INSERT INTO part VALUES ('part-3','ses-bbb','msg-3',3000,3100,"
            "'{\"type\":\"text\",\"text\":\"keep me\"}');");
    db_exec(db,
            "INSERT INTO part VALUES ('part-4','ses-ccc','msg-4',4000,4100,"
            "'{\"type\":\"text\",\"text\":\"keep me too\"}');");

    // The append-only event log: full snapshots of a part on every update, plus
    // one aggregate (ses-ghost) whose session row no longer exists.
    {
        auto add_event = [&](const string& agg, long long seq, const string& type,
                             const string& payload) {
            string sql = "INSERT INTO event VALUES ('evt-" + agg + "-" + std::to_string(seq) +
                         "','" + agg + "'," + std::to_string(seq) + ",'" + type + "','" + payload +
                         "')";
            db_exec(db, sql);
        };
        auto part_payload = [](const string& sid, const string& pid, const string& text) {
            return "{\"sessionID\":\"" + sid + "\",\"part\":{\"id\":\"" + pid +
                   "\",\"sessionID\":\"" + sid + "\",\"type\":\"text\",\"text\":\"" + text + "\"}}";
        };
        // prt-1: 5 versions (4 redundant), prt-2: 2 versions (1 redundant)
        for (int v = 1; v <= 5; ++v)
            add_event("ses-aaa", v, "message.part.updated.1",
                      part_payload("ses-aaa", "prt-1", string((size_t)v * 10, 'x')));
        add_event("ses-aaa", 6, "message.part.updated.1", part_payload("ses-aaa", "prt-2", "one"));
        add_event("ses-aaa", 7, "message.part.updated.1", part_payload("ses-aaa", "prt-2", "two"));
        add_event("ses-aaa", 8, "message.updated.1",
                  "{\"sessionID\":\"ses-aaa\",\"message\":{\"id\":\"msg-1\"}}");
        // prt-3: 2 versions (1 redundant)
        add_event("ses-bbb", 1, "message.part.updated.1", part_payload("ses-bbb", "prt-3", "a"));
        add_event("ses-bbb", 2, "message.part.updated.1", part_payload("ses-bbb", "prt-3", "ab"));
        // three orphan rows belonging to a session that is gone
        add_event("ses-ghost", 1, "message.part.updated.1", part_payload("ses-ghost", "prt-9a", "g1"));
        add_event("ses-ghost", 2, "message.part.updated.1", part_payload("ses-ghost", "prt-9b", "g2"));
        add_event("ses-ghost", 3, "message.part.updated.1", part_payload("ses-ghost", "prt-9c", "g3"));
    }
    db_exec(db, "INSERT INTO attachment VALUES ('att-1', 'msg-1', 'a.png');");
    db_exec(db, "INSERT INTO attachment VALUES ('att-2', 'msg-3', 'b.png');");
    db_exec(db, "INSERT INTO todo VALUES ('todo-1', 'ses-aaa', 'first task');");
    db_exec(db, "INSERT INTO todo VALUES ('todo-2', 'ses-bbb', 'other task');");
    db_exec(db, "INSERT INTO session_share VALUES ('shr-1', 'ses-aaa', 'https://x/1');");

    // remove the project row of ses-ccc so the LEFT JOIN is exercised
    db_exec(db, "DELETE FROM project WHERE id = 'proj-gone';");
    sqlite3_close(db);

    // snapshots: one live project, one orphan, both on non-ASCII paths
    string snap_live = path_join(fx.snapshot, "proj-live");
    string snap_orph = path_join(fx.snapshot, "proj-孤立");
    make_dir(snap_live);
    make_dir(snap_orph);
    write_file(path_join(snap_live, "d1.json"), "0123456789");                 // 10 bytes
    write_file(path_join(snap_orph, "d2.json"), string(4096, 'x'));            // 4096 bytes
    make_dir(path_join(snap_orph, "嵌套"));
    write_file(path_join(path_join(snap_orph, "嵌套"), "d3.bin"), string(101, 'y'));  // 101 bytes

    // session_diff is filled in by test_orphan_diffs(), which needs a known
    // starting state (earlier tests remove sessions).
    return fx;
}

// ---------------------------------------------------------------------------
// tests
// ---------------------------------------------------------------------------
static void test_utf8_helpers() {
    SECTION("utf8 helpers");

    string zh = "会议记录：重构数据层";   // 3 bytes per CJK char
    string cut = utf8_ellipsize(zh, 4);
    CHECK(cut.size() < zh.size(), "ellipsize shortens a long CJK string");
    // must end with a complete ellipsis and contain no broken sequence
    CHECK(cut.compare(cut.size() - 3, 3, "\xE2\x80\xA6") == 0, "ellipsize ends with U+2026");
    bool valid = true;
    for (size_t i = 0; i + 1 < cut.size();) {
        unsigned char c = (unsigned char)cut[i];
        int len = c < 0x80 ? 1 : (c >> 5) == 0x6 ? 2 : (c >> 4) == 0xE ? 3 : (c >> 3) == 0x1E ? 4 : 0;
        if (len == 0) { valid = false; break; }
        for (int k = 1; k < len; ++k)
            if (i + (size_t)k >= cut.size() || ((unsigned char)cut[i + k] & 0xC0) != 0x80) valid = false;
        i += (size_t)len;
    }
    CHECK(valid, "ellipsize never splits a UTF-8 sequence");
    CHECK(utf8_ellipsize("short", 40) == "short", "ellipsize leaves short strings alone");
    CHECK(utf8_ellipsize("anything", 0).empty(), "ellipsize with width 0 is empty");

    CHECK(utf8_icontains("E:/Code/OpenCode", "opencode"), "icontains folds ASCII case");
    CHECK(utf8_icontains("第一个会话", "会话"), "icontains matches CJK substrings");
    CHECK(!utf8_icontains("第一个会话", "第2个"), "icontains rejects absent substrings");
    CHECK(utf8_icontains("x", ""), "empty needle matches");

    CHECK(path_join("C:/a", "b") == "C:/a/b", "path_join inserts a separator");
    CHECK(file_stem("C:/a/ses-aaa.json") == "ses-aaa", "file_stem strips dir+ext");
    CHECK(file_extension("C:/a/ses-aaa.json") == ".json", "file_extension");
}

static void test_load_sessions(const Fixture& fx) {
    SECTION("load_sessions");

    std::vector<Session> sessions = load_sessions(fx.db);
    CHECK(sessions.size() == 3, "all three sessions are listed");
    CHECK(sessions.size() == 3 && sessions[0].id == "ses-ccc",
          "sorted by creation time, newest first");

    const Session* aaa = nullptr;
    const Session* ccc = nullptr;
    for (auto& s : sessions) {
        if (s.id == "ses-aaa") aaa = &s;
        if (s.id == "ses-ccc") ccc = &s;
    }
    CHECK(aaa != nullptr, "ses-aaa present");
    if (aaa) {
        CHECK(aaa->msg_count == 2, "msg_count counts messages");
        CHECK(wide_to_utf8(aaa->title) == "第一个会话", "CJK title round-trips");
        CHECK(aaa->project_name == "示例项目", "project name is joined in");
        CHECK(aaa->worktree == "E:/code/示例项目", "CJK worktree survives");
    }
    // the LEFT JOIN: ses-ccc's project row was deleted on purpose
    CHECK(ccc != nullptr, "session with a dangling project_id is still listed");
    if (ccc) CHECK(ccc->worktree.empty(), "dangling project yields empty worktree, not a crash");
}

static void test_snapshots(const Fixture& fx) {
    SECTION("load_snapshots");

    std::vector<Snapshot> lazy = load_snapshots(fx.db, fx.snapshot, /*with_sizes=*/false);
    CHECK(lazy.size() == 2, "two snapshot directories found under a CJK path");
    bool all_unknown = true;
    for (auto& s : lazy) all_unknown = all_unknown && !s.size_known && s.size == 0;
    CHECK(all_unknown, "lazy mode leaves sizes unset");

    std::vector<Snapshot> full = load_snapshots(fx.db, fx.snapshot, /*with_sizes=*/true);
    CHECK(full.size() == 2, "eager mode finds the same two directories");
    long long live = -1, orphan = -1;
    bool live_active = false, orphan_active = true;
    for (auto& s : full) {
        if (s.id == "proj-live") { live = s.size; live_active = s.active_sessions; }
        if (s.id == "proj-孤立") { orphan = s.size; orphan_active = s.active_sessions; }
    }
    CHECK(live == 10, "size of a small snapshot dir (non-ASCII parent path)");
    CHECK(orphan == 4096 + 101, "size includes nested subdirectories");
    CHECK(live_active, "proj-live is marked as referenced by a session");
    CHECK(!orphan_active, "proj-孤立 is marked as an orphan");

    CHECK(total_dir_size(path_join(fx.snapshot, "proj-孤立")) == 4096 + 101,
          "total_dir_size on a nested CJK path");
    CHECK(total_dir_size("E:/definitely/not/here") == 0, "total_dir_size of a missing dir is 0");
}

static void test_delete_sessions(const Fixture& fx) {
    SECTION("delete_sessions");

    // deleting nothing must not throw and must not open a write transaction
    CHECK(delete_sessions(fx.db, std::vector<string>()) == 0, "empty id list returns 0");

    int deleted = 0;
    try {
        deleted = delete_sessions(fx.db, {"ses-aaa"});
    } catch (const std::exception& e) {
        printf("  !! unexpected: %s\n", e.what());
    }
    CHECK(deleted == 1, "one session deleted");

    sqlite3* db = nullptr;
    db_open(fx.db, &db);
    CHECK(db_scalar(db, "SELECT COUNT(*) FROM session WHERE id='ses-aaa'") == 0,
          "session row is gone");
    CHECK(db_scalar(db, "SELECT COUNT(*) FROM message WHERE session_id='ses-aaa'") == 0,
          "messages removed (session_id column, no declared FK)");
    CHECK(db_scalar(db, "SELECT COUNT(*) FROM part WHERE session_id='ses-aaa'") == 0,
          "parts removed");
    CHECK(db_scalar(db, "SELECT COUNT(*) FROM part WHERE message_id='msg-1'") == 0,
          "parts removed via message_id too");
    CHECK(db_scalar(db, "SELECT COUNT(*) FROM attachment WHERE message_id='msg-1'") == 0,
          "grandchild rows reachable only via message_id removed");
    CHECK(db_scalar(db, "SELECT COUNT(*) FROM todo WHERE session_id='ses-aaa'") == 0,
          "FK-declared child (todo) removed");
    CHECK(db_scalar(db, "SELECT COUNT(*) FROM session_share WHERE session_id='ses-aaa'") == 0,
          "FK-declared child (session_share) removed");
    CHECK(db_scalar(db, "SELECT COUNT(*) FROM part WHERE message_id='msg-3'") == 1,
          "other sessions' rows are untouched");
    CHECK(db_scalar(db, "SELECT COUNT(*) FROM attachment WHERE message_id='msg-3'") == 1,
          "other sessions' grandchildren are untouched");
    CHECK(db_scalar(db, "SELECT COUNT(*) FROM todo WHERE session_id='ses-bbb'") == 1,
          "other sessions' todos are untouched");
    CHECK(db_scalar(db, "SELECT COUNT(*) FROM session") == 2, "the other sessions survive");
    sqlite3_close(db);

    // deleting an unknown id is a no-op, not an error
    CHECK(delete_sessions(fx.db, {"does-not-exist"}) == 0, "unknown id deletes nothing");
}

static void test_locked_database(const Fixture& fx) {
    SECTION("locked database reports failure instead of pretending to work");

    sqlite3* holder = nullptr;
    db_open(fx.db, &holder);
    if (holder) {
        db_exec(holder, "BEGIN IMMEDIATE");   // hold the write lock
        bool threw = false;
        string what;
        try {
            delete_sessions(fx.db, {"ses-bbb"});
        } catch (const std::exception& e) {
            threw = true;
            what = e.what();
        }
        db_exec(holder, "ROLLBACK");
        sqlite3_close(holder);
        CHECK(threw, "delete on a locked db throws (takes ~5s for the busy timeout)");
        if (threw) printf("        message: %s\n", utf8_ellipsize(what, 70).c_str());

        // and the session is still there afterwards
        sqlite3* db = nullptr;
        db_open(fx.db, &db);
        CHECK(db_scalar(db, "SELECT COUNT(*) FROM session WHERE id='ses-bbb'") == 1,
              "the failed delete rolled back cleanly");
        sqlite3_close(db);
    } else {
        CHECK(false, "could not open the database for the lock test");
    }
}

static void test_orphan_diffs(const Fixture& fx) {
    SECTION("cleanup_orphan_diffs");

    // Self-contained: earlier tests delete ses-aaa, so build this set from
    // scratch against the sessions that are still in the database.
    write_file(path_join(fx.diff, "ses-bbb.json"), "{}");   // ses-bbb still exists
    write_file(path_join(fx.diff, "ses-zzz.json"), "{}");   // orphan
    write_file(path_join(fx.diff, "随机名字.json"), "{}");    // orphan, CJK filename
    write_file(path_join(fx.diff, "readme.txt"), "not a diff");
    CHECK(path_exists(path_join(fx.diff, "ses-bbb.json")), "live session diff exists");

    std::vector<string> failed;
    std::vector<string> removed = cleanup_orphan_diffs(fx.db, fx.diff, &failed);
    CHECK(removed.size() == 2, "two orphan diffs removed (incl. one with a CJK name)");
    CHECK(failed.empty(), "no removals failed");
    CHECK(path_exists(path_join(fx.diff, "ses-bbb.json")),
          "the live session's diff is kept");
    CHECK(!path_exists(path_join(fx.diff, "ses-zzz.json")), "the orphan diff is gone");
    CHECK(!path_exists(path_join(fx.diff, "随机名字.json")), "the CJK-named orphan is gone");
    CHECK(path_exists(path_join(fx.diff, "readme.txt")), "non-json files are left alone");
    CHECK(cleanup_orphan_diffs(fx.db, fx.diff).empty(), "second run finds nothing to do");
}

static void test_remove_tree(const Fixture& fx) {
    SECTION("remove_tree / delete_file");

    string victim = path_join(fx.root, "待删除");
    make_dir(victim);
    make_dir(path_join(victim, "子目录"));
    write_file(path_join(victim, "a.txt"), "a");
    write_file(path_join(path_join(victim, "子目录"), "b.txt"), "b");
    CHECK(is_dir(victim), "victim tree created");

    remove_tree(victim);
    CHECK(!path_exists(victim), "remove_tree deleted a non-ASCII tree");

    // read-only files used to make DeleteFile fail silently
    string ro = path_join(fx.root, "只读.txt");
    write_file(ro, "x");
    SetFileAttributesW(utf8_to_wide(ro).c_str(), FILE_ATTRIBUTE_READONLY);
    bool ok = true;
    try {
        delete_file(ro);
    } catch (const std::exception& e) {
        ok = false;
        printf("  !! %s\n", e.what());
    }
    CHECK(ok && !path_exists(ro), "delete_file clears the read-only attribute first");

    // deleting a missing path is a harmless no-op
    ok = true;
    try {
        remove_tree(path_join(fx.root, "not-there"));
        delete_file(path_join(fx.root, "not-there.txt"));
    } catch (const std::exception&) {
        ok = false;
    }
    CHECK(ok, "removing a missing path does not throw");

    CHECK(!is_dir(fx.db), "is_dir is false for a file");
    CHECK(is_file(fx.db), "is_file is true for a file");

    string fresh = path_join(fx.root, "新建目录");
    CHECK(!path_exists(fresh), "target directory does not exist yet");
    CHECK(make_dir(fresh), "make_dir creates it");
    CHECK(make_dir(fresh), "make_dir on an existing directory still succeeds");
    CHECK(is_dir(fresh), "the new directory is really a directory");
}

// ---------------------------------------------------------------------------
// The shared view model (filter + ordering) used by both front ends.
// ---------------------------------------------------------------------------
static std::vector<Session> sample_sessions() {
    std::vector<Session> v(3);
    v[0].id = "ses-a"; v[0].title = L"zebra 会话"; v[0].project_name = "proj1";
    v[0].worktree = "E:/w1"; v[0].time_created = 300; v[0].time_updated = 30; v[0].msg_count = 5;
    v[1].id = "ses-b"; v[1].title = L"alpha"; v[1].project_name = "proj2";
    v[1].worktree = "E:/w2"; v[1].time_created = 100; v[1].time_updated = 90; v[1].msg_count = 50;
    v[2].id = "ses-c"; v[2].title = L"middle 中文"; v[2].project_name = "proj1";
    v[2].worktree = "E:/w3"; v[2].time_created = 200; v[2].time_updated = 10; v[2].msg_count = 5;
    return v;
}

static std::vector<Snapshot> sample_snapshots() {
    std::vector<Snapshot> v(3);
    v[0].id = "one";   v[0].path = "x/one";   v[0].size = 100; v[0].active_sessions = true;
    v[0].name = L"活跃项目";
    v[1].id = "two";   v[1].path = "x/two";   v[1].size = 300; v[1].active_sessions = false;
    v[1].name = L"孤立项目";
    v[2].id = "three"; v[2].path = "x/three"; v[2].size = 200; v[2].active_sessions = false;
    v[2].name = L"third";
    return v;
}

// "ses-a,ses-c,ses-b" style rendering of a view over sessions.
static string session_ids(const std::vector<Session>& all, const std::vector<int>& view) {
    string out;
    for (size_t i = 0; i < view.size(); ++i) {
        if (i) out += ",";
        out += all[(size_t)view[i]].id;
    }
    return out;
}

static string snapshot_ids(const std::vector<Snapshot>& all, const std::vector<int>& view) {
    string out;
    for (size_t i = 0; i < view.size(); ++i) {
        if (i) out += ",";
        out += all[(size_t)view[i]].id;
    }
    return out;
}

static std::vector<int> session_view(const std::vector<Session>& s, const string& filter,
                                     SortField field, bool desc) {
    ViewQuery q;
    q.filter = filter;
    q.field = field;
    q.descending = desc;
    return build_session_view(s, q);
}

static std::vector<int> snapshot_view(const std::vector<Snapshot>& s, const string& filter,
                                      bool orphans_only, SortField field, bool desc) {
    ViewQuery q;
    q.filter = filter;
    q.orphans_only = orphans_only;
    q.field = field;
    q.descending = desc;
    return build_snapshot_view(s, q);
}

static void test_view_model() {
    SECTION("view model: filtering");
    std::vector<Session> ses = sample_sessions();

    CHECK(session_view(ses, "", SORT_CREATED, true).size() == 3, "empty filter keeps every row");
    CHECK(session_ids(ses, session_view(ses, "proj1", SORT_CREATED, true)) == "ses-a,ses-c",
          "filter matches the project name");
    CHECK(session_ids(ses, session_view(ses, "E:/w2", SORT_CREATED, true)) == "ses-b",
          "filter matches the worktree");
    CHECK(session_ids(ses, session_view(ses, "SES-B", SORT_CREATED, true)) == "ses-b",
          "filter folds ASCII case");
    CHECK(session_ids(ses, session_view(ses, "会话", SORT_CREATED, true)) == "ses-a",
          "filter matches a CJK title");
    CHECK(session_ids(ses, session_view(ses, "中文", SORT_CREATED, true)) == "ses-c",
          "filter matches a CJK substring in the middle of a title");
    CHECK(session_view(ses, "no-such-thing", SORT_CREATED, true).empty(),
          "a filter with no match yields an empty view");

    SECTION("view model: session ordering");
    CHECK(session_ids(ses, session_view(ses, "", SORT_CREATED, true)) == "ses-a,ses-c,ses-b",
          "created desc");
    CHECK(session_ids(ses, session_view(ses, "", SORT_CREATED, false)) == "ses-b,ses-c,ses-a",
          "created asc");
    CHECK(session_ids(ses, session_view(ses, "", SORT_UPDATED, true)) == "ses-b,ses-a,ses-c",
          "updated desc");
    // a and c both have 5 messages. The id tie-break follows the sort direction
    // too, so flipping the order fully reverses the list.
    CHECK(session_ids(ses, session_view(ses, "", SORT_MESSAGES, true)) == "ses-b,ses-c,ses-a",
          "messages desc, ties broken by id");
    CHECK(session_ids(ses, session_view(ses, "", SORT_MESSAGES, false)) == "ses-a,ses-c,ses-b",
          "messages asc, ties reversed");
    CHECK(session_ids(ses, session_view(ses, "", SORT_NAME, false)) == "ses-b,ses-c,ses-a",
          "title asc");
    CHECK(session_ids(ses, session_view(ses, "", SORT_LOCATION, false)) == "ses-a,ses-c,ses-b",
          "project asc");
    CHECK(session_ids(ses, session_view(ses, "", SORT_ID, false)) == "ses-a,ses-b,ses-c",
          "id asc");

    SECTION("view model: snapshots");
    std::vector<Snapshot> snaps = sample_snapshots();
    CHECK(snapshot_ids(snaps, snapshot_view(snaps, "", false, SORT_SIZE, true)) == "two,three,one",
          "size desc (largest first)");
    CHECK(snapshot_ids(snaps, snapshot_view(snaps, "", false, SORT_SIZE, false)) == "one,three,two",
          "size asc");
    CHECK(snapshot_ids(snaps, snapshot_view(snaps, "", true, SORT_SIZE, true)) == "two,three",
          "orphans_only hides the in-use directory");
    CHECK(snapshot_ids(snaps, snapshot_view(snaps, "", false, SORT_ID, false)) == "one,three,two",
          "path asc");
    CHECK(snapshot_ids(snaps, snapshot_view(snaps, "", false, SORT_NAME, false)) == "three,two,one",
          "project name asc (CJK compares by codepoint)");
    // status asc => orphans (0) before in-use (1); the two orphans tie and fall
    // back to the path tie-break (x/three < x/two)
    CHECK(snapshot_ids(snaps, snapshot_view(snaps, "", false, SORT_STATUS, false)) == "three,two,one",
          "status asc puts orphans first");
    CHECK(snapshot_ids(snaps, snapshot_view(snaps, "孤立", false, SORT_SIZE, true)) == "two",
          "filter matches a CJK snapshot name");
    CHECK(snapshot_ids(snaps, snapshot_view(snaps, "x/three", true, SORT_SIZE, true)) == "three",
          "filter and orphans_only combine");

    SECTION("view model: structural invariants");
    // the view must be a permutation of exactly the matching rows
    std::vector<int> v = session_view(ses, "proj1", SORT_NAME, true);
    std::set<int> seen(v.begin(), v.end());
    CHECK(seen.size() == v.size(), "no duplicate rows in the view");
    bool in_range = true;
    for (int i : v)
        if (i < 0 || i >= (int)ses.size()) in_range = false;
    CHECK(in_range, "every view entry is a valid index");

    // filtering never mutates the input
    std::vector<Session> copy = sample_sessions();
    (void)session_view(ses, "proj1", SORT_NAME, true);
    bool unchanged = copy.size() == ses.size();
    for (size_t i = 0; unchanged && i < copy.size(); ++i)
        if (copy[i].id != ses[i].id) unchanged = false;
    CHECK(unchanged, "building a view leaves the source vector untouched");

    CHECK(string(sort_field_name(SORT_SIZE)) == "size", "sort_field_name(SORT_SIZE)");
    CHECK(string(sort_field_name(SORT_MESSAGES)) == "msgs", "sort_field_name(SORT_MESSAGES)");
    CHECK(string(sort_field_name(SORT_FIELD_COUNT)) == "?", "unknown sort field is labelled");
}

// ---------------------------------------------------------------------------
// Database maintenance: space analysis, orphan events, compaction, VACUUM,
// preview and export. Uses its own fixture so that the destructive steps cannot
// interfere with the other tests.
// ---------------------------------------------------------------------------
static void test_db_maintenance(const string& scratch) {
    SECTION("db maintenance: report");
    Fixture fx = build_fixture(scratch + "-maint");

    DbReport r = analyze_database(fx.db, 5, nullptr);
    CHECK(r.page_size > 0 && r.page_count > 0, "report: page size/count");
    CHECK(r.file_bytes == r.page_size * r.page_count, "report: file size = pages * page size");
    CHECK(r.session_rows == 3, "report: session rows");
    CHECK(r.event_rows == 13, "report: event rows");
    CHECK(r.event_bytes > 0, "report: event text bytes");
    CHECK(!r.event_types.empty() && r.event_types[0].name == "message.part.updated.1",
          "report: biggest event type is part.updated");
    CHECK(r.orphan_event_rows == 3, "report: orphan event rows (deleted session)");
    CHECK(r.orphan_event_bytes > 0, "report: orphan event bytes");
    CHECK(r.reclaimable_safe() >= r.orphan_event_bytes,
          "report: safe reclaimable includes the free pages");
    bool has_event_table = false;
    for (auto& t : r.tables)
        if (t.name == "event") has_event_table = true;
    CHECK(has_event_table, "report: the table list contains event");
    CHECK(!r.sessions.empty() && r.sessions[0].bytes >= r.sessions.back().bytes,
          "report: sessions are ordered by event footprint");

    SECTION("db maintenance: compaction plan is read-only");
    long long size_before = file_size(fx.db);
    CompactionPlan plan = plan_event_compaction(fx.db, nullptr);
    CHECK(plan.total_rows == 12, "plan: counts only part.updated rows");
    CHECK(plan.parts == 6, "plan: six distinct parts");
    CHECK(plan.removable_rows == 6, "plan: six redundant versions");
    CHECK(plan.removable_bytes > 0, "plan: removable bytes estimated");
    CHECK(file_size(fx.db) == size_before, "plan: a dry run does not touch the database");
    CHECK(!plan.worst_parts.empty() && plan.worst_parts[0].name == "prt-1",
          "plan: worst part is the one with five versions");

    SECTION("db maintenance: orphan events");
    ReclaimResult c = delete_orphan_events(fx.db, nullptr);
    CHECK(c.rows == 3, "orphans: three rows removed");
    sqlite3* db = nullptr;
    db_open(fx.db, &db);
    CHECK(db_scalar(db, "SELECT COUNT(*) FROM event WHERE aggregate_id='ses-ghost'") == 0,
          "orphans: the ghost aggregate is gone");
    CHECK(db_scalar(db, "SELECT COUNT(*) FROM event WHERE aggregate_id='ses-aaa'") == 8,
          "orphans: live events untouched");
    sqlite3_close(db);

    SECTION("db maintenance: apply compaction");
    ReclaimResult a = apply_event_compaction(fx.db, plan, nullptr);
    CHECK(a.rows == 6, "compact: six rows removed");
    db_open(fx.db, &db);
    CHECK(db_scalar(db, "SELECT COUNT(*) FROM event") == 4,
          "compact: four rows left (one per part + the message event)");
    CHECK(db_scalar(db, "SELECT COUNT(*) FROM event WHERE data LIKE '%prt-1%'") == 1,
          "compact: only the newest version of prt-1 survives");
    CHECK(db_scalar(db, "SELECT seq FROM event WHERE data LIKE '%prt-1%'") == 5,
          "compact: and it is the one with the highest seq");
    CHECK(db_scalar(db, "SELECT COUNT(*) FROM event WHERE type='message.updated.1'") == 1,
          "compact: other event types are left alone");
    sqlite3_close(db);

    SECTION("db maintenance: vacuum and preview");
    ReclaimResult v;
    bool vacuum_ok = vacuum_database(fx.db, &v, nullptr);
    CHECK(vacuum_ok, "vacuum: succeeds on a healthy database");
    CHECK(v.file_after <= v.file_before, "vacuum: the file does not grow");
    CHECK(!database_is_busy(fx.db), "vacuum: no other writer reports not-busy");

    std::vector<PreviewMessage> msgs = load_session_preview(fx.db, "ses-aaa", 10);
    CHECK(msgs.size() == 2, "preview: two parts for ses-aaa");
    bool saw_text = false, saw_role = false;
    for (auto& m : msgs) {
        if (m.text.find("hello from part 1") != string::npos) saw_text = true;
        if (m.role == "user" || m.role == "assistant") saw_role = true;
    }
    CHECK(saw_text, "preview: extracts the text payload");
    CHECK(saw_role, "preview: joins the message role");
    CHECK(load_session_preview(fx.db, "no-such-session", 5).empty(),
          "preview: unknown session yields nothing");

    SECTION("db maintenance: export");
    std::vector<string> written;
    string err;
    string out_dir = path_join(fx.root, "导出");
    CHECK(export_sessions_json(fx.db, {"ses-aaa", "ses-bbb"}, out_dir, &written, &err),
          "export: writes both sessions");
    CHECK(written.size() == 2, "export: two files reported");
    CHECK(!written.empty() && file_size(written[0]) > 0, "export: file is not empty");
    bool all_written = true;
    for (auto& w : written)
        if (!path_exists(w)) all_written = false;
    CHECK(all_written, "export: every reported file exists");
    CHECK(export_sessions_json(fx.db, {"ses-aaa"}, "", nullptr, &err) == false,
          "export: an empty destination directory is refused");
}

// ---------------------------------------------------------------------------
// opencode v2 layout. The migration moved sessions into `session_v2` and the
// message store into `session_message`, retired the append-only `event` log, and
// left the v1 tables (session/message/part) behind, frozen. Reading `session`
// here would hide everything created after the upgrade -- and would make
// cleanup_orphan_diffs treat those sessions' diff files as orphans.
// ---------------------------------------------------------------------------
static const char* SCHEMA_V2 =
    "CREATE TABLE project (id TEXT PRIMARY KEY, worktree TEXT, name TEXT);"
    // frozen v1 leftovers
    "CREATE TABLE session (id TEXT PRIMARY KEY, project_id TEXT, title TEXT,"
    "        time_created INTEGER, time_updated INTEGER);"
    "CREATE TABLE message (id TEXT PRIMARY KEY, session_id TEXT, data TEXT);"
    "CREATE TABLE part (id TEXT PRIMARY KEY, session_id TEXT, message_id TEXT, data TEXT);"
    // the v2 tables: no foreign keys declared, only session_id columns
    "CREATE TABLE session_v2 (id TEXT PRIMARY KEY, project_id TEXT, title TEXT,"
    "        time_created INTEGER, time_updated INTEGER, version TEXT);"
    "CREATE TABLE session_message (id TEXT PRIMARY KEY, session_id TEXT, type TEXT,"
    "        seq INTEGER, time_created INTEGER, time_updated INTEGER, data TEXT);"
    "CREATE TABLE instruction_state (session_id TEXT, epoch_start INTEGER,"
    "        through_seq INTEGER, current_values TEXT);"
    "CREATE TABLE session_inbox (id TEXT PRIMARY KEY, session_id TEXT, type TEXT);"
    "CREATE TABLE session_pending (id TEXT PRIMARY KEY, session_id TEXT, type TEXT);"
    // retired, but the table (and its free pages) are still there
    "CREATE TABLE event (id TEXT PRIMARY KEY, aggregate_id TEXT, seq INTEGER,"
    "        type TEXT, data TEXT);";

static Fixture build_v2_fixture(const string& scratch) {
    Fixture fx;
    fx.root = path_join(scratch, "v2 数据目录");
    fx.db = path_join(fx.root, "opencode.db");
    fx.snapshot = path_join(path_join(fx.root, "storage"), "快照目录");
    fx.diff = path_join(path_join(fx.root, "storage"), "session_diff");

    set_delete_to_recycle(false);
    if (path_exists(fx.root)) remove_tree(fx.root);
    make_dir(scratch);
    make_dir(fx.root);
    make_dir(path_join(fx.root, "storage"));
    make_dir(fx.snapshot);
    make_dir(fx.diff);

    sqlite3* db = nullptr;
    db_open(fx.db, &db);
    if (!db) return fx;
    db_exec(db, SCHEMA_V2);

    db_exec(db, "INSERT INTO project VALUES ('p1','E:/code/新项目','新项目');");
    // a v1-era session the migration copied across, and a v2-only one
    db_exec(db, "INSERT INTO session VALUES ('ses-old','p1','旧会话',1000,2000);");
    db_exec(db, "INSERT INTO session_v2 VALUES ('ses-old','p1','旧会话',1000,2000,'1.18.34');");
    db_exec(db, "INSERT INTO session_v2 VALUES ('ses-new','p1','新会话',3000,4000,'2.0.25');");

    auto add_msg = [&](const string& id, const string& sid, const string& type, long long seq,
                       const string& text) {
        db_exec(db, "INSERT INTO session_message VALUES ('" + id + "','" + sid + "','" + type +
                        "'," + std::to_string(seq) + ",1000,1100,"
                        "'{\"content\":[{\"type\":\"reasoning\",\"text\":\"think\"},"
                        "{\"type\":\"text\",\"text\":\"" + text + "\"}],\"finish\":\"stop\"}');");
    };
    add_msg("m1", "ses-old", "user", 1, "hello v2");
    add_msg("m2", "ses-new", "assistant", 1, "第二条");
    add_msg("m3", "ses-new", "user", 2, "第二条 again");

    db_exec(db, "INSERT INTO instruction_state VALUES ('ses-new',1,2,'{}');");
    db_exec(db, "INSERT INTO session_inbox VALUES ('ib1','ses-new','user');");
    db_exec(db, "INSERT INTO session_pending VALUES ('pd1','ses-new','user');");
    sqlite3_close(db);

    write_file(path_join(fx.diff, "ses-new.json"), "{}");
    write_file(path_join(fx.diff, "ses-dead.json"), "{}");
    return fx;
}

static void test_v2_schema(const string& scratch) {
    SECTION("opencode v2: table resolution");
    Fixture fx = build_v2_fixture(scratch);
    {
        sqlite3* db = nullptr;
        db_open(fx.db, &db);
        CHECK(session_table(db) == "session_v2", "session_table picks session_v2");
        CHECK(message_table(db) == "session_message", "message_table picks session_message");
        sqlite3_close(db);
    }
    {
        // a v1 database (no session_v2) must still resolve to the old names
        string v1_path = path_join(scratch, "v1-only.db");
        if (path_exists(v1_path)) delete_file(v1_path);
        sqlite3* db = nullptr;
        db_open(v1_path, &db);
        db_exec(db, "CREATE TABLE session (id TEXT PRIMARY KEY);"
                    "CREATE TABLE message (id TEXT PRIMARY KEY);");
        CHECK(session_table(db) == "session", "v1 database falls back to session");
        CHECK(message_table(db) == "message", "v1 database falls back to message");
        sqlite3_close(db);
    }

    SECTION("opencode v2: sessions come from session_v2");
    {
        auto sessions = load_sessions(fx.db);
        bool has_new = false, has_old = false;
        long long new_msgs = -1;
        for (auto& s : sessions) {
            if (s.id == "ses-new") { has_new = true; new_msgs = s.msg_count; }
            if (s.id == "ses-old") has_old = true;
        }
        CHECK(sessions.size() == 2, "both v2 sessions are listed");
        CHECK(has_new, "a session that exists only in session_v2 is listed");
        CHECK(has_old, "the migrated session is listed");
        CHECK(new_msgs == 2, "msg_count is read from session_message");
    }

    SECTION("opencode v2: preview reads session_message");
    {
        auto msgs = load_session_preview(fx.db, "ses-new", 10);
        CHECK(msgs.size() == 2, "both messages previewed");
        if (msgs.size() == 2) {
            CHECK(msgs[0].role == "assistant", "role comes from the message type");
            CHECK(msgs[1].role == "user", "the second message keeps its role");
            CHECK(msgs[0].text.find("第二条") != string::npos,
                  "text is pulled out of the content array");
            CHECK(msgs[0].text.find("think") == string::npos, "reasoning text is skipped");
        }
    }

    SECTION("opencode v2: orphan diff cleanup must not eat live sessions");
    {
        std::vector<string> failed;
        auto removed = cleanup_orphan_diffs(fx.db, fx.diff, &failed);
        CHECK(path_exists(path_join(fx.diff, "ses-new.json")),
              "the diff of a v2-only session survives");
        CHECK(!path_exists(path_join(fx.diff, "ses-dead.json")), "the real orphan is removed");
        CHECK(removed.size() == 1, "exactly one orphan removed");
    }

    SECTION("opencode v2: deleting cascades through the v2 tables");
    {
        int n = delete_sessions(fx.db, {"ses-new"});
        CHECK(n == 1, "one session row deleted");
        sqlite3* db = nullptr;
        db_open(fx.db, &db);
        CHECK(db_scalar(db, "SELECT COUNT(*) FROM session_v2 WHERE id='ses-new'") == 0,
              "the session_v2 row is gone");
        CHECK(db_scalar(db, "SELECT COUNT(*) FROM session_message WHERE session_id='ses-new'") == 0,
              "session_message rows are gone");
        CHECK(db_scalar(db, "SELECT COUNT(*) FROM instruction_state WHERE session_id='ses-new'") == 0,
              "instruction_state rows are gone");
        CHECK(db_scalar(db, "SELECT COUNT(*) FROM session_inbox WHERE session_id='ses-new'") == 0,
              "session_inbox rows are gone");
        CHECK(db_scalar(db, "SELECT COUNT(*) FROM session_pending WHERE session_id='ses-new'") == 0,
              "session_pending rows are gone");
        CHECK(db_scalar(db, "SELECT COUNT(*) FROM session_message WHERE session_id='ses-old'") == 1,
              "the other session's messages survive");
        sqlite3_close(db);
    }

    SECTION("opencode v2: a retired event log is reported as such");
    {
        DbReport r = analyze_database(fx.db, 8);
        CHECK(r.v2_schema, "the v2 layout is detected");
        CHECK(r.event_rows == 0, "the retired event log reports zero rows");
        CHECK(r.session_rows == 1, "the surviving session is counted from session_v2");
        CHECK(r.message_rows == 1, "messages are counted from session_message");
    }
}

static void test_launch_guard() {    SECTION("launch guard");

    string err;
    CHECK(!launch_opencode_session("bad\"; calc.exe", "", &err),
          "session id with a quote is refused");
    CHECK(!err.empty(), "and it explains why");
    CHECK(launch_opencode_session("", "", &err) == false, "empty id is refused");
}

// ---------------------------------------------------------------------------
// Manual verification helper:
//   test_data.exe --delete <db> <session-id> [<session-id> ...]
// Deletes real sessions so the cascade can be inspected against a real
// database (or a copy of one).
// ---------------------------------------------------------------------------
static int run_delete_mode(int argc, char** argv) {
    std::vector<string> ids;
    for (int i = 3; i < argc; ++i) ids.push_back(argv[i]);
    try {
        int n = delete_sessions(argv[2], ids);
        printf("delete_sessions(%s, %d id(s)) -> %d deleted\n", argv[2], (int)ids.size(), n);
        return 0;
    } catch (const std::exception& e) {
        printf("delete_sessions failed: %s\n", e.what());
        return 1;
    }
}

// ---------------------------------------------------------------------------
int main(int argc, char** argv) {
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
#endif
    if (argc > 3 && strcmp(argv[1], "--delete") == 0) return run_delete_mode(argc, argv);

    string scratch = (argc > 1) ? argv[1] : "test-scratch";

    printf("OpenCode Session Manager data layer tests\n");
    printf("scratch dir: %s\n", scratch.c_str());

    test_utf8_helpers();
    test_view_model();

    Fixture fx = build_fixture(scratch);
    printf("fixture: %s\n", fx.root.c_str());

    test_load_sessions(fx);
    test_snapshots(fx);
    test_delete_sessions(fx);
    test_orphan_diffs(fx);
    test_locked_database(fx);
    test_remove_tree(fx);
    test_db_maintenance(scratch);
    test_v2_schema(scratch);
    test_launch_guard();

    printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
