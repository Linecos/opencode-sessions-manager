// Shared view model: filtering and ordering of sessions / snapshots.
//
// The TUI and the GUI both need "show me these rows, in this order". Keeping
// one implementation means the two front ends cannot drift apart, and it makes
// the rules testable without a screen (see tests/test_data.cpp).
#pragma once

#include <string>
#include <vector>

#include "opencode_data.hpp"

enum SortField {
    SORT_CREATED = 0,   // sessions
    SORT_UPDATED,       // sessions
    SORT_MESSAGES,      // sessions: message count
    SORT_SIZE,          // snapshots: bytes on disk
    SORT_STATUS,        // snapshots: in use vs orphan
    SORT_ID,            // session id / snapshot directory path
    SORT_NAME,          // session title / snapshot project name
    SORT_LOCATION,      // session project-or-worktree / snapshot worktree
    SORT_FIELD_COUNT
};

struct ViewQuery {
    std::string filter;          // UTF-8, ASCII-case-insensitive substring match
    bool orphans_only = false;   // snapshots: hide directories a session still uses
    SortField field = SORT_CREATED;
    bool descending = true;
    // Rule based selection ("show me what is old / small enough to delete").
    long long created_before_ms = 0;   // 0 = no lower bound
    long long max_messages = -1;       // -1 = no upper bound (sessions only)
};

// Does a row survive the filter? Each front end decides what fields it searches;
// these are the canonical sets (id/title/project/worktree for sessions,
// path/name/worktree/id for snapshots).
bool session_matches(const Session& s, const ViewQuery& q);
bool snapshot_matches(const Snapshot& s, const ViewQuery& q);

// Indices into the input vector, filtered and ordered.
std::vector<int> build_session_view(const std::vector<Session>& sessions, const ViewQuery& q);
std::vector<int> build_snapshot_view(const std::vector<Snapshot>& snapshots, const ViewQuery& q);

const char* sort_field_name(SortField field);
