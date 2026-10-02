// OpenCode Session Manager - GUI (Dear ImGui + Win32 + OpenGL3)
//
// Browse old OpenCode sessions & snapshots, copy the resume command, jump into
// a session, and reclaim disk space -- without ever blocking the UI thread.
//
// No bundled DLLs: uses only Windows system libraries (opengl32.dll etc.).
// Build (MinGW-w64 + sqlite3 from C:\MinGW\opt):
//   g++ -std=c++17 -O2 -I imgui -I imgui/backends
//        -I "C:\MinGW\opt\include"
//        main_gui.cpp opencode_data.cpp
//        imgui/imgui.cpp imgui/imgui_draw.cpp imgui/imgui_tables.cpp
//        imgui/imgui_widgets.cpp
//        imgui/backends/imgui_impl_win32.cpp imgui/backends/imgui_impl_opengl3.cpp
//        -L "C:\MinGW\opt\lib" -lsqlite3 -lopengl32 -ldwmapi -lshell32 -lole32
//        -mwindows -o opencode-session-manager-gui.exe
//
// Optional env overrides (handy for testing):
//   OPENCODE_DATA_DIR=<dir containing opencode.db, storage/, snapshot/>
//   OPENCODE_SM_NO_RECYCLE=1   -> delete permanently instead of using the bin

#define WIN32_LEAN_AND_MEAN 1
// WM_DPICHANGED needs the Windows 8.1 SDK level.
#ifndef WINVER
#define WINVER 0x0603
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0603
#endif

#include <windows.h>

#include "imgui.h"
#include "imgui_impl_win32.h"
#include "imgui_impl_opengl3.h"

#include <GL/gl.h>
#include <algorithm>
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
// Helpers
// ---------------------------------------------------------------------------
static const string GUI_DB_PATH = db_path();
static const string GUI_DIFF_DIR = diff_dir();
static const string GUI_SNAPSHOT_DIR = snapshot_dir();

static string u8(const wstring& w) { return wide_to_utf8(w); }
static string u8fmt_time(long long ts) { return u8(format_time(ts)); }
static string u8fmt_size(long long sz) { return u8(format_size(sz)); }

// ---------------------------------------------------------------------------
// Application state
// ---------------------------------------------------------------------------
static const int TAB_SESSIONS = 0;
static const int TAB_SNAPSHOTS = 1;
static const int TAB_DATABASE = 2;

// How many snapshot sizes we walk per frame. Sizing a snapshot directory means
// recursing the whole tree; doing all of them in one go is what used to freeze
// the window on every refresh.
static const int SIZE_SCAN_PER_FRAME = 6;

static int g_tab = TAB_SESSIONS;
static std::vector<Session> g_sessions;
static std::vector<Snapshot> g_snapshots;
static std::set<string> g_selected;            // session id, or snapshot path
static std::vector<int> g_view_sessions;       // indices into g_sessions, filtered+sorted
static std::vector<int> g_view_snapshots;

static char g_filter[128] = "";
static string g_filter_prev;
static bool g_orphans_only = false;
static bool g_orphans_only_prev = false;

static int g_sort_col[2] = {5, 3};             // sessions: Created, snapshots: Size
static bool g_sort_asc[2] = {false, false};

static bool g_view_dirty = true;
static size_t g_size_scan_next = 0;
static size_t g_size_scan_pending = 0;

static string g_message, g_error, g_toast;
static double g_toast_expire = 0.0;
static bool g_cleanup_on_exit = false;
static bool g_confirm_request = false;
static bool g_confirm_open = false;
static bool g_focus_filter = false;

// ---------------------------------------------------------------------------
// Rule based selection, session preview, and database maintenance state
// ---------------------------------------------------------------------------
static int g_older_days_i = 0;        // 0 = no age bound
static int g_max_msgs_i = -1;         // -1 = no bound
static bool g_show_preview = false;
static std::vector<PreviewMessage> g_preview;
static string g_preview_for;          // session id the preview belongs to
static string g_preview_error;

static MaintenanceJob g_job;
static bool g_job_seen = false;       // a finished job has been reported already

// A maintenance action waiting for confirmation.
static int g_maint_action = 0;
static bool g_maint_modal = false;
static bool g_maint_keep_backup = false;
static string g_maint_backup_path;

enum { MA_NONE = 0, MA_CLEAN_ORPHANS, MA_COMPACT, MA_VACUUM, MA_BACKUP };

static string timestamped_backup_path() {
    time_t now = time(nullptr);
    struct tm tmv;
#ifdef _WIN32
    localtime_s(&tmv, &now);
#else
    localtime_r(&now, &tmv);
#endif
    char buf[64];
    snprintf(buf, sizeof(buf), "%04d%02d%02d-%02d%02d%02d", tmv.tm_year + 1900, tmv.tm_mon + 1,
             tmv.tm_mday, tmv.tm_hour, tmv.tm_min, tmv.tm_sec);
    return GUI_DB_PATH + ".backup-" + buf;
}

// Keeps the preview panel in sync with the (single) selected session.
static void sync_preview() {
    if (!g_show_preview || g_tab != TAB_SESSIONS) {
        g_preview.clear();
        g_preview_for.clear();
        return;
    }
    string want;
    if (g_selected.size() == 1) want = *g_selected.begin();
    if (want == g_preview_for) return;

    g_preview.clear();
    g_preview_error.clear();
    g_preview_for = want;
    if (want.empty()) return;
    try {
        g_preview = load_session_preview(GUI_DB_PATH, want, 12);
    } catch (const std::exception& e) {
        g_preview_error = e.what();
    }
}

static void start_maintenance(int action) {
    switch (action) {
    case MA_CLEAN_ORPHANS: g_job.start(JobType::CleanupOrphans, GUI_DB_PATH); break;
    case MA_COMPACT: g_job.start(JobType::CompactEvents, GUI_DB_PATH, g_maint_backup_path); break;
    case MA_VACUUM: g_job.start(JobType::Vacuum, GUI_DB_PATH); break;
    case MA_BACKUP: g_job.start(JobType::Backup, GUI_DB_PATH, g_maint_backup_path); break;
    default: break;
    }
    g_job_seen = false;
}

// ---------------------------------------------------------------------------
// Text size
//
// Dear ImGui 1.92 computes GetFontSize() as
//     style.FontSizeBase * style.FontScaleMain * style.FontScaleDpi
// The previous build handed an already-DPI-scaled pixel size to the font atlas
// *and* set style.FontScaleDpi, so every glyph rendered at scale^2: on a 150%
// display that is 16 * 1.5 * 1.5 = 36px instead of 24px. The bug is having two
// factors, so the fix is to apply the monitor scale exactly once -- but doing
// only that visibly shrinks the text people are used to, so the size is now a
// real setting: Ctrl+Minus / Ctrl+Plus / Ctrl+0, persisted, and defaulting to
// the size this app has always rendered at.
// ---------------------------------------------------------------------------
static const float kStandardTextPx = 16.0f;   // the size a 100% display uses
static float g_dpi_scale = 1.0f;
static float g_text_px = 0.0f;                // logical text size; 0 = not resolved yet
static bool g_text_px_dirty = false;          // has an unsaved change

static string settings_file_path() {
    const char* appdata = getenv("APPDATA");
    if (!appdata || !*appdata) return "";
    return path_join(path_join(appdata, "opencode-session-manager"), "settings.ini");
}

// %APPDATA% may contain non-ASCII characters, so never use plain fopen().
static FILE* open_file_utf8(const string& path, const wchar_t* mode) {
    return _wfopen(utf8_to_wide(path).c_str(), mode);
}

static void load_settings() {
    g_text_px = 0.0f;
    string f = settings_file_path();
    if (f.empty() || !is_file(f)) return;
    FILE* fp = open_file_utf8(f, L"rb");
    if (!fp) return;
    char line[256];
    while (fgets(line, sizeof(line), fp)) {
        float v = 0.0f;
        if (sscanf(line, "text_px=%f", &v) == 1 && v >= 10.0f && v <= 48.0f) g_text_px = v;
    }
    fclose(fp);
}

static void save_settings() {
    string f = settings_file_path();
    if (f.empty()) return;
    size_t slash = f.find_last_of("/\\");
    if (slash != string::npos) make_dir(f.substr(0, slash));
    FILE* fp = open_file_utf8(f, L"wb");
    if (!fp) return;
    fprintf(fp, "text_px=%.0f\n", g_text_px);
    fclose(fp);
    g_text_px_dirty = false;
}

// The size every previous build rendered at (they applied the DPI factor twice).
static float default_text_px() { return kStandardTextPx * g_dpi_scale; }

