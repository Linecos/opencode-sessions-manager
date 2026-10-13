// Database maintenance: where the disk actually goes, and how to get it back.
//
// Measured on a real 4.4 GB opencode database: 93% of it is the append-only
// `event` log, mostly `message.part.updated.1` rows that store a *full snapshot*
// of a message part on every update. Deleting a session does not touch those
// rows, which is why the file never shrinks. This module analyses that, and
// offers the (carefully gated) operations that reclaim it.
//
// All long operations report progress through a callback and are meant to run
// on a worker thread (see MaintenanceJob) so a 4 GB scan never freezes the UI.
#pragma once

#include <functional>
#include <string>
#include <vector>

#include "opencode_data.hpp"

using ProgressFn = std::function<void(int percent, const std::string& stage)>;

// ---------------------------------------------------------------------------
// Space analysis
// ---------------------------------------------------------------------------
struct UsageEntry {
    std::string name;
    std::string extra;       // e.g. the session title, for display next to the id
    long long bytes = 0;
    long long rows = 0;
};

struct DbReport {
    long long file_bytes = 0;        // page_count * page_size
    long long free_bytes = 0;        // free pages: what VACUUM alone can return
    long long page_size = 0;
    long long page_count = 0;
    long long freelist_count = 0;

    bool dbstat_available = false;
    // opencode v2 layout: sessions in `session_v2`, whole messages in
    // `session_message`, and the append-only `event` log retired. The v1 tables
    // (session/message/part) survive as frozen leftovers after the migration.
    bool v2_schema = false;

    // What a session/snapshot based cleanup can actually reach, for contrast
    // with event_bytes -- the numbers that made this module necessary.
    long long session_rows = 0, message_rows = 0, part_rows = 0;
    long long part_bytes = 0, message_bytes = 0;

    std::vector<UsageEntry> tables;      // by dbstat pages, largest first
    long long event_rows = 0, event_bytes = 0;
    std::vector<UsageEntry> event_types; // by text size, largest first
    std::vector<UsageEntry> sessions;    // event footprint per session, largest first
    long long orphan_event_rows = 0, orphan_event_bytes = 0;
    std::vector<UsageEntry> big_items;   // outliers worth looking at

    long long reclaimable_safe() const { return free_bytes + orphan_event_bytes; }
};

// Scans the database. On a multi-GB file this takes minutes (dbstat walks every
// page); pass a progress callback and run it in the background.
DbReport analyze_database(const std::string& db_path, int top_n = 12,
                          const ProgressFn& progress = nullptr);

// ---------------------------------------------------------------------------
// Maintenance operations
// ---------------------------------------------------------------------------
struct ReclaimResult {
    bool ok = true;
    long long rows = 0;        // rows removed
    long long bytes = 0;       // text bytes removed (estimate)
    long long file_before = 0;
    long long file_after = 0;
    std::string error;
};

// Removes event rows whose aggregate_id no longer matches any session. Safe by
// construction: the aggregate is gone, so nothing can replay it. Reclaimed space
// only returns to the filesystem after a VACUUM.
ReclaimResult delete_orphan_events(const std::string& db_path,
                                   const ProgressFn& progress = nullptr);

// ---------------------------------------------------------------------------
// Event log compaction
//
// `message.part.updated.1` stores a complete snapshot of a part on every update,
// so one part in a real database had 87,121 versions. Keeping only the newest
// version per part is the difference between 3.7 GB and a few MB.
//
// This rewrites opencode's event log, so it is deliberately conservative:
//   - rows whose part id cannot be read are never deleted,
//   - a part with any unreadable row is skipped entirely,
//   - it is dry-run by default and takes a backup before applying.
// Nothing short of running opencode against the result can prove the replay is
// still happy, which is why the backup is not optional.
// ---------------------------------------------------------------------------
struct CompactionPlan {
    long long total_rows = 0;      // message.part.updated.1 rows seen
    long long total_bytes = 0;
    long long parts = 0;           // distinct parts
    long long removable_rows = 0;
    long long removable_bytes = 0;
    long long skipped_parts = 0;   // parts skipped because a row was unreadable
    long long skipped_rows = 0;
    std::vector<long long> removable_rowids;   // used by apply()
    std::vector<UsageEntry> worst_parts;       // parts with the most versions
};

