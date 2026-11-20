#include "steam_library.h"

#include <cstdio>

using namespace xmb;
static int failed = 0;
#define CHECK(x) do { if (!(x)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #x); ++failed; } } while (0)

int main() {
    // A launch can be invisible while Steam starts/restarts, then become running.
    SteamTrackState state;
    auto t = steamTrackObservation(state, SteamAppStatus::Unavailable);
    CHECK(t.unavailable && t.state.consecutiveStopped == 0 && !t.confirmedStopped);
    t = steamTrackObservation(t.state, SteamAppStatus::Running);
    CHECK(t.confirmedRunning && t.state.seenRunning);

    // One clear is only debounce evidence; an interrupted clear streak is not an exit.
    t = steamTrackObservation(t.state, SteamAppStatus::Stopped);
    CHECK(!t.confirmedStopped && t.state.consecutiveStopped == 1);
    t = steamTrackObservation(t.state, SteamAppStatus::Unavailable);
    CHECK(t.unavailable && t.state.consecutiveStopped == 0 && !t.confirmedStopped);
    t = steamTrackObservation(t.state, SteamAppStatus::Running);
    CHECK(t.state.seenRunning && t.state.consecutiveStopped == 0);

    // Absent/malformed registry observations are represented as unavailable and never
    // become a stopped result merely because the client is signed out/restarting.
    t = steamTrackObservation(t.state, SteamAppStatus::Unavailable);
    CHECK(!t.confirmedStopped);
    t = steamTrackObservation(t.state, SteamAppStatus::Unavailable);
    CHECK(!t.confirmedStopped && t.state.consecutiveStopped == 0);

    // Updating is known-but-not-stopped and also interrupts the clear streak.
    t = steamTrackObservation(t.state, SteamAppStatus::Stopped);
    CHECK(t.state.consecutiveStopped == 1);
    t = steamTrackObservation(t.state, SteamAppStatus::Updating);
    CHECK(!t.confirmedStopped && t.state.consecutiveStopped == 0);

    // Genuine exit: two consecutive confirmed stopped observations.
    t = steamTrackObservation(t.state, SteamAppStatus::Running);
    t = steamTrackObservation(t.state, SteamAppStatus::Stopped);
    CHECK(!t.confirmedStopped);
    t = steamTrackObservation(t.state, SteamAppStatus::Stopped);
    CHECK(t.confirmedStopped && !t.state.seenRunning && t.state.consecutiveStopped == 0);
    return failed == 0 ? 0 : 1;
}
