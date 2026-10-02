// OpenCode Session Manager -- terminal UI (ncursesw).
//
// Browse and clean old OpenCode conversations & orphan snapshot directories.
// Feature set mirrors the GUI: filter, sort, page navigation, reveal in
// Explorer, on-demand orphan diff cleanup and progressive snapshot sizing.
//
// Build (MinGW-w64, ncursesw + sqlite3 from C:\MinGW\opt), one line:
//   g++ -std=c++17 -O2 -DNCURSES_WIDECHAR -I"C:\MinGW\opt\include" main.cpp opencode_data.cpp
//        -L"C:\MinGW\opt\lib" -lncursesw -lsqlite3 -luser32 -lshell32 -lole32
//        -o opencode-session-manager.exe
//
// Optional env override for data location (handy for testing):
//   OPENCODE_DATA_DIR=<dir containing opencode.db, storage/, snapshot/>
//
// Key bindings (also shown in the on-screen help):
//   up/down, j/k      move cursor          PgUp/PgDn, Home/End   page / jump
//   Space             toggle selection     Ctrl+A / Ctrl+D       all / none
//   Tab               switch view          F5                    reload data
//   /                 filter               Esc                   clear filter, else quit
//   s / S             cycle sort field / reverse direction
//   d / Enter         delete selected (confirm with y/n)
//   c                 copy resume command (opencode -s <id>)
//   o                 resume session in a new console
//   r                 reveal in Explorer
//   O                 snapshots: orphan directories only
//   x                 clean orphan session_diff files now
//   Ctrl+E            toggle cleanup-on-exit
//   q                 quit
//
// Removals go to the Windows Recycle Bin by default; set OPENCODE_SM_NO_RECYCLE=1
// to delete permanently instead.

#define NCURSES_WIDECHAR 1
#include <ncursesw/ncurses.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN 1
#include <windows.h>   // SetConsoleOutputCP
#endif

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <set>
#include <string>
#include <vector>

#include "opencode_data.hpp"
#include "db_maintenance.hpp"
#include "session_view.hpp"
#include "version.h"

using std::string;
using std::wstring;

// ---------------------------------------------------------------------------
// Color pair ids (keep names from original)
// ---------------------------------------------------------------------------
enum {
    COLOR_NORMAL = 1,
    COLOR_SELECTED,
    COLOR_HEADER,
    COLOR_HELP,
    COLOR_WARN,
    COLOR_DIM,
    COLOR_TAB_ACTIVE,
    COLOR_TAB_INACTIVE,
    COLOR_FILTER
};

// ---------------------------------------------------------------------------
// ncurses helpers
// ---------------------------------------------------------------------------
static void waddswstr(WINDOW* win, const wstring& s) {
    waddnwstr(win, s.c_str(), (int)s.size());
}

static void mvwaddswstr(WINDOW* win, int y, int x, const wstring& s) {
    wmove(win, y, x);
    waddswstr(win, s);
}

// UTF-8 text helpers for the filter box (typed text arrives as raw bytes, so a
// pasted CJK query still forms a valid UTF-8 string).
static bool utf8_cont(unsigned char c) { return (c & 0xC0) == 0x80; }

static void utf8_pop_back(string& s) {
    if (s.empty()) return;
    size_t i = s.size() - 1;
    while (i > 0 && utf8_cont((unsigned char)s[i])) --i;
    s.erase(i);
}

// ---------------------------------------------------------------------------
// Sort modes
// ---------------------------------------------------------------------------
enum {
    TAB_SESSIONS = 0,
    TAB_SNAPSHOTS = 1,
    TAB_DATABASE = 2
};

// Database maintenance actions that need a y/n confirmation first.
enum { MA_NONE = 0, MA_CLEAN_ORPHANS, MA_COMPACT, MA_VACUUM };

// Which fields the 's' key cycles through, per view.
static const SortField kSessionSortFields[] = {SORT_CREATED, SORT_UPDATED, SORT_MESSAGES,
                                               SORT_NAME, SORT_LOCATION};
static const SortField kSnapshotSortFields[] = {SORT_SIZE, SORT_ID, SORT_NAME, SORT_STATUS};
static const int kSessionSortCount = (int)(sizeof(kSessionSortFields) / sizeof(SortField));
static const int kSnapshotSortCount = (int)(sizeof(kSnapshotSortFields) / sizeof(SortField));

// ---------------------------------------------------------------------------
// TUI application
// ---------------------------------------------------------------------------
static const string DB_PATH = db_path();
static const string DIFF_DIR = diff_dir();
static const string SNAPSHOT_DIR = snapshot_dir();

// Snapshot sizes are computed a few per redraw so the UI never stalls on a large
// data directory (the GUI does the same, a frame at a time).
static const int SIZE_SCAN_PER_PUMP = 8;

class App {
public:
    int tab = TAB_SESSIONS;
    std::vector<Session> sessions;
    std::vector<Snapshot> snapshots;
    std::vector<int> view;          // row -> index into sessions/snapshots
    std::set<string> selected;      // session id, or snapshot path
    int cursor = 0;
    int scroll = 0;
    int page_rows = 10;
    bool confirm_mode = false;
    bool filter_mode = false;
    string filter;                  // applied query
    string filter_edit;             // query being typed
    bool orphans_only = false;
    SortField session_sort = SORT_CREATED;
    SortField snapshot_sort = SORT_SIZE;
    bool session_desc = true;
    bool snapshot_desc = true;
    bool cleanup_on_exit = false;
    size_t scan_next = 0;
    size_t scan_pending = 0;
    string message;
    bool running = true;

    // database maintenance (the same background job the GUI uses)
    MaintenanceJob job;
    bool job_seen = true;
    int db_scroll = 0;
    int pending_action = MA_NONE;
    bool preview_mode = false;
    int preview_scroll = 0;
    std::vector<PreviewMessage> preview;
    string preview_for;
    string preview_error;

    void run() {
        init_tui();
        refresh_data(false);
        while (running) {
            draw();
            // Poll while sizes or a maintenance job are still running, so both
            // make progress without the user having to press a key.
            bool busy = scan_pending > 0 || job.running();
            if (preview_mode || pending_action != MA_NONE) busy = false;
            if (busy) {
                timeout(50);
                int key = getch();
                if (key == ERR) {
                    if (scan_pending) pump_sizes(SIZE_SCAN_PER_PUMP);
                    check_job();
                    continue;
                }
                handle_key(key);
            } else {
                timeout(-1);
                handle_key(getch());
            }
        }
        job.wait();   // never leave a worker mid-flight
        cleanup_screen();
    }

private:
    void init_tui() {
        curs_set(0);
        start_color();
        use_default_colors();
        init_pair(COLOR_NORMAL, -1, -1);
        init_pair(COLOR_SELECTED, COLOR_GREEN, -1);
        init_pair(COLOR_HEADER, COLOR_BLACK, COLOR_CYAN);
        init_pair(COLOR_HELP, COLOR_BLACK, COLOR_WHITE);
        init_pair(COLOR_WARN, COLOR_RED, -1);
        init_pair(COLOR_DIM, 8, -1);
        init_pair(COLOR_TAB_ACTIVE, COLOR_BLACK, COLOR_GREEN);
        init_pair(COLOR_TAB_INACTIVE, COLOR_BLACK, COLOR_CYAN);
        init_pair(COLOR_FILTER, COLOR_YELLOW, -1);
        cbreak();
        noecho();
        keypad(stdscr, TRUE);
    }

    void cleanup_screen() { endwin(); }

    // -----------------------------------------------------------------------
    // Background maintenance
    // -----------------------------------------------------------------------
    static string timestamp_string() {
        time_t now = time(nullptr);
        struct tm tmv;
#ifdef _WIN32
        localtime_s(&tmv, &now);
#else
        localtime_r(&now, &tmv);
#endif
        char buf[32];
        snprintf(buf, sizeof(buf), "%04d%02d%02d-%02d%02d%02d", tmv.tm_year + 1900, tmv.tm_mon + 1,
                 tmv.tm_mday, tmv.tm_hour, tmv.tm_min, tmv.tm_sec);
        return buf;
    }

