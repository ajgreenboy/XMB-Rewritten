#pragma once
#include <atomic>
#include <mutex>
#include <QString>

namespace xmb {

// The states of a background game transfer (a release download job in Download Management). Kept in a
// header so the state predicates, the settle decision and the in-flight control flags are unit-testable
// without constructing a full Shell (release_tests.cpp). release_jobs.cpp aliases ReleaseJobState as its
// local `State` and uses JobControl directly, so the rest of that file is unchanged.
enum class ReleaseJobState {
    Queued, Paused, Resolving, Downloading, Pausing, Extracting, Cancelling, Finished, Failed, Cancelled
};

// A job can be paused while it still has a queued or started-but-not-paused transfer.
inline bool releaseJobCanPause(ReleaseJobState state) {
    return state == ReleaseJobState::Queued || state == ReleaseJobState::Resolving ||
           state == ReleaseJobState::Downloading;
}

// A job can be cancelled while it is queued, paused, transferring, extracting, or in the middle of a
// pause request. Pausing is cancellable even though it is not pausable: the pause already raised the
// cancel flag, and cancelling drops the pause flag so the worker lands on Cancelled instead of Paused.
inline bool releaseJobCanCancel(ReleaseJobState state) {
    return releaseJobCanPause(state) || state == ReleaseJobState::Paused ||
           state == ReleaseJobState::Extracting || state == ReleaseJobState::Pausing;
}

// A job is "active" while its worker is running (including the wind-down for a pause or cancel).
inline bool releaseJobActive(ReleaseJobState state) {
    return state == ReleaseJobState::Resolving || state == ReleaseJobState::Downloading ||
           state == ReleaseJobState::Pausing || state == ReleaseJobState::Extracting ||
           state == ReleaseJobState::Cancelling;
}

// The terminal state a finished worker settles on from its control flags. A pause request wins over a
// cancel (the worker kept its checkpoint for resume); a cancel with no pause is Cancelled; neither is
// Failed. This is the production mapping in pumpReleaseJobs()'s finished handler, hoisted here so the
// Pausing -> requestCancel -> Cancelled path is testable without a Shell.
inline ReleaseJobState releaseJobSettledState(bool paused, bool cancelled) {
    return paused ? ReleaseJobState::Paused
                  : cancelled ? ReleaseJobState::Cancelled
                              : ReleaseJobState::Failed;
}

// The release-job row-id prefix shared by the list rows ("download:release:<id>") and the detail page's
// first row ("download:release:<id>:status"). Kept here so row decoding is testable.
inline const QString& releaseJobRowPrefix() {
    static const QString prefix = QStringLiteral("download:release:");
    return prefix;
}

// Decodes a release-job row id back to its job id, or empty for any other id. Accepts both the list row
// ("download:release:<id>") and the detail page's status row ("download:release:<id>:status"); an empty
// id or a foreign prefix (SteamCMD, Steam's own queue) returns empty.
inline QString releaseJobIdFromRow(const QString& rowId) {
    if (!rowId.startsWith(releaseJobRowPrefix())) return {};
    return rowId.mid(releaseJobRowPrefix().size()).section(':', 0, 0);
}

// The flags one in-flight worker reads. `cancel` stops the transfer; `pausing` distinguishes a pause
// (keep the checkpoint for resume) from a cancel (discard it). requestCancel() is what the Back/Esc
// affordance and the detail-page Cancel row use: it always clears `pausing`, so a job that was being
// paused when cancelled ends up Cancelled, never Paused.
struct JobControl {
    std::atomic_bool cancel{false};
    std::atomic_bool pausing{false};

    bool requestPause() {
        std::lock_guard lock(extractionMutex);
        if (extracting) return false;
        pausing.store(true);
        cancel.store(true);
        return true;
    }

    void requestCancel() {
        std::lock_guard lock(extractionMutex);
        pausing.store(false);
        cancel.store(true);
    }

    void requestShutdown() {
        std::lock_guard lock(extractionMutex);
        if (!cancel.load()) {
            pausing.store(true);
            cancel.store(true);
        }
    }

    bool beginExtraction() {
        std::lock_guard lock(extractionMutex);
        if (cancel.load()) return false;
        extracting = true;
        return true;
    }

private:
    std::mutex extractionMutex;
    bool extracting = false;
};

} // namespace xmb
