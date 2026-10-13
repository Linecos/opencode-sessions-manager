// OpenCode Session Manager -- shared data layer implementation
#include "opencode_data.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN 1
#include <windows.h>
#include <shellapi.h>
#endif

// ===========================================================================
// UTF-8 <-> wide helpers (Windows)
// ===========================================================================
wstring utf8_to_wide(const string& s) {
    if (s.empty()) return L"";
#ifdef _WIN32
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    if (n <= 0) return L"";
    wstring out((size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &out[0], n);
    return out;
#else
    wstring out;
    for (char c : s) out += (wchar_t)(unsigned char)c;
    return out;
#endif
}

string wide_to_utf8(const wstring& w) {
    if (w.empty()) return "";
#ifdef _WIN32
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    if (n <= 0) return "";
    string out((size_t)n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &out[0], n, nullptr, nullptr);
    return out;
#else
    string out;
    for (wchar_t c : w) out += (char)c;
    return out;
#endif
}

// ===========================================================================
// Path helpers
// ===========================================================================
namespace {

#ifdef _WIN32

// UTF-8 -> a path string the Win32 W APIs accept (backslash separators).
// Paths at or beyond MAX_PATH get the \\?\ prefix so long snapshot/worktree
// paths keep working.
wstring win_path(const string& p) {
    wstring w = utf8_to_wide(p);
    for (size_t i = 0; i < w.size(); ++i)
        if (w[i] == L'/') w[i] = L'\\';
    if (w.size() >= MAX_PATH && w.compare(0, 4, L"\\\\?\\") != 0) {
        if (w.size() >= 2 && w[1] == L':')
            w = L"\\\\?\\" + w;
        else if (w.compare(0, 2, L"\\\\") == 0)
            w = L"\\\\?\\UNC\\" + w.substr(2);
    }
    return w;
}

// SHFileOperationW does not accept the \\?\ prefix, so keep a plain variant.
wstring win_path_plain(const string& p) {
    wstring w = utf8_to_wide(p);
    for (size_t i = 0; i < w.size(); ++i)
        if (w[i] == L'/') w[i] = L'\\';
    return w;
}

// Human readable Win32 error, e.g. "[WinError 5] Access is denied :'C:\x'".
string win_error_text(DWORD code, const string& path) {
    wchar_t* buf = nullptr;
    DWORD n = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                                 FORMAT_MESSAGE_IGNORE_INSERTS,
                             nullptr, code, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
                             (LPWSTR)&buf, 0, nullptr);
    string msg;
    if (n && buf) {
        wstring w(buf, n);
        while (!w.empty() && (w.back() == L'\n' || w.back() == L'\r' || w.back() == L' '))
            w.pop_back();
        msg = wide_to_utf8(w);
    }
    if (buf) LocalFree(buf);
    if (msg.empty()) msg = "unknown error";
    return "[WinError " + std::to_string((unsigned long)code) + "] " + msg + " :'" + path + "'";
}

#endif  // _WIN32

}  // namespace

string path_join(const string& base, const string& name) {
    if (base.empty()) return name;
    char c = base.back();
    if (c == '/' || c == '\\') return base + name;
    return base + "/" + name;
}

bool path_exists(const string& p) {
#ifdef _WIN32
    if (p.empty()) return false;
    return GetFileAttributesW(win_path(p).c_str()) != INVALID_FILE_ATTRIBUTES;
#else
    (void)p;
    return false;
#endif
}