    void check_job() {
        if (job.running() || job_seen) return;
        JobStatus st = job.status();
        job_seen = true;
        if (!st.ok && !st.error.empty()) {
            message = "Error: " + utf8_ellipsize(st.error, 120);
            return;
        }
        char buf[256];
        switch (st.type) {
        case JobType::Report:
            message = "Database scan finished (j/k or PgUp/PgDn to scroll).";
            break;
        case JobType::CleanupOrphans:
            snprintf(buf, sizeof(buf), "Removed %d orphan event row(s) (%s). Press v to VACUUM.",
                     (int)st.result.rows, wide_to_utf8(format_size(st.result.bytes)).c_str());
            message = buf;
            refresh_data(true);
            break;
        case JobType::CompactEvents:
            if (st.result.rows > 0) {
                snprintf(buf, sizeof(buf), "Compacted %d row(s) (%s). Press v to VACUUM.",
                         (int)st.result.rows,
                         wide_to_utf8(format_size(st.result.bytes)).c_str());
                refresh_data(true);
            } else {
                snprintf(buf, sizeof(buf), "Dry run: %d of %d rows redundant, %s reclaimable.",
                         (int)st.plan.removable_rows, (int)st.plan.total_rows,
                         wide_to_utf8(format_size(st.plan.removable_bytes)).c_str());
            }
            message = buf;
            break;
        case JobType::Vacuum:
            snprintf(buf, sizeof(buf), "VACUUM: %s -> %s (freed %s)",
                     wide_to_utf8(format_size(st.result.file_before)).c_str(),
                     wide_to_utf8(format_size(st.result.file_after)).c_str(),
                     wide_to_utf8(format_size(st.result.bytes)).c_str());
            message = buf;
            break;
        case JobType::Backup:
            snprintf(buf, sizeof(buf), "Backup written (%s)",
                     wide_to_utf8(format_size(st.result.bytes)).c_str());
            message = buf;
            break;
        default:
            break;
        }
    }

    void start_action(int action) {
        switch (action) {
        case MA_CLEAN_ORPHANS:
            job.start(JobType::CleanupOrphans, DB_PATH);
            break;
        case MA_COMPACT: {
            // Always back up first from the interactive UI: this rewrites the log.
            string backup = DB_PATH + ".backup-" + timestamp_string();
            job.start(JobType::CompactEvents, DB_PATH, backup);
            message = "Compacting (backup: " + backup + ")";
            break;
        }
        case MA_VACUUM:
            if (database_is_busy(DB_PATH)) {
                message = "Error: database is locked (is opencode running?).";
                return;
            }
            job.start(JobType::Vacuum, DB_PATH);
            break;
        default:
            return;
        }
        job_seen = false;
    }

    void export_selected() {
        std::vector<string> ids(selected.begin(), selected.end());
        if (ids.empty()) {
            message = "Nothing selected to export.";
            return;
        }
        string dir = path_join(data_dir(), "export-" + timestamp_string());
        std::vector<string> written;
        string err;
        if (export_sessions_json(DB_PATH, ids, dir, &written, &err))
            message = "Exported " + std::to_string(written.size()) + " session(s) to " + dir;
        else
            message = "Error: " + err;
    }

    void load_preview_now() {
        preview.clear();
        preview_error.clear();
        preview_for.clear();
        if (tab != TAB_SESSIONS) return;
        string id = key_at(cursor);
        if (id.empty()) return;
        preview_for = id;
        try {
            preview = load_session_preview(DB_PATH, id, 30);
        } catch (const std::exception& e) {
            preview_error = e.what();
        }
    }

    // -----------------------------------------------------------------------
    // Database view
    // -----------------------------------------------------------------------
    bool handle_database_key(int key) {
        switch (key) {
        case 'r':
            job.start(JobType::Report, DB_PATH);
            job_seen = false;
            message = "Scanning database (read-only)...";
            return true;
        case 'c':
            job.start(JobType::CompactEvents, DB_PATH);
            job_seen = false;
            message = "Analysing event log (dry run)...";
            return true;
        case 'x':
            pending_action = MA_CLEAN_ORPHANS;
            return true;
        case 'b':
            pending_action = MA_COMPACT;
            return true;
        case 'v':
            pending_action = MA_VACUUM;
            return true;
        case KEY_DOWN:
        case 'j':
            db_scroll++;
            return true;
        case KEY_UP:
        case 'k':
            if (db_scroll > 0) db_scroll--;
            return true;
        case KEY_NPAGE:
            db_scroll += page_rows;
            return true;
        case KEY_PPAGE:
            db_scroll -= page_rows;
            if (db_scroll < 0) db_scroll = 0;
            return true;
        case KEY_HOME:
            db_scroll = 0;
            return true;
        default:
            return false;
        }
    }

    void draw_database_view(int start_y, int end_y, int w) {
        JobStatus st = job.status();
        std::vector<wstring> lines;
        const DbReport& r = st.report;

        auto add = [&](const wstring& s) { lines.push_back(s); };
        auto addf = [&](const char* fmt, ...) {
            char buf[512];
            va_list ap;
            va_start(ap, fmt);
            vsnprintf(buf, sizeof(buf), fmt, ap);
            va_end(ap);
            lines.push_back(utf8_to_wide(buf));
        };
        auto sz = [](long long b) { return wide_to_utf8(format_size(b)); };

        if (st.running) {
            addf("  %s  [%d%%]", st.stage.c_str(), st.percent);
            add(L"");
        }

        if (r.file_bytes <= 0) {
            add(L"  Press r to scan the database (read-only, takes a minute on a big file).");
            add(L"");
            add(L"  In a real opencode database 90%+ of the file can be the append-only");
            add(L"  `event` log: it stores a complete snapshot of a message part on every");
            add(L"  update, and deleting sessions does not touch those rows - which is why");
            add(L"  the file never shrinks.");
            add(L"");
            add(L"  r  scan                    c  analyse event log (dry run)");
            add(L"  x  clean orphan events     b  compact (always backs up first)");
            add(L"  v  VACUUM");
        } else {
            add(L"  database file    " + utf8_to_wide(sz(r.file_bytes)) + L"   (" +
                std::to_wstring(r.page_count) + L" pages x " + std::to_wstring(r.page_size) +
                L" B)");
            add(L"  free pages       " + utf8_to_wide(sz(r.free_bytes)) + L"   <- VACUUM (v)");
            add(L"  event log        " + utf8_to_wide(sz(r.event_bytes)) + L"   (" +
                std::to_wstring(r.event_rows) + L" rows)");
            add(L"  session/message/part rows: " + std::to_wstring(r.session_rows) + L" / " +
                std::to_wstring(r.message_rows) + L" / " + std::to_wstring(r.part_rows));
            add(L"");
            add(L"  orphan events (deleted sessions)  " + utf8_to_wide(sz(r.orphan_event_bytes)) +
                L"  (" + std::to_wstring(r.orphan_event_rows) + L" rows)   -> x");
            add(L"  safe to reclaim now               " + utf8_to_wide(sz(r.reclaimable_safe())));
            add(L"");

            add(L"  largest tables" + wstring(r.dbstat_available ? L" (dbstat)" : L" (estimated)"));
            for (size_t i = 0; i < r.tables.size() && i < 8; ++i)
                addf("    %-26s %10s", r.tables[i].name.c_str(), sz(r.tables[i].bytes).c_str());
            add(L"");
            add(L"  event log by type");
            for (size_t i = 0; i < r.event_types.size() && i < 8; ++i)
                addf("    %-26s %8d rows %10s", r.event_types[i].name.c_str(),
                     (int)r.event_types[i].rows, sz(r.event_types[i].bytes).c_str());
            add(L"");
            add(L"  top sessions by event footprint");
            for (size_t i = 0; i < r.sessions.size() && i < 8; ++i)
                addf("    %-30s %8d rows %10s  %s", utf8_ellipsize(r.sessions[i].name, 30).c_str(),
                     (int)r.sessions[i].rows, sz(r.sessions[i].bytes).c_str(),
                     utf8_ellipsize(r.sessions[i].extra, 28).c_str());
            if (!r.big_items.empty()) {
                add(L"");
                add(L"  oversized objects");
                for (size_t i = 0; i < r.big_items.size(); ++i)
                    addf("    %-50s %10s", utf8_ellipsize(r.big_items[i].name, 50).c_str(),
                         sz(r.big_items[i].bytes).c_str());
            }
        }

        if (st.plan.total_rows > 0) {
            add(L"");
            add(L"  event log compaction (dry run)");
            addf("    %d of %d part.updated rows are redundant, %s reclaimable",
                 (int)st.plan.removable_rows, (int)st.plan.total_rows,
                 sz(st.plan.removable_bytes).c_str());
            if (!st.plan.worst_parts.empty())
                addf("    worst part: %s with %d versions", st.plan.worst_parts[0].name.c_str(),
                     (int)st.plan.worst_parts[0].rows);
            add(L"    b = apply (a backup is taken first)");
        }

        int visible = end_y - start_y;
        if (visible < 1) visible = 1;
        int max_scroll = (int)lines.size() - visible;
        if (max_scroll < 0) max_scroll = 0;
        if (db_scroll > max_scroll) db_scroll = max_scroll;
        if (db_scroll < 0) db_scroll = 0;

        wattrset(stdscr, COLOR_PAIR(COLOR_DIM));
        for (int i = 0; i < visible && (size_t)(db_scroll + i) < lines.size(); ++i) {
            const wstring& line = lines[(size_t)(db_scroll + i)];
            bool highlight = line.find(L"->") != wstring::npos;
            if (highlight) wattrset(stdscr, COLOR_PAIR(COLOR_WARN));
            mvwaddswstr(stdscr, start_y + i, 0, truncate(line, (size_t)w));
            if (highlight) wattrset(stdscr, COLOR_PAIR(COLOR_DIM));
        }
        wattrset(stdscr, A_NORMAL);
    }