CompactionPlan plan_event_compaction(const std::string& db_path,
                                     const ProgressFn& progress = nullptr);

ReclaimResult apply_event_compaction(const std::string& db_path,
                                     const CompactionPlan& plan,
                                     const ProgressFn& progress = nullptr);

// ---------------------------------------------------------------------------
// VACUUM / backup
// ---------------------------------------------------------------------------
// Blocking variants (used by the CLI and by the worker thread).
bool vacuum_database(const std::string& db_path, ReclaimResult* out,
                     const ProgressFn& progress = nullptr);
bool backup_database(const std::string& db_path, const std::string& dest,
                     long long* bytes_copied, std::string* error);

// True when another process holds a lock (opencode running) -- worth warning
// about before a multi-minute VACUUM.
bool database_is_busy(const std::string& db_path);

// ---------------------------------------------------------------------------
// Auxiliary files next to the database
//
// The data directory holds more than opencode.db. `log/` accumulates a text log,
// and opencode v2 writes oversized tool output into `tool-output/` instead of
// into the database. Those tool-output files are part of the conversation -- the
// session records an `outputPath` pointing at them -- so only the ones no session
// references are safe to remove. That is exactly what deleting a session leaves
// behind, which is what makes this worth cleaning.
//
// The scan is read-only and cheap: the database is asked for the rows that
// mention `tool-output`, and the referenced file names are collected from those.
// ---------------------------------------------------------------------------
struct AuxItem {
    std::string name;        // relative to the data directory, e.g. "log/opencode.log"
    std::string path;        // absolute
    long long bytes = 0;
    bool removable = false;  // nothing references it, or opencode recreates it
    std::string note;        // why it is (not) removable
};

std::vector<AuxItem> scan_auxiliary(const std::string& data_dir, const std::string& db_path,
                                    const ProgressFn& progress = nullptr);

// Removes the items flagged removable (Recycle Bin when that is enabled).
ReclaimResult delete_auxiliary(const std::vector<AuxItem>& items,
                               const ProgressFn& progress = nullptr);

// ---------------------------------------------------------------------------
// Session preview and export
// ---------------------------------------------------------------------------
struct PreviewMessage {
    std::string role;           // "user" / "assistant" / ...
    std::string text;           // extracted text (already clipped)
    std::string kind;           // "text" or the tool/part type
    long long time_created = 0;
};

std::vector<PreviewMessage> load_session_preview(const std::string& db_path,
                                                 const std::string& session_id,
                                                 int limit = 20);

// Writes one JSON file per session (session row, messages and their parts).
bool export_sessions_json(const std::string& db_path,
                          const std::vector<std::string>& session_ids,
                          const std::string& out_dir,
                          std::vector<std::string>* written,
                          std::string* error);

// ---------------------------------------------------------------------------
// Background job wrapper (one job at a time)
// ---------------------------------------------------------------------------
enum class JobType { None, Report, CleanupOrphans, CompactEvents, Vacuum, Backup,
                     ScanAux, CleanAux };

const char* job_type_name(JobType type);

struct JobStatus {
    JobType type = JobType::None;
    bool running = false;
    bool finished = false;      // has completed or failed at least once
    bool ok = false;
    int percent = -1;           // -1 = indeterminate
    std::string stage;
    std::string error;
    double elapsed = 0.0;

    DbReport report;
    CompactionPlan plan;        // CompactEvents, dry run
    ReclaimResult result;
    std::vector<AuxItem> aux;   // ScanAux / CleanAux: files next to the database
};

class MaintenanceJob {
public:
    MaintenanceJob();
    ~MaintenanceJob();

    // type == Backup uses arg1 as the destination; CompactEvents uses arg1 as the
    // backup path (empty means "dry run, don't touch anything").
    bool start(JobType type, const std::string& db_path, const std::string& arg1 = std::string());
    bool running() const;
    JobStatus status() const;    // thread-safe snapshot
    void wait();

private:
    struct Impl;
    Impl* impl_;
    MaintenanceJob(const MaintenanceJob&);
    MaintenanceJob& operator=(const MaintenanceJob&);
};