bool is_dir(const string& p) {
#ifdef _WIN32
    if (p.empty()) return false;
    DWORD a = GetFileAttributesW(win_path(p).c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
#else
    (void)p;
    return false;
#endif
}

bool is_file(const string& p) {
#ifdef _WIN32
    if (p.empty()) return false;
    DWORD a = GetFileAttributesW(win_path(p).c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
#else
    (void)p;
    return false;
#endif
}

bool make_dir(const string& p) {
#ifdef _WIN32
    if (p.empty()) return false;
    if (is_dir(p)) return true;
    CreateDirectoryW(win_path(p).c_str(), nullptr);
    return is_dir(p);
#else
    (void)p;
    return false;
#endif
}

std::vector<string> list_children(const string& p) {
    std::vector<string> out;
#ifdef _WIN32
    if (p.empty()) return out;
    wstring pat = win_path(p);
    if (!pat.empty() && pat.back() != L'\\') pat += L'\\';
    pat += L'*';

    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pat.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return out;
    do {
        if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) continue;
        out.push_back(wide_to_utf8(fd.cFileName));
    } while (FindNextFileW(h, &fd));
    FindClose(h);
#else
    (void)p;
#endif
    return out;
}

std::vector<string> list_child_dirs(const string& p) {
    std::vector<string> out;
    for (auto& n : list_children(p)) {
        string full = path_join(p, n);
        if (is_dir(full)) out.push_back(full);
    }
    return out;
}

std::vector<string> list_child_files(const string& p) {
    std::vector<string> out;
    for (auto& n : list_children(p)) {
        string full = path_join(p, n);
        if (!is_dir(full)) out.push_back(full);
    }
    return out;
}

string file_extension(const string& path) {
    size_t slash = path.find_last_of("/\\");
    size_t dot = path.find_last_of('.');
    if (dot == string::npos) return "";
    if (slash != string::npos && dot < slash) return "";
    return path.substr(dot);
}

string file_stem(const string& path) {
    size_t slash = path.find_last_of("/\\");
    size_t start = (slash == string::npos) ? 0 : slash + 1;
    size_t dot = path.find_last_of('.');
    if (dot == string::npos || dot < start) return path.substr(start);
    return path.substr(start, dot - start);
}

namespace {

#ifdef _WIN32
// Recursive size walk that never follows reparse points (junctions/symlinks),
// which would otherwise let a snapshot directory point back at itself.
long long dir_size_rec(const wstring& dir, int depth) {
    if (depth > 64) return 0;
    wstring pat = dir;
    if (!pat.empty() && pat.back() != L'\\') pat += L'\\';
    pat += L'*';

    long long total = 0;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pat.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;
    do {
        if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) continue;
        wstring child = dir;
        if (!child.empty() && child.back() != L'\\') child += L'\\';
        child += fd.cFileName;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            total += dir_size_rec(child, depth + 1);
        } else {
            total += ((long long)fd.nFileSizeHigh << 32) | (long long)fd.nFileSizeLow;
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return total;
}
#endif

}  // namespace

long long total_dir_size(const string& p) {
#ifdef _WIN32
    if (!is_dir(p)) return 0;
    return dir_size_rec(win_path(p), 0);
#else
    (void)p;
    return 0;
#endif
}

long long file_size(const string& p) {
#ifdef _WIN32
    WIN32_FILE_ATTRIBUTE_DATA fad;
    if (!GetFileAttributesExW(win_path(p).c_str(), GetFileExInfoStandard, &fad)) return 0;
    if (fad.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) return 0;
    return ((long long)fad.nFileSizeHigh << 32) | (long long)fad.nFileSizeLow;
#else
    (void)p;
    return 0;
#endif
}

// ===========================================================================
// Removal: Recycle Bin by default, permanent only when asked for
// ===========================================================================
namespace {

bool g_recycle = true;
bool g_last_recycle = false;

#ifdef _WIN32

bool recycle_one(const string& path) {
    wstring w = win_path_plain(path);
    if (w.empty()) return false;

    // SHFileOperationW wants a double-NUL terminated list of absolute paths.
    std::vector<wchar_t> list;
    list.insert(list.end(), w.begin(), w.end());
    list.push_back(L'\0');
    list.push_back(L'\0');

    SHFILEOPSTRUCTW op;
    ZeroMemory(&op, sizeof(op));
    op.wFunc = FO_DELETE;
    op.pFrom = &list[0];
    op.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_NOERRORUI | FOF_SILENT;

    int rc = SHFileOperationW(&op);
    return rc == 0 && op.fAnyOperationsAborted == FALSE;
}

void delete_file_permanent(const string& path /* UTF-8 */) {
    wstring w = win_path(path);
    // Read-only files cannot be deleted until the attribute is cleared.
    SetFileAttributesW(w.c_str(), FILE_ATTRIBUTE_NORMAL);
    if (!DeleteFileW(w.c_str())) {
        if (!path_exists(path)) return;   // already gone: fine, idempotent
        throw std::runtime_error(win_error_text(GetLastError(), path));
    }
}

// depth guard against pathological trees; reparse points are removed with
// RemoveDirectoryW but never descended into.
void delete_tree_permanent(const string& dir, int depth) {
    if (depth > 64)
        throw std::runtime_error("directory nesting too deep, refusing to delete: '" + dir + "'");

    for (auto& name : list_children(dir)) {
        string child = path_join(dir, name);
        if (is_dir(child)) {
            delete_tree_permanent(child, depth + 1);
        } else {
            delete_file_permanent(child);
        }
    }

    wstring w = win_path(dir);
    SetFileAttributesW(w.c_str(), FILE_ATTRIBUTE_NORMAL);
    if (!RemoveDirectoryW(w.c_str())) {
        if (!path_exists(dir)) return;
        throw std::runtime_error(win_error_text(GetLastError(), dir));
    }
    if (path_exists(dir))
        throw std::runtime_error("path still exists after delete: '" + dir + "'");
}

#endif  // _WIN32

}  // namespace

void set_delete_to_recycle(bool on) { g_recycle = on; }
bool delete_to_recycle() { return g_recycle; }
bool last_delete_went_to_recycle() { return g_last_recycle; }

void delete_file(const string& p) {
#ifdef _WIN32
    if (!path_exists(p)) return;
    if (g_recycle && recycle_one(p)) {
        g_last_recycle = true;
        return;
    }
    g_last_recycle = false;
    delete_file_permanent(p);
#else
    (void)p;
#endif
}

void remove_tree(const string& p) {
#ifdef _WIN32
    if (!path_exists(p)) return;

    // Be forgiving: callers occasionally pass a plain file.
    if (!is_dir(p)) {
        delete_file(p);
        return;
    }
    if (g_recycle && recycle_one(p)) {
        g_last_recycle = true;
        return;
    }
    g_last_recycle = false;
    delete_tree_permanent(p, 0);
#else
    (void)p;
#endif
}

// ===========================================================================
// Config / paths
// ===========================================================================
string home_dir() {
#ifdef _WIN32
    {
        const char* h = getenv("USERPROFILE");
        if (h && *h) return h;
    }
#endif
    {
        const char* h = getenv("HOME");
        if (h && *h) return h;
    }
    return ".";
}

string data_dir() {
    const char* env = getenv("OPENCODE_DATA_DIR");
    if (env && *env) return env;
    return path_join(path_join(path_join(home_dir(), ".local"), "share"), "opencode");
}

string db_path()      { return path_join(data_dir(), "opencode.db"); }
string diff_dir()     { return path_join(path_join(data_dir(), "storage"), "session_diff"); }
string snapshot_dir() { return path_join(data_dir(), "snapshot"); }

// ===========================================================================
// Text helpers
// ===========================================================================
namespace {
inline bool utf8_is_cont(unsigned char c) { return (c & 0xC0) == 0x80; }
inline char ascii_lower(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c; }
}  // namespace

string utf8_ellipsize(const string& s, size_t max_codepoints) {
    if (max_codepoints == 0) return "";
    size_t cp = 0, i = 0;
    while (i < s.size() && cp < max_codepoints) {
        ++i;
        while (i < s.size() && utf8_is_cont((unsigned char)s[i])) ++i;
        ++cp;
    }
    if (i >= s.size()) return s;          // fits, nothing to do
    if (cp == max_codepoints) {           // make room for the ellipsis
        size_t k = i - 1;
        while (k > 0 && utf8_is_cont((unsigned char)s[k])) --k;
        i = k;
    }
    return s.substr(0, i) + "\xE2\x80\xA6";
}

bool utf8_icontains(const string& haystack, const string& needle) {
    if (needle.empty()) return true;
    if (needle.size() > haystack.size()) return false;
    for (size_t i = 0; i + needle.size() <= haystack.size(); ++i) {
        size_t j = 0;
        while (j < needle.size() && ascii_lower(haystack[i + j]) == ascii_lower(needle[j])) ++j;
        if (j == needle.size()) return true;
    }
    return false;
}

wstring format_time(long long ts_ms) {
    if (ts_ms <= 0) return L"-";
    time_t t = (time_t)(ts_ms / 1000);
    struct tm tmv, tmn;
#ifdef _WIN32
    localtime_s(&tmv, &t);
    time_t now = time(nullptr);
    localtime_s(&tmn, &now);
#else
    localtime_r(&t, &tmv);
    time_t now = time(nullptr);
    localtime_r(&now, &tmn);
#endif
    wchar_t buf[64];
    bool same_yday = (tmv.tm_yday == tmn.tm_yday) && (tmv.tm_year == tmn.tm_year);
    if (same_yday) {
        swprintf(buf, 64, L"今天 %02d:%02d", tmv.tm_hour, tmv.tm_min);
    } else if (tmv.tm_year == tmn.tm_year) {
        swprintf(buf, 64, L"%02d-%02d %02d:%02d",
                 tmv.tm_mon + 1, tmv.tm_mday, tmv.tm_hour, tmv.tm_min);
    } else {
        swprintf(buf, 64, L"%04d-%02d-%02d",
                 tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday);
    }
    return buf;
}

wstring format_size(long long size_bytes) {
    wchar_t buf[32];
    if (size_bytes < 1024)
        swprintf(buf, 32, L"%lldB", size_bytes);
    else if (size_bytes < 1024LL * 1024)
        swprintf(buf, 32, L"%.1fK", size_bytes / 1024.0);
    else if (size_bytes < 1024LL * 1024 * 1024)
        swprintf(buf, 32, L"%.1fM", size_bytes / (1024.0 * 1024));
    else
        swprintf(buf, 32, L"%.1fG", size_bytes / (1024.0 * 1024 * 1024));
    return buf;
}

wstring truncate(const wstring& text, size_t width) {
    if (text.size() <= width) return text;
    if (width == 0) return L"";
    return text.substr(0, width - 1) + L"…";
}

wstring ljust(const wstring& text, size_t width) {
    wstring t = truncate(text, width);
    if (t.size() < width) t.append(width - t.size(), L' ');
    return t;
}

wstring rjust(const wstring& text, size_t width) {
    wstring t = truncate(text, width);
    if (t.size() < width) return wstring(width - t.size(), L' ') + t;
    return t;
}

// ===========================================================================
// DB access (sqlite3)
// ===========================================================================
namespace {

// RAII wrappers.  Every early return / exception path now closes the
// connection and finalizes the statements exactly once.
struct DbGuard {
    sqlite3* p = nullptr;
    explicit DbGuard(sqlite3* db) : p(db) {}
    ~DbGuard() { if (p) sqlite3_close(p); }
    DbGuard(const DbGuard&) = delete;
    DbGuard& operator=(const DbGuard&) = delete;
    sqlite3* get() const { return p; }
};

struct StmtGuard {
    sqlite3_stmt* p = nullptr;
    explicit StmtGuard(sqlite3_stmt* s = nullptr) : p(s) {}
    ~StmtGuard() { if (p) sqlite3_finalize(p); }
    StmtGuard(const StmtGuard&) = delete;
    StmtGuard& operator=(const StmtGuard&) = delete;
    sqlite3_stmt* get() const { return p; }
    operator sqlite3_stmt*() const { return p; }
};

string sqlite_error(sqlite3* db) {
    const char* m = db ? sqlite3_errmsg(db) : nullptr;
    return m ? m : "unknown sqlite error";
}

StmtGuard prepare(sqlite3* db, const string& sql) {
    sqlite3_stmt* st = nullptr;
    int rc = sqlite3_prepare_v2(db, sql.c_str(), -1, &st, nullptr);
    // NOTE: the error message must be read while the connection is alive.
    // (The previous code called sqlite3_errmsg() *after* sqlite3_close().)
    if (rc != SQLITE_OK) throw std::runtime_error(sqlite_error(db) + "  [SQL: " + sql + "]");
    return StmtGuard(st);
}

void exec(sqlite3* db, const char* sql) {
    char* err = nullptr;
    if (sqlite3_exec(db, sql, nullptr, nullptr, &err) != SQLITE_OK) {
        string msg = err ? err : sqlite_error(db);
        if (err) sqlite3_free(err);
        throw std::runtime_error(msg);
    }
}

// Rolls the transaction back unless commit() was called.
struct Tx {
    sqlite3* db;
    bool active = false;
    explicit Tx(sqlite3* d) : db(d) { exec(db, "BEGIN IMMEDIATE"); active = true; }
    void commit() { exec(db, "COMMIT"); active = false; }
    ~Tx() { if (active) sqlite3_exec(db, "ROLLBACK", nullptr, nullptr, nullptr); }
    Tx(const Tx&) = delete;
    Tx& operator=(const Tx&) = delete;
};

string quote_ident(const string& s) {
    string out;
    for (char c : s) {
        if (c == '"') out += '"';
        out += c;
    }
    return out;
}

void bind_all_text(sqlite3_stmt* st, const std::vector<string>& values) {
    for (size_t i = 0; i < values.size(); ++i)
        sqlite3_bind_text(st, (int)i + 1, values[i].c_str(), -1, SQLITE_TRANSIENT);
}

string in_placeholders(size_t n) {
    string s;
    for (size_t i = 0; i < n; ++i) s += (i ? ",?" : "?");
    return s;
}

}  // namespace

sqlite3* open_db(const string& path) {
    sqlite3* raw = nullptr;
    // SQLITE_OPEN_READWRITE (deliberately without CREATE): a typo in the data
    // directory now fails loudly instead of silently creating an empty db.
    int rc = sqlite3_open_v2(path.c_str(), &raw, SQLITE_OPEN_READWRITE, nullptr);
    if (rc != SQLITE_OK) {
        string msg = raw ? sqlite_error(raw) : "out of memory";
        if (raw) sqlite3_close(raw);      // sqlite3_open allocates even on failure
        throw std::runtime_error("cannot open database '" + path + "' (SQLite " +
                                 std::to_string(rc) + "): " + msg);
    }
    // Without this, deleting while opencode is running fails instantly with
    // SQLITE_BUSY instead of waiting for the writer to finish.
    sqlite3_busy_timeout(raw, 5000);
    return raw;
}

string col_text(sqlite3_stmt* st, int i) {
    const unsigned char* t = sqlite3_column_text(st, i);
    return t ? reinterpret_cast<const char*>(t) : "";
}

namespace {

bool table_present(sqlite3* db, const char* name) {
    StmtGuard st = prepare(db, "SELECT 1 FROM sqlite_master WHERE type='table' AND name = ?");
    sqlite3_bind_text(st, 1, name, -1, SQLITE_STATIC);
    return sqlite3_step(st) == SQLITE_ROW;
}

}  // namespace

string session_table(sqlite3* db) {
    return table_present(db, "session_v2") ? "session_v2" : "session";
}

string message_table(sqlite3* db) {
    return table_present(db, "session_message") ? "session_message" : "message";
}

std::vector<Session> load_sessions(const string& path) {
    std::vector<Session> out;
    DbGuard db(open_db(path));
    const string sessions = session_table(db.get());
    const string messages = message_table(db.get());
    // LEFT JOIN: a session whose project row is gone is still listed (with the
    // old INNER JOIN it vanished from the UI and could never be deleted).
    // The table names are interpolated (not bound) because SQLite cannot bind
    // identifiers; they come from session_table()/message_table() and are never
    // user input.
    StmtGuard st = prepare(db.get(),
        "SELECT s.id, s.title, s.time_created, s.time_updated,"
        "       COALESCE(p.name, ''), COALESCE(p.worktree, ''),"
        "       (SELECT COUNT(*) FROM \"" + messages + "\" WHERE session_id = s.id)"
        " FROM \"" + sessions + "\" s"
        " LEFT JOIN project p ON s.project_id = p.id"
        " ORDER BY s.time_created DESC");
    while (sqlite3_step(st) == SQLITE_ROW) {
        Session s;
        s.id = col_text(st, 0);
        s.title = utf8_to_wide(col_text(st, 1));
        s.time_created = sqlite3_column_int64(st, 2);
        s.time_updated = sqlite3_column_int64(st, 3);
        s.project_name = col_text(st, 4);
        s.worktree = col_text(st, 5);
        s.msg_count = sqlite3_column_int64(st, 6);
        out.push_back(std::move(s));
    }
    return out;
}

std::vector<Snapshot> load_snapshots(const string& path, const string& snap_dir, bool with_sizes) {
    std::vector<Snapshot> out;
    std::map<string, std::pair<string, string>> project_map;
    std::set<string> active_ids;

    {
        DbGuard db(open_db(path));
        StmtGuard st = prepare(db.get(), "SELECT id, worktree, name FROM project");
        while (sqlite3_step(st) == SQLITE_ROW)
            project_map[col_text(st, 0)] = {col_text(st, 1), col_text(st, 2)};

        StmtGuard st2 = prepare(db.get(), "SELECT DISTINCT project_id FROM \"" +
                                               session_table(db.get()) + "\"");
        while (sqlite3_step(st2) == SQLITE_ROW) active_ids.insert(col_text(st2, 0));
    }

    if (!is_dir(snap_dir)) return out;

    std::vector<string> dirs = list_child_dirs(snap_dir);
    std::sort(dirs.begin(), dirs.end());

    for (auto& d : dirs) {
        Snapshot sn;
        sn.path = d;
        sn.id = file_stem(d);
        sn.size_known = with_sizes;
        sn.size = with_sizes ? total_dir_size(d) : 0;   // lazy mode: UI fills this in
        auto it = project_map.find(sn.id);
        if (it != project_map.end()) {
            sn.worktree = it->second.first;
            sn.name = utf8_to_wide(it->second.second);
        } else {
            sn.worktree = "(deleted project)";
        }
        sn.active_sessions = active_ids.count(sn.id) > 0;
        out.push_back(std::move(sn));
    }
    return out;
}

// ---------------------------------------------------------------------------
// Deleting a session row also has to remove everything that points at it.
//
// Two mechanisms, because neither is sufficient on the real opencode schema:
//   1. The declared foreign key graph (session -> child -> grandchild).  Worth
//      following because it adapts when opencode adds tables.
//   2. Column-name convention: every table that has a `session_id` column is
//      session scoped, and every table with a `message_id` column belongs to a
//      message.  This matters a lot in practice -- in the shipping schema
//      `message`, `part`, `session_message` and `session_input` all carry
//      session_id but declare no foreign key at all, so a FK-only cascade
//      leaves thousands of orphaned rows behind (exactly the bloat this tool
//      exists to remove).
//
// PRAGMA foreign_keys is deliberately left alone: the original code turned it
// on, which only cascaded the two tables that declare ON DELETE CASCADE and
// could abort the whole delete on a constraint.
// ---------------------------------------------------------------------------
namespace {

struct FkRef {
    string table;    // child table
    string column;   // child column that references the parent
    string parent;   // parent table
};

std::vector<FkRef> collect_foreign_keys(sqlite3* db) {
    std::vector<FkRef> refs;
    std::vector<string> tables;
    {
        StmtGuard st = prepare(db, "SELECT name FROM sqlite_master"
                                   " WHERE type='table' AND name NOT LIKE 'sqlite_%'");
        while (sqlite3_step(st) == SQLITE_ROW) tables.push_back(col_text(st, 0));
    }
    for (auto& name : tables) {
        StmtGuard fk = prepare(db, "PRAGMA foreign_key_list(\"" + quote_ident(name) + "\")");
        // columns: id, seq, table, from, to, on_update, on_delete, match
        while (sqlite3_step(fk) == SQLITE_ROW) {
            FkRef r;
            r.table = name;
            r.parent = col_text(fk, 2);
            r.column = col_text(fk, 3);
            if (!r.parent.empty() && !r.column.empty()) refs.push_back(r);
        }
    }
    return refs;
}

// table name -> column names
std::map<string, std::vector<string>> collect_table_columns(sqlite3* db) {
    std::map<string, std::vector<string>> out;
    std::vector<string> tables;
    {
        StmtGuard st = prepare(db, "SELECT name FROM sqlite_master"
                                   " WHERE type='table' AND name NOT LIKE 'sqlite_%'");
        while (sqlite3_step(st) == SQLITE_ROW) tables.push_back(col_text(st, 0));
    }
    for (auto& name : tables) {
        StmtGuard ti = prepare(db, "PRAGMA table_info(\"" + quote_ident(name) + "\")");
        std::vector<string> cols;
        while (sqlite3_step(ti) == SQLITE_ROW) cols.push_back(col_text(ti, 1));
        out[name] = std::move(cols);
    }
    return out;
}

bool has_column(const std::map<string, std::vector<string>>& schema, const string& table,
                const string& column) {
    auto it = schema.find(table);
    if (it == schema.end()) return false;
    for (auto& c : it->second)
        if (c == column) return true;
    return false;
}

int delete_where_in(sqlite3* db, const string& table, const string& column,
                    const std::vector<string>& values) {
    if (values.empty()) return 0;
    StmtGuard st = prepare(db, "DELETE FROM \"" + quote_ident(table) + "\" WHERE \"" +
                                   quote_ident(column) + "\" IN (" +
                                   in_placeholders(values.size()) + ")");
    bind_all_text(st, values);
    if (sqlite3_step(st) != SQLITE_DONE)
        throw std::runtime_error("failed to delete from '" + table + "': " + sqlite_error(db));
    return sqlite3_changes(db);
}

std::vector<string> select_ids_where_in(sqlite3* db, const string& table, const string& column,
                                        const std::vector<string>& values) {
    std::vector<string> out;
    if (values.empty()) return out;
    StmtGuard st = prepare(db, "SELECT \"id\" FROM \"" + quote_ident(table) + "\" WHERE \"" +
                                   quote_ident(column) + "\" IN (" +
                                   in_placeholders(values.size()) + ")");
    bind_all_text(st, values);
    while (sqlite3_step(st) == SQLITE_ROW) out.push_back(col_text(st, 0));
    return out;
}

void prune_children(sqlite3* db, const std::vector<FkRef>& refs,
                    const std::map<string, std::vector<string>>& schema, const string& parent_table,
                    const std::vector<string>& parent_ids, int depth) {
    if (parent_ids.empty() || depth > 4) return;
    for (auto& ref : refs) {
        if (ref.parent != parent_table) continue;
        bool recurse = depth < 4 && has_column(schema, ref.table, "id");
        std::vector<string> child_ids =
            recurse ? select_ids_where_in(db, ref.table, ref.column, parent_ids)
                    : std::vector<string>();
        delete_where_in(db, ref.table, ref.column, parent_ids);
        if (recurse) prune_children(db, refs, schema, ref.table, child_ids, depth + 1);
    }
}

}  // namespace

int delete_sessions(const string& path, const std::vector<string>& ids) {
    if (ids.empty()) return 0;
    DbGuard db(open_db(path));

    std::vector<FkRef> refs;
    std::map<string, std::vector<string>> schema;
    try {
        refs = collect_foreign_keys(db.get());
        schema = collect_table_columns(db.get());
    } catch (const std::exception&) {
        refs.clear();
        schema.clear();   // introspection failed: fall back to a bare session delete
    }

    Tx tx(db.get());
    for (auto& id : ids) {
        std::vector<string> one(1, id);

        // (a) declared foreign keys, transitively
        prune_children(db.get(), refs, schema, "session", one, 0);

        // (b) rows that belong to this session's messages, before the messages go
        if (has_column(schema, "message", "id") && has_column(schema, "message", "session_id")) {
            std::vector<string> msg_ids = select_ids_where_in(db.get(), "message", "session_id", one);
            if (!msg_ids.empty()) {
                for (auto& entry : schema) {
                    if (entry.first == "message") continue;
                    if (has_column(schema, entry.first, "message_id"))
                        delete_where_in(db.get(), entry.first, "message_id", msg_ids);
                }
            }
        }

        // (c) anything else that is directly session scoped
        for (auto& entry : schema) {
            if (has_column(schema, entry.first, "session_id"))
                delete_where_in(db.get(), entry.first, "session_id", one);
        }
    }

    StmtGuard st = prepare(db.get(), "DELETE FROM \"" + session_table(db.get()) + "\" WHERE id = ?");
    int deleted = 0;
    for (auto& id : ids) {
        sqlite3_bind_text(st, 1, id.c_str(), -1, SQLITE_TRANSIENT);
        int rc = sqlite3_step(st);
        if (rc != SQLITE_DONE) {
            string err = sqlite_error(db.get());
            throw std::runtime_error("delete failed for session '" + id + "': " + err);
        }
        deleted += sqlite3_changes(db.get());
        sqlite3_reset(st);
        sqlite3_clear_bindings(st);
    }
    tx.commit();
    return deleted;
}

std::vector<string> cleanup_orphan_diffs(const string& path, const string& diff_dir,
                                         std::vector<string>* failed) {
    std::vector<string> removed;
    std::set<string> existing;
    {
        DbGuard db(open_db(path));
        // Must follow the table the sessions actually live in. On a v2 database
        // the v1 `session` table is frozen, so reading it here would make every
        // session created after the upgrade look like an orphan -- and its
        // session_diff file would be deleted. Both tables are consulted when
        // they exist: a diff is only an orphan if *no* session references it.
        const string live = session_table(db.get());
        StmtGuard st = prepare(db.get(), "SELECT id FROM \"" + live + "\"");
        while (sqlite3_step(st) == SQLITE_ROW) existing.insert(col_text(st, 0));
        if (live != "session" && table_present(db.get(), "session")) {
            StmtGuard old = prepare(db.get(), "SELECT id FROM session");
            while (sqlite3_step(old) == SQLITE_ROW) existing.insert(col_text(old, 0));
        }
    }

    if (!is_dir(diff_dir)) return removed;
    for (auto& f : list_child_files(diff_dir)) {
        if (file_extension(f) != ".json") continue;
        string sid = file_stem(f);
        if (existing.count(sid)) continue;
        try {
            delete_file(f);
            removed.push_back(sid);
        } catch (const std::exception&) {
            if (failed) failed->push_back(f);
        }
    }
    return removed;
}

// ===========================================================================
// Platform actions (shared by TUI and GUI)
// ===========================================================================
namespace {
void set_error(string* error, const string& msg) {
    if (error) *error = msg;
}

#ifdef _WIN32
// Session ids are opaque, but they are pasted into a command line, so refuse
// anything that could break out of the quoting.
bool safe_session_id(const string& id) {
    if (id.empty() || id.size() > 128) return false;
    for (char c : id) {
        bool ok = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                  c == '-' || c == '_' || c == '.';
        if (!ok) return false;
    }
    return true;
}
#endif

}  // namespace

bool launch_opencode_session(const string& session_id, const string& worktree, string* error) {
    if (error) error->clear();
#ifdef _WIN32
    if (!safe_session_id(session_id)) {
        set_error(error, "refusing to launch: session id contains unexpected characters");
        return false;
    }

    wstring cmd = L"cmd.exe /c opencode -s \"" + utf8_to_wide(session_id) + L"\"";
    std::vector<wchar_t> cmdline(cmd.begin(), cmd.end());
    cmdline.push_back(L'\0');

    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_SHOW;
    ZeroMemory(&pi, sizeof(pi));

    // Prefer the session's worktree; if it is gone, start in the user profile
    // rather than inheriting whatever the GUI's working directory happens to be.
    wstring wd;
    if (is_dir(worktree)) wd = win_path(worktree);
    else wd = win_path(home_dir());

    BOOL ok = CreateProcessW(nullptr, &cmdline[0], nullptr, nullptr, FALSE,
                             CREATE_NEW_CONSOLE, nullptr, wd.empty() ? nullptr : wd.c_str(),
                             &si, &pi);
    if (!ok) {
        set_error(error, win_error_text(GetLastError(), "cmd.exe /c opencode"));
        return false;
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return true;
#else
    (void)session_id;
    (void)worktree;
    set_error(error, "not supported on this platform");
    return false;
#endif
}

bool reveal_in_explorer(const string& path, string* error) {
    if (error) error->clear();
#ifdef _WIN32
    wstring target = win_path_plain(path);
    if (target.empty() || !path_exists(path)) {
        set_error(error, "path not found: '" + path + "'");
        return false;
    }
    wstring args = is_dir(path) ? target : (L"/select,\"" + target + L"\"");
    HINSTANCE r = ShellExecuteW(nullptr, L"open", L"explorer.exe", args.c_str(), nullptr,
                                SW_SHOWNORMAL);
    if ((INT_PTR)r <= 32) {
        set_error(error, "could not open Explorer (ShellExecute error " +
                             std::to_string((int)(INT_PTR)r) + ")");
        return false;
    }
    return true;
#else
    (void)path;
    set_error(error, "not supported on this platform");
    return false;
#endif
}

bool set_clipboard_text(const string& text, string* error) {
    if (error) error->clear();
#ifdef _WIN32
    wstring wtext = utf8_to_wide(text);
    if (!OpenClipboard(nullptr)) {
        set_error(error, "could not open the clipboard");
        return false;
    }
    bool ok = false;
    if (EmptyClipboard()) {
        SIZE_T bytes = (wtext.size() + 1) * sizeof(wchar_t);
        HGLOBAL hg = GlobalAlloc(GMEM_MOVEABLE, bytes);
        if (hg) {
            void* dst = GlobalLock(hg);
            if (dst) {
                memcpy(dst, wtext.c_str(), bytes);
                GlobalUnlock(hg);
                if (SetClipboardData(CF_UNICODETEXT, hg)) ok = true;
                else GlobalFree(hg);
            } else {
                GlobalFree(hg);
            }
        }
    }
    CloseClipboard();
    if (!ok) set_error(error, "could not put the text on the clipboard");
    return ok;
#else
    (void)text;
    set_error(error, "not supported on this platform");
    return false;
#endif
}

// ---------------------------------------------------------------------------
// Honour OPENCODE_SM_NO_RECYCLE before anything else runs.
// ---------------------------------------------------------------------------
namespace {
struct DeleteModeInit {
    DeleteModeInit() {
        const char* env = getenv("OPENCODE_SM_NO_RECYCLE");
        if (env && *env && strcmp(env, "0") != 0) g_recycle = false;
    }
} g_delete_mode_init;
}  // namespace