    // -----------------------------------------------------------------------
    // Data
    // -----------------------------------------------------------------------
    void refresh_data(bool keep_view) {
        sessions = load_sessions(DB_PATH);
        // List only: sizes are measured progressively by pump_sizes().
        snapshots = load_snapshots(DB_PATH, SNAPSHOT_DIR, /*with_sizes=*/false);
        scan_next = 0;
        scan_pending = snapshots.size();

        if (!keep_view) {
            selected.clear();
            cursor = 0;
            scroll = 0;
        } else {
            prune_selection();
        }
        rebuild_view();
    }

    void pump_sizes(int budget) {
        const size_t n = snapshots.size();
        if (!n || scan_pending == 0) return;

        int done = 0, scanned = 0;
        while (done < budget && scanned < (int)n) {
            size_t i = scan_next % n;
            scan_next = (i + 1) % n;
            ++scanned;
            if (snapshots[i].size_known) continue;
            snapshots[i].size = total_dir_size(snapshots[i].path);
            snapshots[i].size_known = true;
            --scan_pending;
            ++done;
        }
        // Re-sort only once the scan is complete, otherwise rows would move
        // around under the cursor while sizes trickle in.
        if (scan_pending == 0) {
            if (tab == TAB_SNAPSHOTS && snapshot_sort == SORT_SIZE) rebuild_view();
        }
    }

    // -----------------------------------------------------------------------
    // Filtering / sorting / view
    // -----------------------------------------------------------------------
    // The query in effect: while typing it is the edit buffer, so Esc can drop
    // the whole edit without ever having touched the applied filter.
    const string& active_filter() const { return filter_mode ? filter_edit : filter; }

    void rebuild_view() {
        if (tab == TAB_DATABASE) {
            view.clear();
            cursor = 0;
            scroll = 0;
            return;
        }
        ViewQuery q;
        q.filter = active_filter();
        q.orphans_only = orphans_only;
        if (tab == TAB_SESSIONS) {
            q.field = session_sort;
            q.descending = session_desc;
            view = build_session_view(sessions, q);
        } else {
            q.field = snapshot_sort;
            q.descending = snapshot_desc;
            view = build_snapshot_view(snapshots, q);
        }
        clamp_state();
    }

    size_t total_rows() const {
        if (tab == TAB_DATABASE) return 0;
        return tab == TAB_SESSIONS ? sessions.size() : snapshots.size();
    }
    size_t visible_rows() const { return view.size(); }

    string key_at(int row) const {
        if (row < 0 || row >= (int)view.size()) return string();
        int idx = view[(size_t)row];
        return tab == TAB_SESSIONS ? sessions[(size_t)idx].id : snapshots[(size_t)idx].path;
    }

    void clamp_state() {
        int n = (int)view.size();
        if (n <= 0) {
            cursor = 0;
            scroll = 0;
            return;
        }
        if (cursor > n - 1) cursor = n - 1;
        if (cursor < 0) cursor = 0;
        if (scroll < 0) scroll = 0;
        if (scroll > cursor) scroll = cursor;
    }

    void prune_selection() {
        std::set<string> alive;
        for (auto& s : sessions) alive.insert(s.id);
        for (auto& s : snapshots) alive.insert(s.path);
        for (auto it = selected.begin(); it != selected.end();) {
            if (!alive.count(*it)) it = selected.erase(it);
            else ++it;
        }
    }

    // -----------------------------------------------------------------------
    // Input
    // -----------------------------------------------------------------------
    void handle_key(int key) {
        if (key == ERR) return;

        if (preview_mode) {
            switch (key) {
            case 'q':
            case 27:
            case 'p':
                preview_mode = false;
                break;
            case KEY_DOWN:
            case 'j':
                preview_scroll++;
                break;
            case KEY_UP:
            case 'k':
                if (preview_scroll > 0) preview_scroll--;
                break;
            case KEY_NPAGE:
                preview_scroll += page_rows;
                break;
            case KEY_PPAGE:
                preview_scroll -= page_rows;
                if (preview_scroll < 0) preview_scroll = 0;
                break;
            default:
                break;
            }
            return;
        }
        if (pending_action != MA_NONE) {
            int action = pending_action;
            pending_action = MA_NONE;
            if (key == 'y' || key == 'Y') start_action(action);
            return;
        }
        if (filter_mode) {
            handle_filter_key(key);
            return;
        }
        if (confirm_mode) {
            if (key == 'y' || key == 'Y') do_delete();
            confirm_mode = false;
            return;
        }

        if (tab == TAB_DATABASE && handle_database_key(key)) return;

        switch (key) {
        case 'q':
        case 'Q':
            running = false;
            break;
        case 27:                                  // Esc: clear filter first, then quit
            if (!filter.empty()) {
                filter.clear();
                rebuild_view();
                message = "Filter cleared.";
            } else {
                running = false;
            }
            break;        case '\t':
            tab = (tab + 1) % 3;
            selected.clear();
            cursor = 0;
            scroll = 0;
            message.clear();
            rebuild_view();
            break;
        case 'p':
            if (tab == TAB_SESSIONS) {
                preview_mode = true;
                preview_scroll = 0;
                load_preview_now();
            } else {
                message = "Preview is only available for sessions.";
            }
            break;
        case 'e':
            export_selected();
            break;
        case KEY_UP:
        case 'k':
            if (cursor > 0) cursor--;
            break;
        case KEY_DOWN:
        case 'j':
            if (cursor < (int)visible_rows() - 1) cursor++;
            break;
        case KEY_PPAGE:
            cursor -= page_rows;
            if (cursor < 0) cursor = 0;
            break;
        case KEY_NPAGE:
            cursor += page_rows;
            if (cursor > (int)visible_rows() - 1) cursor = (int)visible_rows() - 1;
            if (cursor < 0) cursor = 0;
            break;
        case KEY_HOME:
            cursor = 0;
            break;
        case KEY_END:
            cursor = (int)visible_rows() - 1;
            if (cursor < 0) cursor = 0;
            break;
        case ' ': {
            string k = key_at(cursor);
            if (!k.empty()) {
                if (selected.count(k)) selected.erase(k);
                else selected.insert(k);
            }
            break;
        }
        case 1:                                   // Ctrl+A
            selected.clear();
            for (size_t i = 0; i < visible_rows(); ++i) selected.insert(key_at((int)i));
            message = "Selected " + std::to_string(selected.size()) + " visible item(s).";
            break;
        case 4:                                   // Ctrl+D
            selected.clear();
            break;
        case '/':
            filter_mode = true;
            filter_edit = filter;
            break;
        case 's':
            if (tab == TAB_SESSIONS) {
                int i = 0;
                while (i < kSessionSortCount && kSessionSortFields[i] != session_sort) ++i;
                session_sort = kSessionSortFields[(i + 1) % kSessionSortCount];
            } else {
                int i = 0;
                while (i < kSnapshotSortCount && kSnapshotSortFields[i] != snapshot_sort) ++i;
                snapshot_sort = kSnapshotSortFields[(i + 1) % kSnapshotSortCount];
            }
            rebuild_view();
            message = "Sort: " + string(sort_field_name(current_sort_field())) +
                      (current_sort_desc() ? " (desc)" : " (asc)");
            break;
        case 'S':
            if (tab == TAB_SESSIONS) session_desc = !session_desc;
            else snapshot_desc = !snapshot_desc;
            rebuild_view();
            message = "Sort: " + string(sort_field_name(current_sort_field())) +
                      (current_sort_desc() ? " (desc)" : " (asc)");
            break;
        case 'O':
            orphans_only = !orphans_only;
            if (tab != TAB_SNAPSHOTS) tab = TAB_SNAPSHOTS;
            selected.clear();
            cursor = 0;
            scroll = 0;
            rebuild_view();
            message = orphans_only ? "Showing orphan snapshot directories only."
                                   : "Showing all snapshot directories.";
            break;
        case 'c':
        case 'C':
            copy_command();
            break;
        case 'o':
            jump_session();
            break;
        case 'r':
            reveal_path();
            break;
        case 'x':
            clean_orphan_diffs_now();
            break;
        case 5:                                   // Ctrl+E
            cleanup_on_exit = !cleanup_on_exit;
            message = string("Cleanup orphan diffs on exit: ") + (cleanup_on_exit ? "ON" : "OFF");
            break;
        case 'd':
        case 'D':
        case 10:
        case 13:
        case KEY_ENTER:
            if (!selected.empty()) confirm_mode = true;
            else message = "Nothing selected.";
            break;
        case KEY_F(5):
            refresh_data(true);
            message = "Reloaded.";
            break;
        default:
            break;
        }
    }