static void set_text_px(float px) {
    if (px < 10.0f) px = 10.0f;
    if (px > 48.0f) px = 48.0f;
    if (px == g_text_px) return;
    g_text_px = px;
    g_text_px_dirty = true;   // picked up by the next apply_dpi_scale() call
}

static size_t total_selected_size() {
    long long total = 0;
    for (auto& s : g_snapshots)
        if (g_selected.count(s.path)) total += s.size;
    return (size_t)total;
}

// ---------------------------------------------------------------------------
// View building: filter -> sort, shared with the TUI (session_view.cpp) so the
// two front ends cannot drift apart. Sorting is an index permutation rather than
// a reordering of the underlying vectors, so row identity -- and therefore the
// selection, which is keyed by id/path -- never moves.
// ---------------------------------------------------------------------------
static SortField session_sort_field(int column) {
    switch (column) {
    case 1: return SORT_ID;
    case 2: return SORT_NAME;
    case 3: return SORT_LOCATION;
    case 4: return SORT_MESSAGES;
    case 6: return SORT_UPDATED;
    case 5:
    default: return SORT_CREATED;
    }
}

static SortField snapshot_sort_field(int column) {
    switch (column) {
    case 1: return SORT_STATUS;
    case 2: return SORT_LOCATION;
    case 4: return SORT_NAME;
    case 3:
    default: return SORT_SIZE;
    }
}

static void rebuild_views() {
    // Rule based bounds shared by both views (they only apply to sessions).
    long long created_before = 0;
    if (g_older_days_i > 0) {
        time_t now = time(nullptr);
        created_before = (long long)(now - (time_t)g_older_days_i * 24 * 3600) * 1000;
    }

    ViewQuery sq;
    sq.filter = g_filter;
    sq.field = session_sort_field(g_sort_col[TAB_SESSIONS]);
    sq.descending = !g_sort_asc[TAB_SESSIONS];
    sq.created_before_ms = created_before;
    sq.max_messages = g_max_msgs_i;
    g_view_sessions = build_session_view(g_sessions, sq);

    ViewQuery pq;
    pq.filter = g_filter;
    pq.orphans_only = g_orphans_only;
    pq.field = snapshot_sort_field(g_sort_col[TAB_SNAPSHOTS]);
    pq.descending = !g_sort_asc[TAB_SNAPSHOTS];
    g_view_snapshots = build_snapshot_view(g_snapshots, pq);
}

static void ensure_view() {
    if (!g_view_dirty) return;
    rebuild_views();
    g_view_dirty = false;
}

static size_t view_size() {
    return g_tab == TAB_SESSIONS ? g_view_sessions.size() : g_view_snapshots.size();
}

static string key_of_view_row(int row) {
    if (g_tab == TAB_SESSIONS) {
        if (row < 0 || row >= (int)g_view_sessions.size()) return string();
        return g_sessions[(size_t)g_view_sessions[(size_t)row]].id;
    }
    if (row < 0 || row >= (int)g_view_snapshots.size()) return string();
    return g_snapshots[(size_t)g_view_snapshots[(size_t)row]].path;
}

static void select_all_visible() {
    for (size_t i = 0; i < view_size(); ++i) g_selected.insert(key_of_view_row((int)i));
}

static void clear_selection() { g_selected.clear(); }

// ---------------------------------------------------------------------------
// Data loading
// ---------------------------------------------------------------------------
static void refresh_data(bool keep_view) {
    g_error.clear();
    try {
        g_sessions = load_sessions(GUI_DB_PATH);
        // Sizes are filled in a few per frame by pump_size_scan(), so the
        // window stays interactive no matter how large the snapshot dirs are.
        g_snapshots = load_snapshots(GUI_DB_PATH, GUI_SNAPSHOT_DIR, /*with_sizes=*/false);
        g_size_scan_next = 0;
        g_size_scan_pending = g_snapshots.size();
    } catch (const std::exception& e) {
        g_error = string("Failed to read OpenCode data: ") + e.what();
        g_message = "Error";
        return;
    }

    if (!keep_view) {
        g_selected.clear();
        g_message.clear();
    } else {
        // Drop selections whose row no longer exists.
        std::set<string> alive;
        for (auto& s : g_sessions) alive.insert(s.id);
        for (auto& s : g_snapshots) alive.insert(s.path);
        for (auto it = g_selected.begin(); it != g_selected.end();) {
            if (!alive.count(*it)) it = g_selected.erase(it);
            else ++it;
        }
    }
    g_view_dirty = true;
}

// Computes a few unknown snapshot sizes per frame.
static void pump_size_scan(int budget) {
    const size_t n = g_snapshots.size();
    if (!n || g_size_scan_pending == 0) return;

    int done = 0;
    int scanned = 0;
    while (done < budget && scanned < (int)n) {
        size_t i = g_size_scan_next % n;
        g_size_scan_next = (i + 1) % n;
        ++scanned;
        if (g_snapshots[i].size_known) continue;
        g_snapshots[i].size = total_dir_size(g_snapshots[i].path);
        g_snapshots[i].size_known = true;
        if (g_size_scan_pending) g_size_scan_pending--;
        ++done;
    }
    // Re-sort only once the scan is complete: otherwise rows would shuffle
    // under the cursor while sizes trickle in.
    if (g_size_scan_pending == 0 && g_sort_col[TAB_SNAPSHOTS] == 3) g_view_dirty = true;
}

// ---------------------------------------------------------------------------
// Delete actions
// ---------------------------------------------------------------------------
static void do_delete() {
    if (g_selected.empty()) return;

    if (g_tab == TAB_SESSIONS) {
        std::vector<string> ids(g_selected.begin(), g_selected.end());
        try {
            int n = delete_sessions(GUI_DB_PATH, ids);
            std::vector<string> failed;
            auto orphans = cleanup_orphan_diffs(GUI_DB_PATH, GUI_DIFF_DIR, &failed);
            g_message = "Deleted " + std::to_string(n) + " session(s)";
            if (!orphans.empty())
                g_message += (delete_to_recycle() ? ", moved " : ", removed ") +
                             std::to_string(orphans.size()) + " orphan diff(s)";
            if (!failed.empty())
                g_message += ", " + std::to_string(failed.size()) + " orphan diff(s) failed";
            g_selected.clear();
            refresh_data(true);
        } catch (const std::exception& e) {
            g_error = string("Delete failed: ") + e.what();
            g_message = "Error";
        }
        return;
    }

    std::vector<string> paths;
    long long total = 0;
    for (auto& s : g_snapshots) {
        if (g_selected.count(s.path)) {
            paths.push_back(s.path);
            total += s.size;
        }
    }
    if (paths.empty()) return;

    size_t failed = 0;
    string first_error;
    for (auto& p : paths) {
        try {
            remove_tree(p);
        } catch (const std::exception& e) {
            if (failed == 0) first_error = e.what();
            ++failed;
        }
    }
    bool recycled = last_delete_went_to_recycle();

    if (failed == 0) {
        g_message = (recycled ? "Moved " : "Removed ") + std::to_string(paths.size()) +
                    " snapshot(s)" + (recycled ? " to the Recycle Bin (" : " (") +
                    u8fmt_size(total) + (recycled ? ")" : " freed)");
    } else if (failed == paths.size()) {
        g_error = first_error;
        g_message = "Error";
    } else {
        g_message = "Removed " + std::to_string(paths.size() - failed) + " snapshot(s), " +
                    std::to_string(failed) + " failed: " + utf8_ellipsize(first_error, 60);
    }
    g_selected.clear();
    refresh_data(true);
}

static void do_cleanup_orphans() {
    try {
        std::vector<string> failed;
        auto orphans = cleanup_orphan_diffs(GUI_DB_PATH, GUI_DIFF_DIR, &failed);
        if (orphans.empty() && failed.empty())
            g_message = "No orphan session_diff files to clean.";
        else
            g_message = (delete_to_recycle() ? "Moved " : "Removed ") +
                        std::to_string(orphans.size()) + " orphan diff file(s)" +
                        (failed.empty() ? "" : ", " + std::to_string(failed.size()) + " failed");
    } catch (const std::exception& e) {
        g_error = string("Cleanup failed: ") + e.what();
        g_message = "Error";
    }
}

// ---------------------------------------------------------------------------
// Column widths derived from the live font and style.
// Hard-coded pixel values break the moment the DPI or the text size changes --
// that is exactly what clipped the "dir" button (fixed 150px column, while
// copy+jump+dir actually needed 171px at 150% scaling).
// ---------------------------------------------------------------------------
static float sel_column_width() {
    const ImGuiStyle& st = ImGui::GetStyle();
    return ImGui::GetFrameHeight() + st.CellPadding.x * 2.0f + 4.0f;
}

