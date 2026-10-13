// OpenCode Session Manager -- shared data layer (used by TUI and GUI)
#pragma once

#include <sqlite3.h>

#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

using std::string;
using std::wstring;

// ---------------------------------------------------------------------------
// Path helpers.
//
// Every helper takes/returns UTF-8 and goes through the wide-character (W)
// Win32 API internally, so paths containing CJK (or any non-ASCII) characters
// work: a Chinese %USERPROFILE%, a Chinese project worktree, a Chinese snapshot
// name.  The previous ANSI (A) implementation silently returned empty listings
// and failed to delete for those paths.
//
// std::filesystem is intentionally *not* used: this project still targets
// GCC 8 / MinGW-w64, where <filesystem> is unusable.
// ---------------------------------------------------------------------------
string path_join(const string& base, const string& name);
bool path_exists(const string& p);
bool is_dir(const string& p);
bool is_file(const string& p);
bool make_dir(const string& p);   // single level, true if it exists afterwards
std::vector<string> list_children(const string& p);
std::vector<string> list_child_dirs(const string& p);
std::vector<string> list_child_files(const string& p);
string file_extension(const string& path);
string file_stem(const string& path);
long long total_dir_size(const string& p);
long long file_size(const string& p);   // 0 when missing

// Both of these throw std::runtime_error on failure (nothing is silently
// swallowed any more).  See "Deletion policy" below for where the data goes.
void remove_tree(const string& p);   // directory tree
void delete_file(const string& p);   // single file

// ---------------------------------------------------------------------------
// Deletion policy
//
// By default removals are sent to the Windows Recycle Bin (FOF_ALLOWUNDO), so a
// wrong click is recoverable.  Set the environment variable
// OPENCODE_SM_NO_RECYCLE=1, or call set_delete_to_recycle(false), to force
// permanent deletion (used by the test suite).
// ---------------------------------------------------------------------------
void set_delete_to_recycle(bool on);
bool delete_to_recycle();
bool last_delete_went_to_recycle();   // how the most recent removal was done

// ---------------------------------------------------------------------------
// Config / paths
// ---------------------------------------------------------------------------
string home_dir();
string data_dir();
string db_path();
string diff_dir();
string snapshot_dir();

// ---------------------------------------------------------------------------
// UTF-8 <-> wide helpers (Windows)
// ---------------------------------------------------------------------------
wstring utf8_to_wide(const string& s);
string wide_to_utf8(const wstring& w);

// ---------------------------------------------------------------------------
// Text helpers
// ---------------------------------------------------------------------------
// Codepoint-aware ellipsis ("abc…"): never splits a multi-byte UTF-8 sequence.
string utf8_ellipsize(const string& s, size_t max_codepoints);
// ASCII-case-insensitive substring test; safe on UTF-8 (non-ASCII bytes are
// compared verbatim, which is what we want for CJK).
bool utf8_icontains(const string& haystack, const string& needle);

wstring format_time(long long ts_ms);
wstring format_size(long long size_bytes);
wstring truncate(const wstring& text, size_t width);
wstring ljust(const wstring& text, size_t width);
wstring rjust(const wstring& text, size_t width);

// ---------------------------------------------------------------------------
// Data models
// ---------------------------------------------------------------------------
struct Session {
    string id;
    wstring title;
    long long time_created = 0;
    long long time_updated = 0;
    string project_name;
    string worktree;
    long long msg_count = 0;
};

struct Snapshot {
    string id;            // project_id / directory name
    string path;          // directory on disk
    long long size = 0;
    bool size_known = true;   // false => size not computed yet (lazy mode)
    string worktree;
    wstring name;
    bool active_sessions = false;
};

// ---------------------------------------------------------------------------
// DB access (sqlite3)
// ---------------------------------------------------------------------------
sqlite3* open_db(const string& path);
string col_text(sqlite3_stmt* st, int i);

// opencode v2 renamed `session` to `session_v2` (the migration copies the old
// rows across and leaves the v1 table frozen) and moved the message store from
// `message` + `part` into `session_message`.  These resolve the right table for
// the database at hand: the v2 name when it exists, the v1 name otherwise.
// Reading `session` on a v2 database silently hides every session created after
// the upgrade -- and makes cleanup_orphan_diffs treat those sessions' diff files
// as orphans, which is data loss.
string session_table(sqlite3* db);
string message_table(sqlite3* db);

std::vector<Session> load_sessions(const string& path);

// with_sizes == false leaves Snapshot::size at 0 and size_known at false, so a
// UI can fill the sizes in lazily instead of blocking on a full disk walk.
std::vector<Snapshot> load_snapshots(const string& path, const string& snap_dir,
                                     bool with_sizes = true);

// Returns the number of rows actually deleted.  Throws on any SQLite error
// (including SQLITE_BUSY) -- callers must not assume success.
int delete_sessions(const string& path, const std::vector<string>& ids);

// Moves orphan session_diff/*.json files (no matching session row) to the
// Recycle Bin.  Returns the session ids whose diff was removed; files that
// could not be removed are appended to `failed` when that pointer is given.
std::vector<string> cleanup_orphan_diffs(const string& path, const string& diff_dir,
                                         std::vector<string>* failed = nullptr);

// ---------------------------------------------------------------------------
// Platform actions (shared by TUI and GUI, previously duplicated)
// ---------------------------------------------------------------------------
// Opens a new console and runs `opencode -s <id>` inside the session's worktree.
bool launch_opencode_session(const string& session_id, const string& worktree,
                             string* error = nullptr);
// Opens a folder (or selects a file) in Windows Explorer.
bool reveal_in_explorer(const string& path, string* error = nullptr);
// Puts UTF-8 text on the clipboard as CF_UNICODETEXT.
bool set_clipboard_text(const string& text, string* error = nullptr);