    void handle_filter_key(int key) {
        switch (key) {
        case 10:
        case 13:
        case KEY_ENTER:
            filter = filter_edit;
            filter_mode = false;
            rebuild_view();
            cursor = 0;
            scroll = 0;
            message = filter.empty() ? "Filter cleared."
                                     : "Filter: \"" + filter + "\" -> " +
                                           std::to_string(visible_rows()) + " match(es)";
            break;
        case 27:                                  // Esc cancels the edit
            filter_edit = filter;
            filter_mode = false;
            rebuild_view();
            clamp_state();
            break;
        case KEY_BACKSPACE:
        case 127:
        case 8:
            utf8_pop_back(filter_edit);
            break;
        default:
            // Printable bytes (including multi-byte UTF-8, so a pasted CJK query
            // works). Filter live while typing.
            if (key >= 32 && key < 256) {
                filter_edit += (char)key;
                rebuild_view();
                clamp_state();
            }
            break;
        }
    }

    void copy_command() {
        if (tab != TAB_SESSIONS) {
            message = "Resume command copy is only available for sessions.";
            return;
        }
        if (view.empty()) return;
        const Session& s = sessions[(size_t)view[(size_t)cursor]];
        string cmd = "opencode -s " + s.id;
        string err;
        if (set_clipboard_text(cmd, &err)) message = "Copied: " + cmd;
        else message = "Error: " + err;
    }

    void jump_session() {
        if (tab != TAB_SESSIONS) {
            message = "Jump is only available for sessions.";
            return;
        }
        if (view.empty()) return;
        const Session& s = sessions[(size_t)view[(size_t)cursor]];
        string err;
        if (launch_opencode_session(s.id, s.worktree, &err)) message = "Resumed session " + s.id;
        else message = "Error: " + err;
    }

    void reveal_path() {
        if (view.empty()) return;
        string path, err;
        if (tab == TAB_SESSIONS) {
            path = sessions[(size_t)view[(size_t)cursor]].worktree;
            if (path.empty()) {
                message = "This session's project directory is unknown.";
                return;
            }
        } else {
            path = snapshots[(size_t)view[(size_t)cursor]].path;
        }
        if (reveal_in_explorer(path, &err)) message = "Opened in Explorer: " + path;
        else message = "Error: " + err;
    }

    void clean_orphan_diffs_now() {
        try {
            std::vector<string> failed;
            auto orphans = cleanup_orphan_diffs(DB_PATH, DIFF_DIR, &failed);
            if (orphans.empty() && failed.empty())
                message = "No orphan session_diff files to clean.";
            else
                message = (delete_to_recycle() ? "Moved " : "Removed ") +
                          std::to_string(orphans.size()) + " orphan diff file(s)" +
                          (failed.empty() ? "" : ", " + std::to_string(failed.size()) + " failed");
        } catch (const std::exception& e) {
            message = string("Error: ") + e.what();
        }
    }

    void do_delete() {
        if (tab == TAB_SESSIONS) delete_sessions_ui();
        else delete_snapshots_ui();
    }

    void delete_sessions_ui() {
        std::vector<string> ids(selected.begin(), selected.end());
        if (ids.empty()) return;
        try {
            int n = delete_sessions(DB_PATH, ids);
            std::vector<string> failed;
            auto orphans = cleanup_orphan_diffs(DB_PATH, DIFF_DIR, &failed);
            message = "Deleted " + std::to_string(n) + " session(s)";
            if (!orphans.empty())
                message += (delete_to_recycle() ? ", moved " : ", removed ") +
                           std::to_string(orphans.size()) + " orphan diff(s)";
            if (!failed.empty())
                message += ", " + std::to_string(failed.size()) + " diff(s) failed";
            refresh_data(true);
        } catch (const std::exception& e) {
            message = string("Error: ") + e.what();
        }
    }

    void delete_snapshots_ui() {
        std::vector<string> paths;
        long long total_size = 0;
        for (auto& key : selected) {
            for (auto& s : snapshots) {
                if (s.path == key) {
                    paths.push_back(s.path);
                    total_size += s.size;
                    break;
                }
            }
        }
        if (paths.empty()) return;

        size_t failed = 0;
        string first_error;
        for (auto& d : paths) {
            try {
                remove_tree(d);
            } catch (const std::exception& e) {
                if (failed == 0) first_error = e.what();
                ++failed;
            }
        }

        if (failed == 0) {
            string size_text = wide_to_utf8(format_size(total_size));
            if (delete_to_recycle())
                message = "Moved " + std::to_string(paths.size()) +
                          " snapshot(s) to the Recycle Bin (" + size_text + ")";
            else
                message = "Removed " + std::to_string(paths.size()) + " snapshot(s), freed " +
                          size_text;
        } else if (failed == paths.size()) {
            message = "Error: " + first_error;
        } else {
            string size_text = wide_to_utf8(format_size(total_size));
            message = "Removed " + std::to_string(paths.size() - failed) + " snapshot(s) (" +
                      size_text + "), " + std::to_string(failed) +
                      " failed: " + utf8_ellipsize(first_error, 40);
        }
        refresh_data(true);
    }

    // -----------------------------------------------------------------------
    // Drawing
    // -----------------------------------------------------------------------
    SortField current_sort_field() const {
        return tab == TAB_SESSIONS ? session_sort : snapshot_sort;
    }
    bool current_sort_desc() const {
        return tab == TAB_SESSIONS ? session_desc : snapshot_desc;
    }
    wstring sort_label() const {
        if (tab == TAB_DATABASE) return L"-";
        return utf8_to_wide(sort_field_name(current_sort_field()));
    }

    void draw() {
        erase();
        int h = 0, w = 0;
        getmaxyx(stdscr, h, w);
        if (w < 1) w = 1;
        if (h < 1) h = 1;

        draw_header(w);

        int list_start_y = 2;
        int list_end_y = h - 5;                 // cursor info + 2 help lines + message below
        if (list_end_y <= list_start_y) list_end_y = list_start_y + 1;
        int visible = list_end_y - list_start_y;
        page_rows = visible > 1 ? visible - 1 : 1;

        if (tab == TAB_SESSIONS) draw_session_list(list_start_y, list_end_y, w);
        else if (tab == TAB_SNAPSHOTS) draw_snapshot_list(list_start_y, list_end_y, w);
        else draw_database_view(list_start_y, list_end_y, w);

        draw_info_line(h - 4, w);
        draw_help_lines(h - 3, w);              // occupies h-3 and h-2
        draw_message_line(h - 1, w);

        if (confirm_mode) draw_confirm_dialog(h, w);
        if (pending_action != MA_NONE) draw_action_prompt(h, w);
        if (preview_mode) draw_preview_overlay(h, w);

        wnoutrefresh(stdscr);
        doupdate();
    }

