// Shared view model implementation.
#include "session_view.hpp"

#include <algorithm>

using std::string;
using std::vector;

bool session_matches(const Session& s, const ViewQuery& q) {
    if (q.created_before_ms > 0 && s.time_created >= q.created_before_ms) return false;
    if (q.max_messages >= 0 && s.msg_count > q.max_messages) return false;
    if (q.filter.empty()) return true;
    const string& f = q.filter;
    return utf8_icontains(s.id, f) || utf8_icontains(s.project_name, f) ||
           utf8_icontains(s.worktree, f) || utf8_icontains(wide_to_utf8(s.title), f);
}

bool snapshot_matches(const Snapshot& s, const ViewQuery& q) {
    if (q.orphans_only && s.active_sessions) return false;
    if (q.filter.empty()) return true;
    const string& f = q.filter;
    return utf8_icontains(s.id, f) || utf8_icontains(s.worktree, f) ||
           utf8_icontains(wide_to_utf8(s.name), f) || utf8_icontains(s.path, f);
}

namespace {

int cmp_i64(long long a, long long b) { return (a < b) ? -1 : (a > b ? 1 : 0); }

int compare_sessions(const Session& a, const Session& b, SortField field) {
    switch (field) {
    case SORT_UPDATED: return cmp_i64(a.time_updated, b.time_updated);
    case SORT_MESSAGES: return cmp_i64(a.msg_count, b.msg_count);
    case SORT_NAME: return a.title.compare(b.title);
    case SORT_LOCATION: {
        string pa = a.project_name.empty() ? a.worktree : a.project_name;
        string pb = b.project_name.empty() ? b.worktree : b.project_name;
        return pa.compare(pb);
    }
    case SORT_ID: return a.id.compare(b.id);
    case SORT_CREATED:
    default: return cmp_i64(a.time_created, b.time_created);
    }
}

int compare_snapshots(const Snapshot& a, const Snapshot& b, SortField field) {
    switch (field) {
    case SORT_ID: return a.path.compare(b.path);
    case SORT_NAME: return a.name.compare(b.name);
    case SORT_LOCATION: return a.worktree.compare(b.worktree);
    case SORT_STATUS: return (a.active_sessions ? 1 : 0) - (b.active_sessions ? 1 : 0);
    case SORT_SIZE:
    default: return cmp_i64(a.size, b.size);
    }
}

}  // namespace

vector<int> build_session_view(const vector<Session>& sessions, const ViewQuery& q) {
    vector<int> view;
    view.reserve(sessions.size());
    for (int i = 0; i < (int)sessions.size(); ++i)
        if (session_matches(sessions[(size_t)i], q)) view.push_back(i);

    const SortField field = q.field;
    const bool desc = q.descending;
    std::stable_sort(view.begin(), view.end(), [&sessions, field, desc](int ai, int bi) {
        const Session& a = sessions[(size_t)ai];
        const Session& b = sessions[(size_t)bi];
        int r = compare_sessions(a, b, field);
        if (r == 0) r = a.id.compare(b.id);   // deterministic tie-break
        return desc ? r > 0 : r < 0;
    });
    return view;
}

vector<int> build_snapshot_view(const vector<Snapshot>& snapshots, const ViewQuery& q) {
    vector<int> view;
    view.reserve(snapshots.size());
    for (int i = 0; i < (int)snapshots.size(); ++i)
        if (snapshot_matches(snapshots[(size_t)i], q)) view.push_back(i);

    const SortField field = q.field;
    const bool desc = q.descending;
    std::stable_sort(view.begin(), view.end(), [&snapshots, field, desc](int ai, int bi) {
        const Snapshot& a = snapshots[(size_t)ai];
        const Snapshot& b = snapshots[(size_t)bi];
        int r = compare_snapshots(a, b, field);
        if (r == 0) r = a.path.compare(b.path);   // deterministic tie-break
        return desc ? r > 0 : r < 0;
    });
    return view;
}

const char* sort_field_name(SortField field) {
    switch (field) {
    case SORT_CREATED: return "created";
    case SORT_UPDATED: return "updated";
    case SORT_MESSAGES: return "msgs";
    case SORT_SIZE: return "size";
    case SORT_STATUS: return "status";
    case SORT_ID: return "id";
    case SORT_NAME: return "name";
    case SORT_LOCATION: return "project";
    default: return "?";
    }
}
