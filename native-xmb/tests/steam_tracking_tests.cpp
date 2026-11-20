#include "steam_library.h"

#include <cstdio>

using namespace xmb;
static int failed = 0;
#define CHECK(x) do { if (!(x)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #x); ++failed; } } while (0)

// The exact production launch wait (steam_library.h kSteamLaunchWaitMs); the shell and this test
// share the literal so the reducer's timeout behavior is exercised at the production value.
static constexpr qint64 kTimeout = kSteamLaunchWaitMs;

int main() {
    // --- Launch timeout: readable Steam that never confirms the run ----------------------------
    // Before the wait expires, a stopped observation keeps watching.
    SteamTrackState state;
    auto t = steamTrackObservation(state, SteamAppStatus::Stopped, kTimeout - 1, kTimeout);
    CHECK(t.action == SteamTrackAction::KeepWatching && !t.state.seenRunning);
    // The reducer owns the boundary comparison: at the timeout the launch wait expires.
    t = steamTrackObservation(t.state, SteamAppStatus::Stopped, kTimeout, kTimeout);
    CHECK(t.action == SteamTrackAction::TimedOut && t.status == SteamAppStatus::Stopped);

    // --- Launch timeout: unreadable Steam before a confirmed run -------------------------------
    // Unavailable before the wait expires keeps watching and is never a stopped observation.
    state = {};
    t = steamTrackObservation(state, SteamAppStatus::Unavailable, kTimeout - 1, kTimeout);
    CHECK(t.action == SteamTrackAction::KeepWatching && t.state.consecutiveStopped == 0);
    // A permanently unavailable client still recovers within the launch wait; the transition keeps
    // the unavailable status so the shell can report an accurate message.
    t = steamTrackObservation(t.state, SteamAppStatus::Unavailable, kTimeout, kTimeout);
    CHECK(t.action == SteamTrackAction::TimedOut && t.status == SteamAppStatus::Unavailable);

    // --- Update patience: a confirmed update before the run restarts the launch wait ----------
    state = {};
    t = steamTrackObservation(state, SteamAppStatus::Updating, kTimeout, kTimeout);
    CHECK(t.action == SteamTrackAction::RestartWait && !t.state.seenRunning && t.state.consecutiveStopped == 0);
    // Even with the clock already expired, an update never times out: it asks the shell to restart
    // the launch-wait clock instead (pollSteamGame calls steamWatchAge_.restart()).
    t = steamTrackObservation(t.state, SteamAppStatus::Updating, kTimeout, kTimeout);
    CHECK(t.action == SteamTrackAction::RestartWait);

    // --- Running confirmation and a clear streak interrupted by an update ---------------------
    state = {};
    t = steamTrackObservation(state, SteamAppStatus::Running, 0, kTimeout);
    CHECK(t.action == SteamTrackAction::ConfirmedRunning && t.state.seenRunning);
    // Running again while already cleanly running is a no-op (no changed emit).
    t = steamTrackObservation(t.state, SteamAppStatus::Running, 0, kTimeout);
    CHECK(t.action == SteamTrackAction::KeepWatching);
    // One stopped observation is only debounce evidence.
    t = steamTrackObservation(t.state, SteamAppStatus::Stopped, 0, kTimeout);
    CHECK(t.action == SteamTrackAction::Cleared && t.state.consecutiveStopped == 1);
    // An update interrupts the clear streak without being an exit.
    t = steamTrackObservation(t.state, SteamAppStatus::Updating, 0, kTimeout);
    CHECK(t.action == SteamTrackAction::KeepWatching && t.state.seenRunning && t.state.consecutiveStopped == 0);

    // --- After a confirmed run, unknown is never equated with exit ----------------------------
    t = steamTrackObservation(t.state, SteamAppStatus::Stopped, 0, kTimeout);
    CHECK(t.action == SteamTrackAction::Cleared);
    t = steamTrackObservation(t.state, SteamAppStatus::Unavailable, 0, kTimeout);
    CHECK(t.action != SteamTrackAction::Returned && t.action != SteamTrackAction::TimedOut);
    CHECK(t.state.seenRunning && t.state.consecutiveStopped == 0);
    // Repeated unavailable observations still never become a return.
    t = steamTrackObservation(t.state, SteamAppStatus::Unavailable, 0, kTimeout);
    CHECK(t.action == SteamTrackAction::KeepWatching && t.state.seenRunning);

    // --- Resuming after a clear is a fresh confirmation ---------------------------------------
    t = steamTrackObservation(t.state, SteamAppStatus::Stopped, 0, kTimeout);
    CHECK(t.action == SteamTrackAction::Cleared && t.state.consecutiveStopped == 1);
    t = steamTrackObservation(t.state, SteamAppStatus::Running, 0, kTimeout);
    CHECK(t.action == SteamTrackAction::ConfirmedRunning && t.state.seenRunning && t.state.consecutiveStopped == 0);

    // --- A clear streak interrupted by unavailable is not an exit -----------------------------
    t = steamTrackObservation(t.state, SteamAppStatus::Stopped, 0, kTimeout);
    CHECK(t.action == SteamTrackAction::Cleared);
    t = steamTrackObservation(t.state, SteamAppStatus::Unavailable, 0, kTimeout);
    CHECK(t.action == SteamTrackAction::KeepWatching && t.state.seenRunning && t.state.consecutiveStopped == 0);

    // --- Genuine exit: two consecutive confirmed stopped observations -------------------------
    t = steamTrackObservation(t.state, SteamAppStatus::Stopped, 0, kTimeout);
    CHECK(t.action == SteamTrackAction::Cleared);
    t = steamTrackObservation(t.state, SteamAppStatus::Stopped, 0, kTimeout);
    CHECK(t.action == SteamTrackAction::Returned && !t.state.seenRunning && t.state.consecutiveStopped == 0);

    return failed == 0 ? 0 : 1;
}