    // Line 0: tabs, filter state and totals.
    void draw_header(int w) {
        wattrset(stdscr, A_NORMAL);
        mvwaddswstr(stdscr, 0, 0, wstring((size_t)w, L' '));

        wchar_t buf[128];
        swprintf(buf, 128, L" Sessions (%d) ", (int)sessions.size());
        wstring tab_s = buf;
        swprintf(buf, 128, L" Snapshots (%d) ", (int)snapshots.size());
        wstring tab_p = buf;
        wstring tab_d = L" Database ";

        int off = 0;
        struct TabLabel { const wstring& label; bool active; } tabs[3] = {
            {tab_s, tab == TAB_SESSIONS},
            {tab_p, tab == TAB_SNAPSHOTS},
            {tab_d, tab == TAB_DATABASE}
        };
        for (auto& t : tabs) {
            wattrset(stdscr, t.active ? A_REVERSE : COLOR_PAIR(COLOR_DIM));
            mvwaddswstr(stdscr, 0, off, t.label);
            off += (int)t.label.size();
        }

        // right hand side: filter / sort / orphan totals
        size_t orphans = 0;
        long long orphan_bytes = 0;
        bool sizes_pending = false;
        for (auto& s : snapshots) {
            if (!s.active_sessions) {
                ++orphans;
                orphan_bytes += s.size;
            }
            if (!s.size_known) sizes_pending = true;
        }

        wstring right;
        const string& shown = active_filter();
        if (!shown.empty() && tab != TAB_DATABASE) {
            wstring f = utf8_to_wide(shown);
            if (f.size() > 24) f = truncate(f, 24);
            right += L"filter:\"" + f + L"\" ";
        }
        if (tab == TAB_DATABASE) {
            JobStatus st = job.status();
            if (st.running)
                right += utf8_to_wide(std::to_string(st.percent) + "% " + st.stage);
            else
                right += L"r:scan  c:analyse  x:clean  b:compact  v:vacuum";
        } else {
            swprintf(buf, 128, L"%d/%d  sort:%ls%ls  orphan:%d(%ls)%ls",
                     (int)visible_rows(), (int)total_rows(), sort_label().c_str(),
                     current_sort_desc() ? L"\u2193" : L"\u2191", (int)orphans,
                     format_size(orphan_bytes).c_str(),
                     sizes_pending ? L"  sizing..." : L"");
            right += buf;
        }

        int x = w - (int)right.size();
        if (x > off) {
            wattrset(stdscr, shown.empty() ? COLOR_PAIR(COLOR_DIM) : COLOR_PAIR(COLOR_FILTER));
            mvwaddswstr(stdscr, 0, x, right);
        }
        wattrset(stdscr, A_NORMAL);
    }

    void draw_info_line(int y, int w) {
        if (y < 0 || y >= LINES) return;
        string info;
        if (tab == TAB_DATABASE) {
            JobStatus st = job.status();
            if (st.running)
                info = "Working: " + st.stage + "  (" + std::to_string(st.percent) + "%)";
            else if (st.finished && !st.error.empty())
                info = "Last error: " + st.error;
            else if (st.finished)
                info = string("Last job: ") + job_type_name(st.type) + "  " +
                       std::to_string((int)st.elapsed) + "s";
        } else if (tab == TAB_SESSIONS) {
            if (!view.empty()) {
                const Session& s = sessions[(size_t)view[(size_t)cursor]];
                info = "Session ID: " + s.id;
                if (!s.worktree.empty()) info += "   worktree: " + s.worktree;
            }
        } else {
            if (!view.empty()) {
                const Snapshot& s = snapshots[(size_t)view[(size_t)cursor]];
                info = "Snapshot: " + wide_to_utf8(s.name) + "   path: " + s.path;
                if (!s.worktree.empty()) info += "   worktree: " + s.worktree;
            }
        }
        wattrset(stdscr, COLOR_PAIR(COLOR_DIM));
        mvwaddswstr(stdscr, y, 0, truncate(utf8_to_wide(info), (size_t)w));
        wattrset(stdscr, A_NORMAL);
    }

    void draw_help_lines(int y, int w) {
        if (y < 0 || y >= LINES) return;
        if (filter_mode) {
            WINDOW* win = derwin(stdscr, 1, w, y, 0);
            wbkgd(win, ' ' | COLOR_PAIR(COLOR_FILTER));
            wstring prompt = L" Filter: " + utf8_to_wide(filter_edit) + L"_";
            waddswstr(win, truncate(prompt, (size_t)w));
            wnoutrefresh(win);
            delwin(win);
        } else if (confirm_mode) {
            WINDOW* win = derwin(stdscr, 1, w, y, 0);
            wbkgd(win, ' ' | COLOR_PAIR(COLOR_HELP));
            waddswstr(win, truncate(L" Confirm remove?  [Y] Yes  [N] Cancel ", (size_t)w));
            wnoutrefresh(win);
            delwin(win);
        } else {
            wchar_t buf[160];
            if (tab == TAB_DATABASE)
                swprintf(buf, 160,
                         L" r:Scan  c:Analyse  x:Clean orphans  b:Compact(+backup)  v:VACUUM  "
                         L"j/k:Scroll  Tab:View  q:Quit ");
            else
                swprintf(buf, 160,
                         L" up/down:Move  Space:Sel  ^A:All  ^D:None  /:Filter  s:Sort  "
                         L"d:Del  c:Copy  o:Jump  p:Preview  e:Export  Tab:View  F5:Reload  "
                         L"q:Quit | Sel %d/%d ",
                         (int)selected.size(), (int)visible_rows());
            WINDOW* win = derwin(stdscr, 1, w, y, 0);
            wbkgd(win, ' ' | COLOR_PAIR(COLOR_HELP));
            waddswstr(win, truncate(buf, (size_t)w));
            wnoutrefresh(win);
            delwin(win);
        }

        // second help line: secondary keys / state
        if (y + 1 < LINES) {
            wchar_t buf2[160];
            swprintf(buf2, 160,
                     L" PgUp/PgDn:Page  Home/End:Jump  r:Reveal  O:Orphans  x:Clean diffs  "
                     L"^E:cleanup on exit:%ls ",
                     cleanup_on_exit ? L"ON" : L"OFF");
            wattrset(stdscr, COLOR_PAIR(COLOR_DIM));
            mvwaddswstr(stdscr, y + 1, 0, truncate(buf2, (size_t)w));
            wattrset(stdscr, A_NORMAL);
        }
    }

    void draw_message_line(int y, int w) {
        if (y < 0 || y >= LINES || message.empty()) return;
        attr_t attr = COLOR_PAIR(message.find("Error") != string::npos ? COLOR_WARN : COLOR_DIM);
        wattrset(stdscr, attr);
        mvwaddswstr(stdscr, y, 0, truncate(utf8_to_wide(message), w > 0 ? (size_t)w - 1 : 0));
        wattrset(stdscr, A_NORMAL);
    }

    void adjust_scroll(int visible) {
        if (cursor < scroll) scroll = cursor;
        else if (cursor >= scroll + visible) scroll = cursor - visible + 1;
        if (scroll < 0) scroll = 0;
    }

    void draw_session_list(int start_y, int end_y, int w) {
        const int checkbox_w = 3, date_w = 13, project_w = 20, msgs_w = 6, col_gap = 1;
        if (view.empty()) {
            wattrset(stdscr, COLOR_PAIR(COLOR_DIM));
            const char* why = sessions.empty()
                                  ? "No sessions found."
                                  : (filter.empty() ? "No sessions match the current filter."
                                                    : "No session matches the filter.");
            mvwaddstr(stdscr, start_y + 1, 2, why);
            return;
        }
        int visible = end_y - start_y;
        if (visible < 1) visible = 1;
        adjust_scroll(visible);

        int title_w = w - checkbox_w - col_gap - date_w - col_gap - project_w - col_gap - msgs_w - 2;
        if (title_w < 1) title_w = 1;

        for (int row = scroll; row < std::min((int)view.size(), scroll + visible); ++row) {
            int y = start_y + (row - scroll);
            if (y >= end_y) break;
            Session& s = sessions[(size_t)view[(size_t)row]];
            bool is_sel = selected.count(s.id) > 0;

            attr_t attr = is_sel ? COLOR_PAIR(COLOR_SELECTED) : COLOR_PAIR(COLOR_DIM);
            if (row == cursor) attr |= A_REVERSE;
            wattrset(stdscr, attr);

            mvwaddswstr(stdscr, y, 0, is_sel ? L"[x]" : L"[ ]");

            wstring title_text = truncate(s.title.empty() ? L"(untitled)" : s.title, (size_t)title_w);
            mvwaddswstr(stdscr, y, checkbox_w + col_gap, ljust(title_text, (size_t)title_w));

            int off = checkbox_w + col_gap + title_w + col_gap;
            mvwaddswstr(stdscr, y, off, ljust(format_time(s.time_created), (size_t)date_w));

            string proj = s.project_name.empty() ? s.worktree : s.project_name;
            if (proj.empty()) proj = "-";
            off += date_w + col_gap;
            mvwaddswstr(stdscr, y, off,
                        ljust(truncate(utf8_to_wide(proj), (size_t)project_w), (size_t)project_w));

            off += project_w + col_gap;
            mvwaddswstr(stdscr, y, off, rjust(std::to_wstring(s.msg_count), (size_t)msgs_w));
        }
    }