static float button_column_width(const char* a, const char* b, const char* c) {
    const ImGuiStyle& st = ImGui::GetStyle();
    float text = ImGui::CalcTextSize(a).x + ImGui::CalcTextSize(b).x;
    float buttons = 2.0f;
    if (c && *c) {
        text += ImGui::CalcTextSize(c).x;
        buttons = 3.0f;
    }
    return text + 2.0f * buttons * st.FramePadding.x + (buttons - 1.0f) * st.ItemSpacing.x +
           st.CellPadding.x * 2.0f + 8.0f;
}

// ---------------------------------------------------------------------------
// UI: toolbar
// ---------------------------------------------------------------------------
static void draw_toolbar() {
    if (ImGui::Button("Refresh (F5)")) refresh_data(true);
    ImGui::SameLine();
    if (ImGui::Button("Select All")) select_all_visible();
    ImGui::SameLine();
    if (ImGui::Button("Clear Sel")) clear_selection();
    ImGui::SameLine();

    bool can_delete = !g_selected.empty();
    ImGui::BeginDisabled(!can_delete);
    if (ImGui::Button("Delete Selected (Del)")) g_confirm_request = true;
    ImGui::EndDisabled();
    if (!can_delete && ImGui::IsItemHovered())
        ImGui::SetTooltip("Select at least one row first");

    ImGui::SameLine();
    ImGui::TextDisabled("|");
    ImGui::SameLine();
    if (ImGui::Button("Clean orphan diffs")) do_cleanup_orphans();
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Removes session_diff/*.json files whose session no longer exists");
    ImGui::SameLine();
    ImGui::Checkbox("Cleanup on exit", &g_cleanup_on_exit);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Also clean orphan diffs when the window closes (off by default)");

    // Second row: search and filters. Kept separate so the bar cannot overflow
    // the window when the text size is turned up.
    ImGui::TextUnformatted("Search:");
    ImGui::SameLine();
    if (g_focus_filter) {
        ImGui::SetKeyboardFocusHere();
        g_focus_filter = false;
    }
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 14.0f);
    ImGui::InputTextWithHint("##filter", "id / title / project / path", g_filter, sizeof(g_filter));
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Ctrl+F focuses this box, Esc clears it");
    ImGui::SameLine();
    ImGui::Checkbox("Orphans only", &g_orphans_only);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Snapshots view: only directories with no session referencing them");

    // Rule based selection: filter, then grab everything that matches.
    ImGui::SameLine();
    ImGui::TextDisabled("|");
    ImGui::SameLine();
    ImGui::TextUnformatted("Older than");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 3.2f);
    if (ImGui::InputInt("##olderdays", &g_older_days_i, 0, 0)) {
        if (g_older_days_i < 0) g_older_days_i = 0;
        g_view_dirty = true;
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Sessions view: only sessions created more than N days ago (0 = off)");
    ImGui::SameLine();
    ImGui::TextUnformatted("days");
    ImGui::SameLine();
    ImGui::TextUnformatted("max msgs");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 3.2f);
    if (ImGui::InputInt("##maxmsgs", &g_max_msgs_i, 0, 0)) {
        if (g_max_msgs_i < -1) g_max_msgs_i = -1;
        g_view_dirty = true;
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Sessions view: only sessions with at most N messages (-1 = off)");
    ImGui::SameLine();
    if (ImGui::Button("Select matching")) select_all_visible();
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Select every row the current filter shows (then Delete Selected)");

    ImGui::SameLine();
    ImGui::Checkbox("Preview", &g_show_preview);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Sessions view: show the tail of the selected session");

    ImGui::SameLine();
    ImGui::TextDisabled("|");
    ImGui::SameLine();
    ImGui::Text("Selected: %d/%d", (int)g_selected.size(), (int)view_size());

    // Text size lives here rather than in the menu bar so neither bar can
    // overflow when the text is turned up.
    ImGui::SameLine(0, 32);
    ImGui::TextDisabled("|");
    ImGui::SameLine();
    ImGui::Text("Text: %.0f px", g_text_px);
    ImGui::SameLine();
    if (ImGui::SmallButton("-##font")) set_text_px(g_text_px - 2.0f);
    ImGui::SameLine();
    if (ImGui::SmallButton("+##font")) set_text_px(g_text_px + 2.0f);
    ImGui::SameLine();
    if (ImGui::SmallButton("reset##font")) set_text_px(default_text_px());
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Back to the size this app has always used\n(standard would be %.0f px at this DPI)",
                          kStandardTextPx);
}

// ---------------------------------------------------------------------------
// UI: sessions tab
// ---------------------------------------------------------------------------
static void draw_sessions_tab() {
    if (g_sessions.empty()) {
        ImGui::TextDisabled("No sessions found in %s", GUI_DB_PATH.c_str());
        return;
    }

    const ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders |
                                  ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollY |
                                  ImGuiTableFlags_Sortable | ImGuiTableFlags_SizingStretchProp;
    if (!ImGui::BeginTable("sessions", 8, flags)) return;

    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("Sel", ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_NoSort,
                            sel_column_width());
    ImGui::TableSetupColumn("Session id", ImGuiTableColumnFlags_WidthStretch, 2.4f);
    ImGui::TableSetupColumn("Title", ImGuiTableColumnFlags_WidthStretch, 3.2f);
    ImGui::TableSetupColumn("Project / Worktree", ImGuiTableColumnFlags_WidthStretch, 2.8f);
    ImGui::TableSetupColumn("Msgs", ImGuiTableColumnFlags_WidthStretch, 0.8f);
    ImGui::TableSetupColumn("Created", ImGuiTableColumnFlags_WidthStretch | ImGuiTableColumnFlags_DefaultSort |
                                          ImGuiTableColumnFlags_PreferSortDescending,
                              1.8f);
    ImGui::TableSetupColumn("Updated", ImGuiTableColumnFlags_WidthStretch, 1.8f);
    ImGui::TableSetupColumn("Actions", ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_NoSort,
                            button_column_width("copy", "jump", "dir"));
    ImGui::TableHeadersRow();

    if (ImGuiTableSortSpecs* specs = ImGui::TableGetSortSpecs()) {
        if (specs->SpecsCount > 0) {
            int col = specs->Specs[0].ColumnIndex;
            bool asc = specs->Specs[0].SortDirection == ImGuiSortDirection_Ascending;
            if (col != g_sort_col[TAB_SESSIONS] || asc != g_sort_asc[TAB_SESSIONS]) {
                g_sort_col[TAB_SESSIONS] = col;
                g_sort_asc[TAB_SESSIONS] = asc;
                g_view_dirty = true;
            }
        }
        specs->SpecsDirty = false;
    }
    ensure_view();

    if (g_view_sessions.empty()) {
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImGui::TextDisabled(g_filter[0] ? "No session matches the search." : "No sessions.");
        ImGui::EndTable();
        return;
    }

    for (size_t row = 0; row < g_view_sessions.size(); ++row) {
        Session& s = g_sessions[(size_t)g_view_sessions[row]];
        bool is_selected = g_selected.count(s.id) > 0;

        ImGui::TableNextRow();
        ImGui::PushID((int)row);

        ImGui::TableSetColumnIndex(0);
        if (ImGui::Checkbox("##sel", &is_selected)) {
            if (is_selected) g_selected.insert(s.id);
            else g_selected.erase(s.id);
        }

        ImGui::TableSetColumnIndex(1);
        ImGui::TextUnformatted(s.id.c_str());
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("opencode -s %s", s.id.c_str());

        ImGui::TableSetColumnIndex(2);
        ImGui::TextUnformatted(s.title.empty() ? "(untitled)" : u8(s.title).c_str());

        ImGui::TableSetColumnIndex(3);
        {
            string proj = s.project_name.empty() ? s.worktree : s.project_name;
            ImGui::TextUnformatted(proj.empty() ? "-" : proj.c_str());
            if (ImGui::IsItemHovered() && !s.worktree.empty())
                ImGui::SetTooltip("%s", s.worktree.c_str());
        }

        ImGui::TableSetColumnIndex(4);
        ImGui::Text("%lld", s.msg_count);

        ImGui::TableSetColumnIndex(5);
        ImGui::TextUnformatted(u8fmt_time(s.time_created).c_str());

        ImGui::TableSetColumnIndex(6);
        ImGui::TextUnformatted(u8fmt_time(s.time_updated).c_str());

        ImGui::TableSetColumnIndex(7);
        if (ImGui::SmallButton("copy")) {
            string cmd = "opencode -s " + s.id;
            ImGui::SetClipboardText(cmd.c_str());
            g_toast = "Copied: " + cmd;
            g_toast_expire = ImGui::GetTime() + 2.0;
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Copy the resume command");
        ImGui::SameLine();
        if (ImGui::SmallButton("jump")) {
            string err;
            if (launch_opencode_session(s.id, s.worktree, &err))
                g_message = "Resumed session " + s.id;
            else
                g_error = err.empty() ? "Failed to launch opencode" : err;
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Resume this session in a new console\n(worktree: %s)",
                              s.worktree.empty() ? "-" : s.worktree.c_str());
        ImGui::SameLine();
        ImGui::BeginDisabled(!is_dir(s.worktree));
        if (ImGui::SmallButton("dir")) {
            string err;
            if (!reveal_in_explorer(s.worktree, &err)) g_error = err;
        }
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip(is_dir(s.worktree) ? "Open the worktree in Explorer"
                                                 : "Worktree no longer exists");

        ImGui::PopID();
    }
    ImGui::EndTable();
}

// ---------------------------------------------------------------------------
// UI: snapshots tab
// ---------------------------------------------------------------------------
static void draw_snapshots_tab() {
    if (g_snapshots.empty()) {
        ImGui::TextDisabled("No snapshots found in %s", GUI_SNAPSHOT_DIR.c_str());
        return;
    }

    const ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders |
                                  ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollY |
                                  ImGuiTableFlags_Sortable | ImGuiTableFlags_SizingStretchProp;
    if (!ImGui::BeginTable("snapshots", 6, flags)) return;

    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("Sel", ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_NoSort,
                            sel_column_width());
    ImGui::TableSetupColumn("Status", ImGuiTableColumnFlags_WidthStretch | ImGuiTableColumnFlags_NoSort, 1.4f);
    ImGui::TableSetupColumn("Worktree", ImGuiTableColumnFlags_WidthStretch, 4.2f);
    ImGui::TableSetupColumn("Size", ImGuiTableColumnFlags_WidthStretch | ImGuiTableColumnFlags_DefaultSort |
                                        ImGuiTableColumnFlags_PreferSortDescending,
                            1.2f);
    ImGui::TableSetupColumn("Project", ImGuiTableColumnFlags_WidthStretch, 2.4f);
    ImGui::TableSetupColumn("Actions", ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_NoSort,
                            button_column_width("open", "copy path", nullptr));
    ImGui::TableHeadersRow();

    if (ImGuiTableSortSpecs* specs = ImGui::TableGetSortSpecs()) {
        if (specs->SpecsCount > 0) {
            int col = specs->Specs[0].ColumnIndex;
            bool asc = specs->Specs[0].SortDirection == ImGuiSortDirection_Ascending;
            if (col != g_sort_col[TAB_SNAPSHOTS] || asc != g_sort_asc[TAB_SNAPSHOTS]) {
                g_sort_col[TAB_SNAPSHOTS] = col;
                g_sort_asc[TAB_SNAPSHOTS] = asc;
                g_view_dirty = true;
            }
        }
        specs->SpecsDirty = false;
    }
    ensure_view();

    if (g_view_snapshots.empty()) {
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImGui::TextDisabled("No snapshot matches the current filter.");
        ImGui::EndTable();
        return;
    }

    for (size_t row = 0; row < g_view_snapshots.size(); ++row) {
        Snapshot& s = g_snapshots[(size_t)g_view_snapshots[row]];
        bool is_orphan = !s.active_sessions;
        bool is_selected = g_selected.count(s.path) > 0;

        ImGui::TableNextRow();
        ImGui::PushID((int)row);

        ImGui::TableSetColumnIndex(0);
        if (ImGui::Checkbox("##sel", &is_selected)) {
            if (is_selected) g_selected.insert(s.path);
            else g_selected.erase(s.path);
        }

        ImGui::TableSetColumnIndex(1);
        if (is_orphan)
            ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.42f, 1.0f), "no session");
        else
            ImGui::TextColored(ImVec4(0.42f, 0.85f, 0.5f, 1.0f), "in use");
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip(is_orphan ? "No session references this project any more"
                                        : "A session still references this project");

        ImGui::TableSetColumnIndex(2);
        ImGui::TextUnformatted(s.worktree.c_str());
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", s.worktree.c_str());

        ImGui::TableSetColumnIndex(3);
        if (s.size_known)
            ImGui::TextUnformatted(u8fmt_size(s.size).c_str());
        else
            ImGui::TextDisabled("...");

        ImGui::TableSetColumnIndex(4);
        ImGui::TextUnformatted(u8(s.name).c_str());

        ImGui::TableSetColumnIndex(5);
        if (ImGui::SmallButton("open")) {
            string err;
            if (!reveal_in_explorer(s.path, &err)) g_error = err;
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Open this snapshot folder in Explorer");
        ImGui::SameLine();
        if (ImGui::SmallButton("copy path")) {
            ImGui::SetClipboardText(s.path.c_str());
            g_toast = "Copied: " + s.path;
            g_toast_expire = ImGui::GetTime() + 2.0;
        }

        ImGui::PopID();
    }
    ImGui::EndTable();
}

// ---------------------------------------------------------------------------
// UI: session preview panel
// ---------------------------------------------------------------------------
static void draw_preview_panel() {
    if (!g_show_preview || g_tab != TAB_SESSIONS) return;

    float h = ImGui::GetTextLineHeightWithSpacing() * 8.0f + ImGui::GetFrameHeightWithSpacing();
    ImGui::BeginChild("##preview", ImVec2(0, h), true);
    ImGui::TextDisabled("Preview");
    ImGui::SameLine();
    if (g_preview_for.empty())
        ImGui::TextDisabled("- select exactly one session");
    else
        ImGui::TextDisabled("%s   (%d part(s) shown)", g_preview_for.c_str(), (int)g_preview.size());

    if (!g_preview_error.empty()) {
        ImGui::TextColored(ImVec4(1.0f, 0.42f, 0.42f, 1.0f), "%s", g_preview_error.c_str());
    } else if (!g_preview_for.empty() && g_preview.empty()) {
        ImGui::TextDisabled("No parts recorded for this session.");
    }
    ImGui::Separator();
    for (auto& m : g_preview) {
        bool user = (m.role == "user");
        ImGui::TextColored(user ? ImVec4(0.55f, 0.78f, 1.0f, 1.0f) : ImVec4(0.6f, 0.9f, 0.6f, 1.0f),
                           "%s", m.role.empty() ? m.kind.c_str() : m.role.c_str());
        ImGui::SameLine();
        ImGui::TextDisabled("%s", u8fmt_time(m.time_created).c_str());
        ImGui::TextWrapped("%s", m.text.c_str());
    }
    ImGui::EndChild();
}

// ---------------------------------------------------------------------------
// UI: database space report and maintenance
// ---------------------------------------------------------------------------
static void draw_usage_table(const char* id, const char* col0, const char* col1,
                             const std::vector<UsageEntry>& rows, bool show_extra) {
    if (!ImGui::BeginTable(id, show_extra ? 4 : 3,
                           ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders | ImGuiTableFlags_ScrollY,
                           ImVec2(0, ImGui::GetTextLineHeightWithSpacing() * 7.0f)))
        return;
    ImGui::TableSetupColumn(col0, ImGuiTableColumnFlags_WidthStretch, 3.0f);
    ImGui::TableSetupColumn(col1, ImGuiTableColumnFlags_WidthFixed, ImGui::GetFontSize() * 9.0f);
    ImGui::TableSetupColumn("size", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFontSize() * 7.0f);
    if (show_extra) ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthStretch, 1.6f);
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableHeadersRow();
    for (auto& e : rows) {
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImGui::TextUnformatted(e.name.c_str());
        ImGui::TableSetColumnIndex(1);
        ImGui::Text("%d", (int)e.rows);
        ImGui::TableSetColumnIndex(2);
        ImGui::TextUnformatted(u8fmt_size(e.bytes).c_str());
        if (show_extra) {
            ImGui::TableSetColumnIndex(3);
            ImGui::TextUnformatted(e.extra.c_str());
        }
    }
    ImGui::EndTable();
}

static void draw_job_progress() {
    JobStatus st = g_job.status();
    if (!st.running) return;
    if (st.percent >= 0)
        ImGui::ProgressBar(st.percent / 100.0f, ImVec2(-1.0f, 0.0f), st.stage.c_str());
    else {
        ImGui::ProgressBar(-1.0f * (float)ImGui::GetTime(), ImVec2(-1.0f, 0.0f), "");
        ImGui::TextDisabled("%s", st.stage.c_str());
    }
}

static void draw_database_tab() {
    JobStatus st = g_job.status();
    const bool busy = st.running;

    ImGui::BeginDisabled(busy);
    if (ImGui::Button("Scan database")) g_job.start(JobType::Report, GUI_DB_PATH);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Read-only. Shows where the disk space goes\n(this takes a minute or two on a large database)");

    ImGui::SameLine();
    ImGui::TextDisabled("|");
    ImGui::SameLine();
    if (ImGui::Button("Analyse event log")) g_job.start(JobType::CompactEvents, GUI_DB_PATH);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Dry run: how much would keeping only the newest\nversion of each part reclaim?");

    ImGui::SameLine();
    ImGui::TextDisabled("|");
    ImGui::SameLine();
    if (ImGui::Button("Clean orphan events")) {
        g_maint_action = MA_CLEAN_ORPHANS;
        g_maint_backup_path.clear();
        g_maint_modal = true;
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Remove event rows belonging to sessions that are already deleted.\nSafe: nothing can replay a deleted aggregate.");

    ImGui::SameLine();
    if (ImGui::Button("Compact event log")) {
        g_maint_action = MA_COMPACT;
        g_maint_keep_backup = true;
        g_maint_backup_path = timestamped_backup_path();
        g_maint_modal = true;
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Keep only the newest snapshot of each message part.\nThis rewrites the opencode event log - back up first.");

    ImGui::SameLine();
    if (ImGui::Button("VACUUM")) {
        g_maint_action = MA_VACUUM;
        g_maint_backup_path.clear();
        g_maint_modal = true;
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Rewrite the file to hand free pages back to the filesystem.\nNeeds temp space and a few minutes; close opencode first.");

    ImGui::SameLine();
    if (ImGui::Button("Backup database")) {
        g_maint_action = MA_BACKUP;
        g_maint_backup_path = timestamped_backup_path() + ".bak";
        g_maint_modal = true;
    }
    ImGui::EndDisabled();

    if (busy) {
        ImGui::Spacing();
        draw_job_progress();
    } else if (st.finished) {
        ImGui::Spacing();
        if (st.type == JobType::CleanupOrphans || st.type == JobType::CompactEvents) {
            if (st.ok)
                ImGui::Text("%s: removed %d rows (%s)", job_type_name(st.type), (int)st.result.rows,
                            u8fmt_size(st.result.bytes).c_str());
            else
                ImGui::TextColored(ImVec4(1.0f, 0.42f, 0.42f, 1.0f), "%s failed: %s",
                                   job_type_name(st.type), st.error.c_str());
            if (st.ok && st.result.file_before == st.result.file_after)
                ImGui::TextDisabled("The file only shrinks after a VACUUM.");
        } else if (st.type == JobType::Vacuum) {
            if (st.ok)
                ImGui::Text("VACUUM: %s -> %s (freed %s)",
                            u8fmt_size(st.result.file_before).c_str(),
                            u8fmt_size(st.result.file_after).c_str(),
                            u8fmt_size(st.result.bytes).c_str());
            else
                ImGui::TextColored(ImVec4(1.0f, 0.42f, 0.42f, 1.0f), "VACUUM failed: %s",
                                   st.error.c_str());
        } else if (st.type == JobType::Backup) {
            if (st.ok)
                ImGui::Text("Backup written (%s)", u8fmt_size(st.result.bytes).c_str());
            else
                ImGui::TextColored(ImVec4(1.0f, 0.42f, 0.42f, 1.0f), "Backup failed: %s",
                                   st.error.c_str());
        } else if (st.type == JobType::Report && !st.error.empty()) {
            ImGui::TextColored(ImVec4(1.0f, 0.42f, 0.42f, 1.0f), "Scan failed: %s", st.error.c_str());
        }
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    const DbReport& r = st.report;
    if (r.file_bytes <= 0) {
        ImGui::TextDisabled("Press \"Scan database\" to see where the space goes.");
        ImGui::Spacing();
        ImGui::TextWrapped(
            "Background: in a real opencode database 90%%+ of the file can be the append-only `event` "
            "log, which stores a complete snapshot of a message part on every update. Deleting "
            "sessions does not touch those rows, which is why the file never shrinks.");
        return;
    }

    if (ImGui::BeginTable("dbmetrics", 3, ImGuiTableFlags_SizingStretchSame)) {
        const char* labels[3] = {"database file", "free pages (VACUUM)",
                                 "safe to reclaim now"};
        char values[3][64];
        snprintf(values[0], 64, "%s", u8fmt_size(r.file_bytes).c_str());
        snprintf(values[1], 64, "%s", u8fmt_size(r.free_bytes).c_str());
        snprintf(values[2], 64, "%s", u8fmt_size(r.reclaimable_safe()).c_str());
        for (int i = 0; i < 3; ++i) {
            ImGui::TableNextColumn();
            ImGui::TextDisabled("%s", labels[i]);
            ImGui::Text("%s", values[i]);
        }
        ImGui::EndTable();
    }

    ImGui::Spacing();
    ImGui::Text("event log: %d rows, %s   |   session %d, message %d, part %d",
                (int)r.event_rows, u8fmt_size(r.event_bytes).c_str(), (int)r.session_rows,
                (int)r.message_rows, (int)r.part_rows);
    ImGui::Spacing();

    if (ImGui::BeginTabBar("dbtabs")) {
        if (ImGui::BeginTabItem("Tables")) {
            draw_usage_table("t_tables", "table", "rows",
                             r.tables, false);
            if (!r.dbstat_available)
                ImGui::TextDisabled("dbstat is not compiled into this sqlite3, so sizes are estimated from content bytes.");
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Event types")) {
            draw_usage_table("t_types", "event type", "rows", r.event_types, false);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Sessions")) {
            JobStatus cur = g_job.status();
            (void)cur;
            draw_usage_table("t_sessions", "session", "rows", r.sessions, true);
            ImGui::TextDisabled("Tip: switch to the Sessions view and delete these to stop them growing.");
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Oversized")) {
            if (r.big_items.empty())
                ImGui::TextDisabled("Nothing unusually large.");
            else
                draw_usage_table("t_big", "object", "rows", r.big_items, false);
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();
    ImGui::Text("orphan events (deleted sessions): %d rows, %s", (int)r.orphan_event_rows,
                u8fmt_size(r.orphan_event_bytes).c_str());
    if (st.plan.total_rows > 0) {
        ImGui::Text("event log compaction: %d of %d rows redundant, %s reclaimable", (int)st.plan.removable_rows,
                    (int)st.plan.total_rows, u8fmt_size(st.plan.removable_bytes).c_str());
        if (!st.plan.worst_parts.empty()) {
            ImGui::TextDisabled("worst part: %s with %d versions", st.plan.worst_parts[0].name.c_str(),
                                (int)st.plan.worst_parts[0].rows);
        }
    }
}

// ---------------------------------------------------------------------------
// UI: maintenance confirmation
// ---------------------------------------------------------------------------
static void draw_maintenance_modal() {
    if (!g_maint_modal) return;
    ImGui::OpenPopup("Database maintenance");
    ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));

    if (ImGui::BeginPopupModal("Database maintenance", nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove)) {
        JobStatus st = g_job.status();
        switch (g_maint_action) {
        case MA_CLEAN_ORPHANS:
            ImGui::Text("Clean orphan events?");
            ImGui::TextWrapped(
                "Removes event rows that belong to sessions which no longer exist. "
                "Nothing can replay a deleted aggregate, so this is safe.");
            if (st.report.orphan_event_rows > 0)
                ImGui::Text("Would remove %d rows, about %s.", (int)st.report.orphan_event_rows,
                            u8fmt_size(st.report.orphan_event_bytes).c_str());
            else
                ImGui::TextDisabled("Run \"Scan database\" first for an exact figure.");
            break;
        case MA_COMPACT:
            ImGui::Text("Compact the event log?");
            ImGui::TextWrapped(
                "opencode stores a complete snapshot of a message part on every update, so one part "
                "can have tens of thousands of versions. This keeps only the newest version of each "
                "part and deletes the rest. It rewrites the opencode event log.");
            if (st.plan.total_rows > 0)
                ImGui::Text("Would remove %d rows, about %s.", (int)st.plan.removable_rows,
                            u8fmt_size(st.plan.removable_bytes).c_str());
            else
                ImGui::TextDisabled("Tip: run \"Analyse event log\" first (dry run) for exact figures.");
            ImGui::Checkbox("Back up the database first (recommended)", &g_maint_keep_backup);
            if (g_maint_keep_backup) {
                char buf[512];
                snprintf(buf, sizeof(buf), "%s", g_maint_backup_path.c_str());
                ImGui::SetNextItemWidth(ImGui::GetFontSize() * 26.0f);
                if (ImGui::InputText("##backup", buf, sizeof(buf))) g_maint_backup_path = buf;
                ImGui::TextDisabled("%s free space needed; the file only shrinks after a VACUUM.",
                                    u8fmt_size(st.report.file_bytes > 0 ? st.report.file_bytes
                                                                       : file_size(GUI_DB_PATH))
                                        .c_str());
            }
            break;
        case MA_VACUUM:
            ImGui::Text("Run VACUUM?");
            ImGui::TextWrapped(
                "VACUUM rewrites the whole file so free pages go back to the filesystem. It needs "
                "temporary disk space about the size of the database and takes a few minutes on a "
                "multi-GB file.");
            if (st.report.free_bytes > 0)
                ImGui::Text("Free pages now: %s.", u8fmt_size(st.report.free_bytes).c_str());
            if (database_is_busy(GUI_DB_PATH))
                ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.3f, 1.0f),
                                   "opencode appears to be holding the database: VACUUM will fail.");
            break;
        case MA_BACKUP:
            ImGui::Text("Back up the database?");
            ImGui::TextWrapped("Copies the database file with sqlite3's online backup API.");
            {
                char buf[512];
                snprintf(buf, sizeof(buf), "%s", g_maint_backup_path.c_str());
                ImGui::SetNextItemWidth(ImGui::GetFontSize() * 26.0f);
                if (ImGui::InputText("##backupdest", buf, sizeof(buf))) g_maint_backup_path = buf;
            }
            break;
        default:
            break;
        }

        ImGui::Spacing();
        ImGui::Separator();
        bool can_go = !g_job.running();
        if (g_maint_action == MA_COMPACT && g_maint_keep_backup && g_maint_backup_path.empty())
            can_go = false;
        if (g_maint_action == MA_BACKUP && g_maint_backup_path.empty()) can_go = false;

        ImGui::BeginDisabled(!can_go);
        if (ImGui::Button("Confirm", ImVec2(ImGui::GetFontSize() * 7.0f, 0.0f))) {
            g_maint_modal = false;
            ImGui::CloseCurrentPopup();
            start_maintenance(g_maint_action);
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(ImGui::GetFontSize() * 7.0f, 0.0f))) {
            g_maint_modal = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

// ---------------------------------------------------------------------------
// UI: modals
// ---------------------------------------------------------------------------
static void draw_confirm_modal() {
    // OpenPopup only on the transition, and keep a p_open so Esc / the close
    // button can actually dismiss it (they used to be ignored).
    if (g_confirm_request) {
        g_confirm_request = false;
        g_confirm_open = true;
        ImGui::OpenPopup("Confirm Delete");
    }
    if (!g_confirm_open) return;

    ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));

    if (ImGui::BeginPopupModal("Confirm Delete", &g_confirm_open,
                               ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove)) {
        int count = (int)g_selected.size();
        ImGui::Text("Remove %d item(s)?", count);

        if (g_tab == TAB_SNAPSHOTS) {
            ImGui::TextWrapped("Snapshot folders selected: %s.", u8fmt_size((long long)total_selected_size()).c_str());
        } else {
            ImGui::TextWrapped("These sessions and all of their messages are removed from the database.");
        }

        ImGui::Spacing();
        ImGui::BeginChild("##list", ImVec2(ImGui::GetFontSize() * 22.0f, ImGui::GetFontSize() * 5.0f),
                          true);
        int shown = 0;
        for (auto& key : g_selected) {
            if (shown++ >= 40) {
                ImGui::TextDisabled("... and %d more", count - 40);
                break;
            }
            if (g_tab == TAB_SNAPSHOTS)
                ImGui::TextWrapped("%s", key.c_str());
            else
                ImGui::TextUnformatted(key.c_str());
        }
        ImGui::EndChild();

        ImGui::Spacing();
        if (delete_to_recycle())
            ImGui::TextColored(ImVec4(0.45f, 0.85f, 0.55f, 1.0f),
                               "Files go to the Recycle Bin, so this is recoverable.");
        else
            ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.3f, 1.0f),
                               "OPENCODE_SM_NO_RECYCLE is set: this cannot be undone.");

        ImGui::Spacing();
        ImGui::Separator();
        if (ImGui::Button("Confirm Delete", ImVec2(ImGui::GetFontSize() * 7.0f, 0))) {
            g_confirm_open = false;
            ImGui::CloseCurrentPopup();
            do_delete();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(ImGui::GetFontSize() * 7.0f, 0))) {
            g_confirm_open = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

static void draw_error_modal() {
    if (g_error.empty()) return;
    ImGui::OpenPopup("Error");
    ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(ImGui::GetFontSize() * 24.0f, 0), ImGuiCond_Appearing);

    bool open = true;
    if (ImGui::BeginPopupModal("Error", &open, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 22.0f);
        ImGui::TextColored(ImVec4(1.0f, 0.42f, 0.42f, 1.0f), "%s", g_error.c_str());
        ImGui::PopTextWrapPos();
        ImGui::Spacing();
        ImGui::Separator();
        if (ImGui::Button("OK", ImVec2(ImGui::GetFontSize() * 6.0f, 0))) {
            g_error.clear();
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
    if (!open) g_error.clear();
}

// Transient toast, drawn on the foreground draw list.
static void draw_toast() {
    if (g_toast.empty()) return;
    double now = ImGui::GetTime();
    if (now > g_toast_expire + 0.5) {
        g_toast.clear();
        return;
    }
    float fade = (float)(g_toast_expire - now);
    if (fade < 0.0f) fade = 0.0f;
    float alpha = fade < 0.5f ? fade / 0.5f : 1.0f;
    if (alpha <= 0.0f) {
        g_toast.clear();
        return;
    }

    // Codepoint-aware clamp: the old byte-wise substr() could cut a CJK
    // character in half and render mojibake.
    string text = utf8_ellipsize(g_toast, 96);

    ImDrawList* dl = ImGui::GetForegroundDrawList();
    ImFont* font = ImGui::GetFont();
    float font_size = ImGui::GetFontSize();
    ImVec2 text_size = font->CalcTextSizeA(font_size, FLT_MAX, 0.0f, text.c_str());
    const float pad_x = font_size * 0.7f, pad_y = font_size * 0.4f;
    ImVec2 box_min(ImGui::GetIO().DisplaySize.x * 0.5f - text_size.x * 0.5f - pad_x,
                   ImGui::GetIO().DisplaySize.y - font_size * 3.0f);
    if (box_min.x < 8.0f) box_min.x = 8.0f;
    ImVec2 box_max(box_min.x + text_size.x + pad_x * 2.0f, box_min.y + text_size.y + pad_y * 2.0f);

    dl->AddRectFilled(box_min, box_max,
                      ImGui::ColorConvertFloat4ToU32(ImVec4(0.08f, 0.10f, 0.09f, 0.92f * alpha)), 6.0f);
    dl->AddRect(box_min, box_max,
                ImGui::ColorConvertFloat4ToU32(ImVec4(0.5f, 0.8f, 0.5f, alpha)), 6.0f);
    dl->AddText(ImVec2(box_min.x + pad_x, box_min.y + pad_y),
                ImGui::ColorConvertFloat4ToU32(ImVec4(0.6f, 1.0f, 0.6f, alpha)), text.c_str());
}

// ---------------------------------------------------------------------------
// UI: keyboard shortcuts
// ---------------------------------------------------------------------------
static void handle_shortcuts() {
    ImGuiIO& io = ImGui::GetIO();
    if (g_confirm_open) return;   // let the modal own the keyboard

    if (ImGui::IsKeyPressed(ImGuiKey_F5)) refresh_data(true);

    if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_A)) select_all_visible();
    if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_D)) clear_selection();
    if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_F)) g_focus_filter = true;

    // Text size: same gestures as a browser.
    if (io.KeyCtrl && (ImGui::IsKeyPressed(ImGuiKey_Equal) ||
                       ImGui::IsKeyPressed(ImGuiKey_KeypadAdd)))
        set_text_px(g_text_px + 2.0f);
    if (io.KeyCtrl && (ImGui::IsKeyPressed(ImGuiKey_Minus) ||
                       ImGui::IsKeyPressed(ImGuiKey_KeypadSubtract)))
        set_text_px(g_text_px - 2.0f);
    if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_0)) set_text_px(default_text_px());

    if (!io.WantTextInput && ImGui::IsKeyPressed(ImGuiKey_Delete) && !g_selected.empty())
        g_confirm_request = true;

    if (!io.WantTextInput && ImGui::IsKeyPressed(ImGuiKey_Escape)) {
        if (g_filter[0]) {
            g_filter[0] = '\0';
            g_view_dirty = true;
        } else {
            g_selected.clear();
        }
    }
}

// ---------------------------------------------------------------------------
// UI: main window
// ---------------------------------------------------------------------------
static void draw_ui() {
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(ImGui::GetIO().DisplaySize);
    ImGui::Begin("OpenCode Session Manager", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_MenuBar);

    if (ImGui::BeginMenuBar()) {
        ImGui::TextUnformatted("View:");
        ImGui::Separator();
        if (ImGui::Button(g_tab == TAB_SESSIONS ? "[Sessions]" : "Sessions")) {
            g_tab = TAB_SESSIONS;
            g_selected.clear();
            g_view_dirty = true;
        }
        ImGui::Separator();
        if (ImGui::Button(g_tab == TAB_SNAPSHOTS ? "[Snapshots]" : "Snapshots")) {
            g_tab = TAB_SNAPSHOTS;
            g_selected.clear();
            g_view_dirty = true;
        }
        ImGui::Separator();
        if (ImGui::Button(g_tab == TAB_DATABASE ? "[Database]" : "Database")) {
            g_tab = TAB_DATABASE;
            g_selected.clear();
            g_view_dirty = true;
        }
        ImGui::SameLine(0, 40);
        size_t orphans = 0;
        long long orphan_bytes = 0;
        for (auto& s : g_snapshots) {
            if (!s.active_sessions) {
                ++orphans;
                orphan_bytes += s.size;
            }
        }
        ImGui::TextDisabled("Sessions: %d    Snapshot dirs: %d    Orphan dirs: %d (%s)",
                            (int)g_sessions.size(), (int)g_snapshots.size(), (int)orphans,
                            u8fmt_size(orphan_bytes).c_str());
        ImGui::EndMenuBar();
    }

    // Pick up filter / orphans-only edits.
    if (g_filter_prev != g_filter || g_orphans_only_prev != g_orphans_only) {
        g_filter_prev = g_filter;
        g_orphans_only_prev = g_orphans_only;
        g_view_dirty = true;
    }

    handle_shortcuts();
    ensure_view();
    draw_toolbar();
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    if (g_tab == TAB_SESSIONS) {
        draw_sessions_tab();
        sync_preview();
        draw_preview_panel();
    } else if (g_tab == TAB_SNAPSHOTS) {
        draw_snapshots_tab();
    } else {
        draw_database_tab();
    }

    // Keep the sizes trickling in after the frame is built.
    pump_size_scan(SIZE_SCAN_PER_FRAME);

    // Report a finished background job exactly once, and pick up its effects.
    {
        JobStatus st = g_job.status();
        if (st.finished && !g_job_seen) {
            g_job_seen = true;
            if (!st.ok && !st.error.empty()) {
                g_error = st.error;
                g_message = "Error";
            } else {
                switch (st.type) {
                case JobType::Report:
                    g_message = "Database scanned.";
                    break;
                case JobType::CleanupOrphans:
                    g_message = "Removed " + std::to_string(st.result.rows) +
                                " orphan event row(s) (" + u8fmt_size(st.result.bytes) + ")";
                    refresh_data(true);
                    break;
                case JobType::CompactEvents:
                    if (st.result.rows > 0) {
                        g_message = "Compacted " + std::to_string(st.result.rows) +
                                    " redundant event row(s) (" + u8fmt_size(st.result.bytes) +
                                    "); run VACUUM to release the space";
                        refresh_data(true);
                    } else {
                        g_message = "Event log analysed: " +
                                    std::to_string(st.plan.removable_rows) +
                                    " redundant row(s), " + u8fmt_size(st.plan.removable_bytes);
                    }
                    break;
                case JobType::Vacuum:
                    g_message = "VACUUM finished: " + u8fmt_size(st.result.file_before) + " -> " +
                                u8fmt_size(st.result.file_after);
                    break;
                case JobType::Backup:
                    g_message = "Backup written (" + u8fmt_size(st.result.bytes) + ")";
                    break;
                default:
                    break;
                }
            }
        }
    }

    // Status line
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();
    bool is_err = g_message.rfind("Error", 0) == 0;
    if (is_err)
        ImGui::TextColored(ImVec4(1.0f, 0.42f, 0.42f, 1.0f), "%s", g_message.c_str());
    else
        ImGui::TextUnformatted(g_message.empty() ? "Ready." : g_message.c_str());

    if (g_size_scan_pending) {
        ImGui::SameLine();
        ImGui::TextDisabled("   scanning snapshot sizes... %d left",
                            (int)g_size_scan_pending);
    } else {
        ImGui::SameLine();
        ImGui::TextDisabled("   |   F5 refresh   Del remove   Ctrl+A all   Ctrl+D none   "
                            "Ctrl+F search   Ctrl +/- text size");
    }

    draw_confirm_modal();
    draw_error_modal();
    draw_maintenance_modal();


    ImGui::End();
    draw_toast();
}

// ---------------------------------------------------------------------------
// Win32 window + WGL (OpenGL 3.0 core) plumbing
// ---------------------------------------------------------------------------
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam,
                                                            LPARAM lParam);

static HDC g_hdc = nullptr;

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (ImGui_ImplWin32_WndProcHandler(hwnd, msg, wParam, lParam))
        return true;

    switch (msg) {
    case WM_ERASEBKGND:
        return 1;           // don't flicker; we repaint every frame
    case WM_CLOSE:
        // Never abandon a maintenance job half way: a VACUUM or a compaction is
        // a single long SQLite call, and killing the process mid-way would leave
        // the work unfinished with no way to report it.
        if (g_job.running()) {
            MessageBoxW(hwnd, L"A database job is still running.\n\nWait for it to finish, then close.",
                        L"opencode session manager", MB_ICONINFORMATION | MB_OK);
            return 0;
        }
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    case WM_DPICHANGED: {
        // Follow the OS-suggested rect; the scale change itself is picked up by
        // the main loop, which re-bakes the style and font scale.
        RECT* suggested = (RECT*)lParam;
        SetWindowPos(hwnd, nullptr, suggested->left, suggested->top,
                     suggested->right - suggested->left, suggested->bottom - suggested->top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
        return 0;
    }
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    default:
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
}

// WGL_ARB_create_context constants (avoid dragging in <GL/wglext.h>)
#define WGL_CONTEXT_MAJOR_VERSION_ARB 0x2091
#define WGL_CONTEXT_MINOR_VERSION_ARB 0x2092
#define WGL_CONTEXT_FLAGS_ARB         0x2094
#define WGL_CONTEXT_PROFILE_MASK_ARB  0x9126
#define WGL_CONTEXT_CORE_PROFILE_BIT_ARB 0x00000001

typedef HGLRC(WINAPI* PFN_wglCreateContextAttribsARB)(HDC, HGLRC, const int*);
static PFN_wglCreateContextAttribsARB wglCreateContextAttribsARB = nullptr;

static HGLRC create_gl_context(HWND hwnd) {
    g_hdc = GetDC(hwnd);
    if (!g_hdc)
        return nullptr;

    PIXELFORMATDESCRIPTOR pfd = {};
    pfd.nSize        = sizeof(pfd);
    pfd.nVersion     = 1;
    pfd.dwFlags      = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
    pfd.iPixelType   = PFD_TYPE_RGBA;
    pfd.cColorBits   = 24;
    pfd.cDepthBits   = 24;
    pfd.cStencilBits = 8;
    pfd.iLayerType   = PFD_MAIN_PLANE;

    const int pixel_format = ChoosePixelFormat(g_hdc, &pfd);
    if (pixel_format == 0 || !SetPixelFormat(g_hdc, pixel_format, &pfd))
        return nullptr;

    // probe wglCreateContextAttribsARB via a temporary 1.1 context
    HGLRC probe = wglCreateContext(g_hdc);
    if (!probe)
        return nullptr;
    wglMakeCurrent(g_hdc, probe);
    wglCreateContextAttribsARB =
        (PFN_wglCreateContextAttribsARB)wglGetProcAddress("wglCreateContextAttribsARB");

    HGLRC gl_context = nullptr;
    if (wglCreateContextAttribsARB) {
        const int attribs[] = {
            WGL_CONTEXT_MAJOR_VERSION_ARB, 3,
            WGL_CONTEXT_MINOR_VERSION_ARB, 0,
            WGL_CONTEXT_PROFILE_MASK_ARB,  WGL_CONTEXT_CORE_PROFILE_BIT_ARB,
            WGL_CONTEXT_FLAGS_ARB,         0,
            0
        };
        gl_context = wglCreateContextAttribsARB(g_hdc, nullptr, attribs);
    }
    if (gl_context == nullptr)
        gl_context = wglCreateContext(g_hdc);      // fallback: legacy GL 1.x

    wglMakeCurrent(nullptr, nullptr);
    wglDeleteContext(probe);

    if (gl_context == nullptr || !wglMakeCurrent(g_hdc, gl_context)) {
        if (gl_context)
            wglDeleteContext(gl_context);
        return nullptr;
    }
    return gl_context;
}

// Re-bakes the style for the current DPI scale and text size. ImGui requires the
// style to be reset before scaling it again, so we keep a pristine copy of the
// stock style captured at startup.
static ImGuiStyle g_base_style;
static float g_applied_scale = 1.0f;
static float g_applied_text_px = 0.0f;

static void apply_dpi_scale(float scale) {
    if (scale <= 0.0f) scale = 1.0f;
    g_dpi_scale = scale;
    if (g_text_px <= 0.0f) g_text_px = default_text_px();
    if (scale == g_applied_scale && g_text_px == g_applied_text_px) return;

    ImGuiStyle& style = ImGui::GetStyle();
    style = g_base_style;
    style.ScaleAllSizes(scale);      // paddings, scrollbars, spacings... (once)
    style.FontScaleDpi = scale;      // apply the monitor scale to glyphs (once)
    style.FontSizeBase = g_text_px;  // ... at the user's chosen text size
    g_applied_scale = scale;
    g_applied_text_px = g_text_px;
}

// ---------------------------------------------------------------------------
// WinMain
// ---------------------------------------------------------------------------
int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE, LPSTR, int nCmdShow) {
    // Per-monitor DPI awareness (best font / OpenGL scaling on HiDPI).
    ImGui_ImplWin32_EnableDpiAwareness();

    if (!path_exists(GUI_DB_PATH)) {
        fprintf(stderr, "Database not found: %s\n", GUI_DB_PATH.c_str());
        MessageBoxA(nullptr, ("Database not found:\n" + GUI_DB_PATH +
                              "\n\nSet OPENCODE_DATA_DIR to point at another data directory.")
                                 .c_str(),
                    "OpenCode Session Manager", MB_ICONERROR);
        return 1;
    }

    WNDCLASSEXW wc = {};
    wc.cbSize        = sizeof(wc);
    wc.style         = CS_CLASSDC;
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = hInstance;
    wc.hIcon         = LoadIconW(hInstance, MAKEINTRESOURCEW(1));
    wc.hIconSm       = (HICON)LoadImageW(hInstance, MAKEINTRESOURCEW(1),
                                         IMAGE_ICON, 16, 16, LR_DEFAULTCOLOR);
    wc.lpszClassName = L"OpenCodeSessionManagerWnd";
    if (!RegisterClassExW(&wc)) {
        MessageBoxA(nullptr, "RegisterClassExW() failed.", "OpenCode Session Manager", MB_ICONERROR);
        return 1;
    }

    HWND window = CreateWindowExW(0, wc.lpszClassName,
                                  (wstring(L"opencode session manager ") +
                                   utf8_to_wide(OPENCODE_SM_VERSION_STRING)).c_str(),
                                  WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 1100, 720,
                                  nullptr, nullptr, hInstance, nullptr);
    if (window == nullptr) {
        MessageBoxA(nullptr, "CreateWindowExW() failed.", "OpenCode Session Manager", MB_ICONERROR);
        return 1;
    }

    HICON hIcon = LoadIconW(hInstance, MAKEINTRESOURCEW(1));
    if (hIcon) {
        SendMessageW(window, WM_SETICON, ICON_BIG, (LPARAM)hIcon);
        SendMessageW(window, WM_SETICON, ICON_SMALL, (LPARAM)hIcon);
    }

    HGLRC gl_context = create_gl_context(window);
    if (gl_context == nullptr) {
        MessageBoxA(nullptr, "Could not create an OpenGL context.", "OpenCode Session Manager",
                    MB_ICONERROR);
        return 1;
    }

    float main_scale = ImGui_ImplWin32_GetDpiScaleForHwnd(window);
    if (main_scale <= 0.0f)
        main_scale = 1.0f;
    g_dpi_scale = main_scale;

    ShowWindow(window, nCmdShow);
    SetWindowPos(window, nullptr, 0, 0, (int)(1100 * main_scale), (int)(720 * main_scale),
                 SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);

    // Setup Dear ImGui context
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO(); (void)io;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.IniFilename = nullptr;   // don't persist window layout

    ImGui::StyleColorsDark();
    g_base_style = ImGui::GetStyle();
    load_settings();            // user's text size, if they ever changed it
    apply_dpi_scale(main_scale);

    // Load a CJK-capable font (session titles / project names may be Chinese).
    // Since ImGui 1.92 fonts are rasterised on demand at any size, so the size
    // passed here only seeds the atlas -- the size actually used comes from
    // style.FontSizeBase, which apply_dpi_scale() sets. (The old code multiplied
    // the DPI factor in here *and* via FontScaleDpi, which is why 16px text
    // rendered as 36px on a 150% display.)
    const char* font_candidates[] = {
        "C:/Windows/Fonts/msyh.ttc",     // Microsoft YaHei
        "C:/Windows/Fonts/msyhbd.ttc",
        "C:/Windows/Fonts/simhei.ttf",   // SimHei
        "C:/Windows/Fonts/simsun.ttc",   // SimSun
        "C:/Windows/Fonts/simkai.ttf",   // KaiTi
    };
    ImFontConfig cfg;
    cfg.OversampleH = 2;
    cfg.OversampleV = 1;
    ImFontGlyphRangesBuilder builder;
    builder.AddRanges(io.Fonts->GetGlyphRangesDefault());
    builder.AddRanges(io.Fonts->GetGlyphRangesChineseFull());
    ImVector<ImWchar> ranges;
    builder.BuildRanges(&ranges);

    ImFont* cjk_font = nullptr;
    for (auto* path : font_candidates) {
        cjk_font = io.Fonts->AddFontFromFileTTF(path, kStandardTextPx, &cfg, ranges.Data);
        if (cjk_font) break;
    }
    if (!cjk_font) {
        io.Fonts->AddFontDefault();
        fprintf(stderr, "warning: no CJK font found, falling back to the built-in font\n");
    }

    ImGui_ImplWin32_InitForOpenGL(window);
    ImGui_ImplOpenGL3_Init("#version 130");

    refresh_data(false);
    ImVec4 clear_color = ImVec4(0.10f, 0.11f, 0.13f, 1.00f);

    bool done = false;
    while (!done) {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
            if (msg.message == WM_QUIT)
                done = true;
        }
        if (done)
            break;

        if (IsIconic(window)) {
            Sleep(10);
            continue;
        }

        // Follow the monitor the window currently sits on.
        apply_dpi_scale(ImGui_ImplWin32_GetDpiScaleForHwnd(window));
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();

        draw_ui();

        ImGui::Render();
        glViewport(0, 0, (int)io.DisplaySize.x, (int)io.DisplaySize.y);
        glClearColor(clear_color.x, clear_color.y, clear_color.z, clear_color.w);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        SwapBuffers(g_hdc);
    }

    if (g_text_px_dirty) save_settings();

    // Exit cleanup: opt-in only, and reported if it fails.
    if (g_cleanup_on_exit) {
        try {
            std::vector<string> failed;
            auto orphans = cleanup_orphan_diffs(GUI_DB_PATH, GUI_DIFF_DIR, &failed);
            if (!orphans.empty())
                fprintf(stderr, "Cleaned up %d orphan diff file(s)%s.\n", (int)orphans.size(),
                        delete_to_recycle() ? " (Recycle Bin)" : "");
            if (!failed.empty())
                fprintf(stderr, "%d orphan diff file(s) could not be removed.\n", (int)failed.size());
        } catch (const std::exception& e) {
            fprintf(stderr, "Orphan diff cleanup failed: %s\n", e.what());
        }
    }

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();

    wglMakeCurrent(nullptr, nullptr);
    wglDeleteContext(gl_context);
    ReleaseDC(window, g_hdc);
    DestroyWindow(window);
    UnregisterClassW(wc.lpszClassName, hInstance);
    return 0;
}