    void draw_snapshot_list(int start_y, int end_y, int w) {
        const int tag_w = 12, size_w = 8, name_w = 25, col_gap = 1;
        if (view.empty()) {
            wattrset(stdscr, COLOR_PAIR(COLOR_DIM));
            const char* why = snapshots.empty() ? "No snapshots found."
                                                : "No snapshot matches the current filter.";
            mvwaddstr(stdscr, start_y + 1, 2, why);
            return;
        }
        int visible = end_y - start_y;
        if (visible < 1) visible = 1;
        adjust_scroll(visible);

        int path_w = w - tag_w - col_gap - size_w - col_gap - name_w - col_gap;
        if (path_w < 1) path_w = 1;

        for (int row = scroll; row < std::min((int)view.size(), scroll + visible); ++row) {
            int y = start_y + (row - scroll);
            if (y >= end_y) break;
            Snapshot& s = snapshots[(size_t)view[(size_t)row]];
            bool is_orphan = !s.active_sessions;
            bool is_sel = selected.count(s.path) > 0;

            attr_t attr;
            if (is_sel) attr = COLOR_PAIR(COLOR_SELECTED);
            else if (is_orphan) attr = COLOR_PAIR(COLOR_WARN);
            else attr = COLOR_PAIR(COLOR_DIM);
            if (row == cursor) attr |= A_REVERSE;
            wattrset(stdscr, attr);

            wstring checkbox = is_sel ? L"[x]" : L"[ ]";
            wstring tag = is_orphan ? L"[ORPHAN] " : L"[ACTIVE] ";
            mvwaddswstr(stdscr, y, 0, ljust(checkbox + tag, (size_t)tag_w));

            int off = tag_w + col_gap;
            mvwaddswstr(stdscr, y, off,
                        ljust(truncate(utf8_to_wide(s.worktree), (size_t)path_w), (size_t)path_w));

            off += path_w + col_gap;
            mvwaddswstr(stdscr, y, off,
                        rjust(s.size_known ? format_size(s.size) : wstring(L"..."),
                              (size_t)size_w));

            off += size_w + col_gap;
            mvwaddswstr(stdscr, y, off, ljust(truncate(s.name, (size_t)name_w), (size_t)name_w));
        }
    }

    string describe_key(const string& key) const {
        return tab == TAB_SESSIONS ? "session " + utf8_ellipsize(key, 24)
                                   : utf8_ellipsize(key, 52);
    }

    void draw_confirm_dialog(int h, int w) {
        int selected_count = (int)selected.size();
        long long total_size = 0;
        std::vector<string> listing(selected.begin(), selected.end());
        if (tab == TAB_SNAPSHOTS) {
            for (auto& s : snapshots)
                if (selected.count(s.path)) total_size += s.size;
        }

        const size_t max_listed = 6;
        int listed = (int)std::min(max_listed, listing.size());

        int dialog_w = 62;
        if (dialog_w > w - 2) dialog_w = w - 2;
        if (dialog_w < 24) dialog_w = 24;
        int dialog_h = 6 + listed + (listing.size() > max_listed ? 1 : 0);
        if (dialog_h > h - 2) dialog_h = h - 2;

        int dy = (h - dialog_h) / 2;
        int dx = (w - dialog_w) / 2;
        if (dy < 0) dy = 0;
        if (dx < 0) dx = 0;

        auto put = [&](int row, const wstring& text) {
            if (row < 0 || row >= dialog_h) return;
            mvwaddswstr(stdscr, dy + row, dx + 2, truncate(text, (size_t)dialog_w - 4));
        };

        wattrset(stdscr, COLOR_PAIR(COLOR_WARN) | A_BOLD);
        for (int y = dy; y < dy + dialog_h && y < h; y++)
            mvwaddswstr(stdscr, y, dx, wstring((size_t)dialog_w, L' '));
        mvwaddswstr(stdscr, dy, dx, L"\u250c" + wstring((size_t)dialog_w - 2, L'\u2500') + L"\u2510");
        mvwaddswstr(stdscr, dy + dialog_h - 1, dx,
                    L"\u2514" + wstring((size_t)dialog_w - 2, L'\u2500') + L"\u2518");
        for (int y = dy + 1; y < dy + dialog_h - 1 && y < h; y++) {
            mvwaddswstr(stdscr, y, dx, L"\u2502");
            mvwaddswstr(stdscr, y, dx + dialog_w - 1, L"\u2502");
        }

        wchar_t buf[128];
        swprintf(buf, 128, L" Remove %d item(s)?", selected_count);
        put(1, buf);

        wstring detail = tab == TAB_SNAPSHOTS
                             ? L"This permanently removes these snapshot folders ("
                                   + format_size(total_size) + L"):"
                             : L"These sessions and their messages are removed:";
        put(2, detail);

        for (int i = 0; i < listed; i++)
            put(3 + i, L"  - " + utf8_to_wide(describe_key(listing[(size_t)i])));
        if ((int)listing.size() > listed)
            put(3 + listed, L"  ... and " + std::to_wstring(listing.size() - (size_t)listed) +
                                L" more");

        put(dialog_h - 3, delete_to_recycle()
                              ? L" -> goes to the Recycle Bin (recoverable)"
                              : L" -> permanently deleted, this cannot be undone");
        put(dialog_h - 2, L"[Y] Confirm    [N] Cancel");
        wattrset(stdscr, A_NORMAL);
    }

    void draw_action_prompt(int h, int w) {
        const wchar_t* text = L"";
        switch (pending_action) {
        case MA_CLEAN_ORPHANS:
            text = L" Remove event rows of deleted sessions?  [Y] Yes  [N] Cancel ";
            break;
        case MA_COMPACT:
            text = L" Compact the event log (backup first, then drop old versions)?  "
                   L"[Y] Yes  [N] Cancel ";
            break;
        case MA_VACUUM:
            text = L" Rewrite the database file to reclaim free pages?  [Y] Yes  [N] Cancel ";
            break;
        default:
            return;
        }
        int y = h - 2;
        if (y < 0 || y >= LINES) return;
        wattrset(stdscr, COLOR_PAIR(COLOR_WARN) | A_BOLD);
        mvwaddswstr(stdscr, y, 0, truncate(text, (size_t)w));
        wattrset(stdscr, A_NORMAL);
    }

    void draw_preview_overlay(int h, int w) {
        int box_h = h - 4;
        int box_w = w - 4;
        if (box_h < 4 || box_w < 20) return;
        const int dy = 2, dx = 2;

        wattrset(stdscr, COLOR_PAIR(COLOR_NORMAL));
        for (int y = 0; y < box_h && dy + y < h; y++)
            mvwaddswstr(stdscr, dy + y, dx, wstring((size_t)box_w, L' '));

        wstring title = L" Preview: " + utf8_to_wide(preview_for) + L"   (" +
                        std::to_wstring(preview.size()) +
                        L" part(s))  -  j/k scroll, p/Esc close ";
        wattrset(stdscr, COLOR_PAIR(COLOR_HEADER) | A_BOLD);
        mvwaddswstr(stdscr, dy, dx, truncate(title, (size_t)box_w));

        std::vector<wstring> lines;
        if (!preview_error.empty()) lines.push_back(L"  Error: " + utf8_to_wide(preview_error));
        for (auto& m : preview) {
            lines.push_back(L"  [" + (m.role.empty() ? utf8_to_wide(m.kind) : utf8_to_wide(m.role)) +
                            L"]  " + format_time(m.time_created));
            wstring text = utf8_to_wide(m.text);
            size_t per = (size_t)std::max(20, box_w - 8);
            for (size_t i = 0; i < text.size(); i += per)
                lines.push_back(L"      " + text.substr(i, per));
        }
        if (lines.empty()) lines.push_back(L"  (no parts recorded for this session)");

        int visible = box_h - 1;
        int max_scroll = (int)lines.size() - visible;
        if (max_scroll < 0) max_scroll = 0;
        if (preview_scroll > max_scroll) preview_scroll = max_scroll;
        if (preview_scroll < 0) preview_scroll = 0;

        wattrset(stdscr, COLOR_PAIR(COLOR_DIM));
        for (int i = 0; i < visible && (size_t)(preview_scroll + i) < lines.size(); ++i)
            mvwaddswstr(stdscr, dy + 1 + i, dx,
                        truncate(lines[(size_t)(preview_scroll + i)], (size_t)box_w - 1));
        wattrset(stdscr, A_NORMAL);
    }
};

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------
// Headless modes. They render the very same pipeline the interactive UI uses
// (load -> filter -> sort -> format) plus the database maintenance operations,
// which makes the tool scriptable and means everything can be exercised and
// verified without a console.
struct CliOptions {
    // listing
    bool list = false;
    bool snapshots = false;
    bool orphans_only = false;
    string filter;
    SortField field = SORT_CREATED;
    bool field_given = false;
    bool ascending = false;
    long long limit = 0;             // 0 = no limit
    long long older_than_days = 0;   // 0 = no age bound
    long long max_messages = -1;     // -1 = no bound
    // maintenance
    bool db_report = false;
    bool clean_orphans = false;
    bool compact_events = false;
    bool vacuum = false;
    bool preview = false;
    bool export_json = false;
    bool yes = false;                // required to actually modify anything
    string backup_path;
    string preview_id;
    string export_dir;
};

static bool parse_sort_field(const string& name, SortField* out) {
    for (int i = 0; i < SORT_FIELD_COUNT; ++i) {
        SortField f = (SortField)i;
        if (name == sort_field_name(f)) {
            *out = f;
            return true;
        }
    }
    return false;
}

static void print_usage() {
    printf("opencode-session-manager " OPENCODE_SM_VERSION_STRING " [options]\n"
           "\n"
           "Interactive TUI (no options opens the UI):\n"
           "  --cleanup            also clean up orphan session_diff files on exit\n"
           "  --no-recycle         delete permanently instead of using the Recycle Bin\n"
           "\n"
           "Headless listing (same filter/sort pipeline as the UI):\n"
           "  --list               print the current view and exit\n"
           "  --snapshots          list snapshot directories instead of sessions\n"
           "  --orphans            --snapshots: only directories no session references\n"
           "  --filter <text>      match id / title / project / worktree (or path / name)\n"
           "  --sort <field>       created|updated|msgs|size|status|id|name|project\n"
           "  --asc                sort ascending (default: descending)\n"
           "  --limit <n>          print at most n rows\n"
           "  --older-than <days>  sessions not touched for that many days\n"
           "  --max-messages <n>   sessions with at most n messages\n"
           "\n"
           "Database maintenance (read-only unless --yes is given):\n"
           "  --db-report          show where the disk space goes, then exit\n"
           "  --preview <id>       print the tail of a session, then exit\n"
           "  --export <dir>       export the listed sessions as JSON\n"
           "  --clean-orphan-events  drop event rows of deleted sessions\n"
           "  --compact-events     keep only the newest snapshot of each part\n"
           "  --vacuum             rewrite the database file to reclaim free pages\n"
           "  --backup <path>      --compact-events: back up here before changing\n"
           "  --yes                actually perform the change (otherwise: dry run)\n"
           "\n"
           "  --version            print the version and exit\n"
           "  --help               show this message\n");
}

static void cli_progress(int percent, const std::string& stage) {
    printf("\r  [%3d%%] %-64s", percent, utf8_ellipsize(stage, 64).c_str());
    fflush(stdout);
    if (percent >= 100) printf("\n");
}

static int run_list(const CliOptions& o) {
    ViewQuery q;
    q.filter = o.filter;
    q.orphans_only = o.orphans_only;
    q.descending = !o.ascending;
    q.max_messages = o.max_messages;
    if (o.older_than_days > 0) {
        time_t now = time(nullptr);
        q.created_before_ms = (long long)(now - (time_t)o.older_than_days * 24 * 3600) * 1000;
    }

    if (!o.snapshots) {
        q.field = o.field_given ? o.field : SORT_CREATED;
        std::vector<Session> sessions = load_sessions(DB_PATH);
        std::vector<int> view = build_session_view(sessions, q);

        // --export writes exactly the rows the listing would have shown
        if (o.export_json) {
            std::vector<string> ids;
            for (int idx : view) ids.push_back(sessions[(size_t)idx].id);
            if (ids.empty()) {
                printf("Nothing to export.\n");
                return 0;
            }
            std::vector<string> written;
            string err;
            if (!export_sessions_json(DB_PATH, ids, o.export_dir, &written, &err)) {
                fprintf(stderr, "Export failed: %s\n", err.c_str());
                return 1;
            }
            printf("Exported %d session(s) to %s\n", (int)written.size(), o.export_dir.c_str());
            for (auto& w : written) printf("  %s\n", w.c_str());
            return 0;
        }

        long long shown = 0;
        for (int idx : view) {
            if (o.limit > 0 && shown >= o.limit) break;
            const Session& s = sessions[(size_t)idx];
            string proj = s.project_name.empty() ? s.worktree : s.project_name;
            // Note the explicit casts: MinGW's printf cannot print %lld.
            printf("%-36s %6d  %-12s  %-28s  %s\n", s.id.c_str(), (int)s.msg_count,
                   wide_to_utf8(format_time(s.time_created)).c_str(),
                   proj.empty() ? "-" : utf8_ellipsize(proj, 28).c_str(),
                   s.title.empty() ? "(untitled)" : utf8_ellipsize(wide_to_utf8(s.title), 60).c_str());
            ++shown;
        }
        printf("%d of %d session(s) shown   (sort: %s %s)\n", (int)shown, (int)view.size(),
               sort_field_name(q.field), q.descending ? "desc" : "asc");
        return 0;
    }

    q.field = o.field_given ? o.field : SORT_SIZE;
    std::vector<Snapshot> snaps = load_snapshots(DB_PATH, SNAPSHOT_DIR, /*with_sizes=*/true);
    std::vector<int> view = build_snapshot_view(snaps, q);

    long long shown = 0, total_bytes = 0;
    for (int idx : view) {
        const Snapshot& s = snaps[(size_t)idx];
        total_bytes += s.size;
        if (o.limit > 0 && shown >= o.limit) continue;
        printf("%-9s %-9s  %-44s  %s\n", s.active_sessions ? "in use" : "orphan",
               wide_to_utf8(format_size(s.size)).c_str(),
               utf8_ellipsize(s.worktree, 44).c_str(),
               wide_to_utf8(s.name).c_str());
        ++shown;
    }
    printf("%d of %d snapshot(s) shown, %s in total   (sort: %s %s)\n", (int)shown, (int)view.size(),
           wide_to_utf8(format_size(total_bytes)).c_str(), sort_field_name(q.field),
           q.descending ? "desc" : "asc");
    return 0;
}

static int run_db_report() {
    DbReport r = analyze_database(DB_PATH, 12, cli_progress);
    printf("\n");
    printf("database file   : %s  (%d pages x %d bytes)\n",
           wide_to_utf8(format_size(r.file_bytes)).c_str(), (int)r.page_count, (int)r.page_size);
    printf("  free pages    : %s   <- reclaimable by VACUUM\n",
           wide_to_utf8(format_size(r.free_bytes)).c_str());
    printf("  rows          : session %d, message %d, part %d, event %d\n", (int)r.session_rows,
           (int)r.message_rows, (int)r.part_rows, (int)r.event_rows);
    printf("\nlargest tables (%s):\n",
           r.dbstat_available ? "exact, from dbstat" : "estimated from content bytes");
    for (auto& t : r.tables)
        printf("  %-28s %12s\n", t.name.c_str(), wide_to_utf8(format_size(t.bytes)).c_str());
    printf("\nevent log %s (%d rows) by type:\n", wide_to_utf8(format_size(r.event_bytes)).c_str(),
           (int)r.event_rows);
    for (auto& t : r.event_types)
        printf("  %-28s %8d rows %12s\n", t.name.c_str(), (int)t.rows,
               wide_to_utf8(format_size(t.bytes)).c_str());
    printf("\ntop sessions by event footprint:\n");
    for (auto& t : r.sessions)
        printf("  %-32s %8d rows %12s   %s\n", utf8_ellipsize(t.name, 32).c_str(), (int)t.rows,
               wide_to_utf8(format_size(t.bytes)).c_str(), t.extra.c_str());
    printf("\nreclaimable:\n");
    printf("  orphan events (deleted sessions) %8d rows %12s   <- safe: --clean-orphan-events\n",
           (int)r.orphan_event_rows, wide_to_utf8(format_size(r.orphan_event_bytes)).c_str());
    printf("  free pages                               %12s   <- --vacuum\n",
           wide_to_utf8(format_size(r.free_bytes)).c_str());
    printf("  total safe to reclaim                    %12s\n",
           wide_to_utf8(format_size(r.reclaimable_safe())).c_str());
    if (!r.big_items.empty()) {
        printf("\noversized objects worth a look:\n");
        for (auto& t : r.big_items)
            printf("  %-58s %12s\n", utf8_ellipsize(t.name, 58).c_str(),
                   wide_to_utf8(format_size(t.bytes)).c_str());
    }
    printf("\nhint: --compact-events reclaims much more (dry run unless you add --yes)\n");
    return 0;
}

static int run_preview(const CliOptions& o) {
    auto msgs = load_session_preview(DB_PATH, o.preview_id, 20);
    if (msgs.empty()) {
        printf("No parts found for %s\n", o.preview_id.c_str());
        return 1;
    }
    printf("Last %d part(s) of session %s:\n\n", (int)msgs.size(), o.preview_id.c_str());
    for (auto& m : msgs) {
        printf("  [%s] %s\n", m.role.empty() ? m.kind.c_str() : m.role.c_str(),
               wide_to_utf8(format_time(m.time_created)).c_str());
        printf("      %s\n", m.text.c_str());
    }
    return 0;
}

static int run_clean_orphans(const CliOptions& o) {
    if (!o.yes) {
        printf("Dry run (add --yes to apply).\n\n");
        DbReport r = analyze_database(DB_PATH, 3, cli_progress);
        printf("\nwould remove %d orphan event rows, about %s (they belong to sessions that are gone)\n",
               (int)r.orphan_event_rows, wide_to_utf8(format_size(r.orphan_event_bytes)).c_str());
        printf("The file only shrinks after a --vacuum.\n");
        return 0;
    }
    ReclaimResult r = delete_orphan_events(DB_PATH, cli_progress);
    printf("\nremoved %d rows, about %s\n", (int)r.rows,
           wide_to_utf8(format_size(r.bytes)).c_str());
    printf("file size: %s -> %s\n", wide_to_utf8(format_size(r.file_before)).c_str(),
           wide_to_utf8(format_size(r.file_after)).c_str());
    return 0;
}

static int run_compact(const CliOptions& o) {
    CompactionPlan plan = plan_event_compaction(DB_PATH, cli_progress);
    printf("\n");
    printf("message.part.updated.1: %d rows / %s across %d parts\n", (int)plan.total_rows,
           wide_to_utf8(format_size(plan.total_bytes)).c_str(), (int)plan.parts);
    printf("redundant versions    : %d rows / %s\n", (int)plan.removable_rows,
           wide_to_utf8(format_size(plan.removable_bytes)).c_str());
    if (plan.skipped_rows)
        printf("skipped (unreadable)  : %d rows (never deleted)\n", (int)plan.skipped_rows);
    if (!plan.worst_parts.empty()) {
        printf("\nparts with the most versions:\n");
        for (auto& p : plan.worst_parts)
            printf("  %-34s %8d versions %12s\n", p.name.c_str(), (int)p.rows,
                   wide_to_utf8(format_size(p.bytes)).c_str());
    }
    if (!o.yes) {
        printf("\nDry run: nothing was modified.\n");
        printf("To apply: --compact-events --yes --backup <file>\n");
        printf("Note: this rewrites the opencode event log. Back it up first.\n");
        return 0;
    }
    if (o.backup_path.empty()) {
        fprintf(stderr,
                "--compact-events --yes refuses to run without --backup <file>:\n"
                "it rewrites the opencode event log.\n");
        return 2;
    }
    printf("\nBacking up to %s ...\n", o.backup_path.c_str());
    long long copied = 0;
    string err;
    if (!backup_database(DB_PATH, o.backup_path, &copied, &err)) {
        fprintf(stderr, "Backup failed: %s\n", err.c_str());
        return 1;
    }
    printf("Backup complete (%s)\n\n", wide_to_utf8(format_size(copied)).c_str());
    ReclaimResult r = apply_event_compaction(DB_PATH, plan, cli_progress);
    printf("\nremoved %d rows, about %s\n", (int)r.rows,
           wide_to_utf8(format_size(r.bytes)).c_str());
    printf("file size: %s -> %s (run --vacuum to release it)\n",
           wide_to_utf8(format_size(r.file_before)).c_str(),
           wide_to_utf8(format_size(r.file_after)).c_str());
    return 0;
}

static int run_vacuum(const CliOptions& o) {
    if (!o.yes) {
        DbReport r = analyze_database(DB_PATH, 3, cli_progress);
        printf("\nDry run: %s of free pages can be reclaimed.\n",
               wide_to_utf8(format_size(r.free_bytes)).c_str());
        printf("VACUUM needs temp space about the size of the database and takes a few minutes.\n");
        printf("To apply: --vacuum --yes (it fails while opencode is running)\n");
        return 0;
    }
    if (database_is_busy(DB_PATH)) {
        fprintf(stderr, "The database is locked (opencode is probably running) - close it first.\n");
        return 1;
    }
    ReclaimResult r;
    if (!vacuum_database(DB_PATH, &r, cli_progress)) {
        fprintf(stderr, "VACUUM failed: %s\n", r.error.c_str());
        return 1;
    }
    printf("\nfile size: %s -> %s (freed %s)\n", wide_to_utf8(format_size(r.file_before)).c_str(),
           wide_to_utf8(format_size(r.file_after)).c_str(),
           wide_to_utf8(format_size(r.bytes)).c_str());
    return 0;
}

int main(int argc, char** argv) {
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
#endif
    bool cleanup_on_exit = false;
    CliOptions opt;

    for (int i = 1; i < argc; i++) {
        string a = argv[i];
        auto need_value = [&](const char* what) -> const char* {
            if (i + 1 >= argc) {
                fprintf(stderr, "Option %s needs a value.\n", what);
                return nullptr;
            }
            return argv[++i];
        };

        if (a == "--list") opt.list = true;
        else if (a == "--snapshots") opt.snapshots = true;
        else if (a == "--orphans") opt.orphans_only = true;
        else if (a == "--asc") opt.ascending = true;
        else if (a == "--yes") opt.yes = true;
        else if (a == "--cleanup") cleanup_on_exit = true;
        else if (a == "--no-recycle") set_delete_to_recycle(false);
        else if (a == "--db-report") opt.db_report = true;
        else if (a == "--clean-orphan-events") opt.clean_orphans = true;
        else if (a == "--compact-events") opt.compact_events = true;
        else if (a == "--vacuum") opt.vacuum = true;
        else if (a == "--filter") {
            const char* v = need_value("--filter");
            if (!v) return 2;
            opt.filter = v;
        } else if (a == "--limit") {
            const char* v = need_value("--limit");
            if (!v) return 2;
            opt.limit = atoll(v);
        } else if (a == "--older-than") {
            const char* v = need_value("--older-than");
            if (!v) return 2;
            opt.older_than_days = atoll(v);
        } else if (a == "--max-messages") {
            const char* v = need_value("--max-messages");
            if (!v) return 2;
            opt.max_messages = atoll(v);
        } else if (a == "--backup") {
            const char* v = need_value("--backup");
            if (!v) return 2;
            opt.backup_path = v;
        } else if (a == "--preview") {
            const char* v = need_value("--preview");
            if (!v) return 2;
            opt.preview = true;
            opt.preview_id = v;
        } else if (a == "--export") {
            const char* v = need_value("--export");
            if (!v) return 2;
            opt.export_json = true;
            opt.export_dir = v;
        } else if (a == "--sort") {
            const char* v = need_value("--sort");
            if (!v) return 2;
            if (!parse_sort_field(v, &opt.field)) {
                fprintf(stderr, "Unknown sort field: %s\n", v);
                return 2;
            }
            opt.field_given = true;
        } else if (a == "--version" || a == "-V") {
            printf("opencode-session-manager %s\n", OPENCODE_SM_VERSION_STRING);
            return 0;
        } else if (a == "--help" || a == "-h") {
            print_usage();
            return 0;
        } else {
            fprintf(stderr, "Unknown option: %s\n", a.c_str());
            print_usage();
            return 2;
        }
    }

    if (!path_exists(DB_PATH)) {
        fprintf(stderr, "Database not found: %s\n", DB_PATH.c_str());
        fprintf(stderr, "Set OPENCODE_DATA_DIR to point at another data directory.\n");
        return 1;
    }

    // Headless modes, ordered from least to most destructive.
    if (opt.list || opt.export_json || opt.preview || opt.db_report || opt.clean_orphans ||
        opt.compact_events || opt.vacuum) {
        try {
            if (opt.preview) return run_preview(opt);
            if (opt.db_report) return run_db_report();
            if (opt.clean_orphans) return run_clean_orphans(opt);
            if (opt.compact_events) return run_compact(opt);
            if (opt.vacuum) return run_vacuum(opt);
            return run_list(opt);
        } catch (const std::exception& e) {
            fprintf(stderr, "\nError: %s\n", e.what());
            return 1;
        }
    }

    initscr();
    try {
        App app;
        app.cleanup_on_exit = cleanup_on_exit;
        app.run();
    } catch (const std::exception&) {
        return 1;
    }

    // Opt-in only: silently rewriting the data directory on exit is not
    // something a cleanup tool should do behind the user's back.
    if (cleanup_on_exit) {
        try {
            std::vector<string> failed;
            auto orphans = cleanup_orphan_diffs(DB_PATH, DIFF_DIR, &failed);
            if (!orphans.empty())
                printf("Cleaned up %d orphan diff file(s)%s.\n", (int)orphans.size(),
                       delete_to_recycle() ? " (moved to the Recycle Bin)" : "");
            if (!failed.empty())
                printf("%d orphan diff file(s) could not be removed.\n", (int)failed.size());
        } catch (const std::exception& e) {
            printf("Orphan diff cleanup failed: %s\n", e.what());
        }
    }
    return 0;
}
