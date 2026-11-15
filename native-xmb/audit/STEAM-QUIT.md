# Quit Game for a Steam title

Created 2026-11-14. Branch `wt/steamquit`.

## Root cause

Quit Game (the PS system menu row and the in-game XMB row, both through `Shell::requestQuitGame`) ended in
`Shell::quitRunningTitle`. For a Steam title that did one thing: run `steam.exe +app_stop <appid>`. The owner's HTPC
report (The Quarry, app 208650) shows the log stopping at `shell: asked Steam to stop 208650` with the game still
running and a Steam window in front. `audit/STEAM-CLI.md` only ever exercised `+app_stop` against a nonexistent app
(table row "Command accepted; only tested against a nonexistent app, never against a live game"), so the claim that
it stops a live game was never verified, and on the owner's client it does not. The second `steam.exe` invocation
also raised Steam's own UI. That is the cause as far as the log shows; the client's behaviour on `+app_stop` was
not reproduced here (no Steam may be run on the development PC).

## What changed

The Steam branch of `quitRunningTitle` now calls `Shell::quitSteamTitle` (`src/shell.cpp`), which closes the game's own
processes with the same three stages a non-Steam title gets. steam.exe is not started on this path.

1. **Find the game folder.** `steamInstalled_` (the existing scan of every library folder in `libraryfolders.vdf`
   and each `appmanifest_<appid>.acf` `installdir` under `steamapps\common`), or a fresh `scanSteam` when the app is not
   in that cache.
2. **Select the processes** (`src/steam_quit.cpp`, `selectGameProcesses`, pure and unit-tested): every process whose
   image path is strictly inside that folder (toolhelp snapshot plus `QueryFullProcessImageNameW`), and every
   descendant of those (launcher and anti-cheat helper children, even when their image is elsewhere). Never selected:
   the XMB itself, any process named like the Steam client (`steam.exe`, `steamwebhelper.exe`, `steamservice.exe`,
   `gameoverlayui*.exe`, `steamerrorreporter*.exe`, `steam_monitor.exe`, `streaming_client.exe` and a few more), or
   anything that only descends from one of those. A child counts only if it started no earlier than its parent, so a
   recycled parent id cannot pull in an unrelated process. Processes whose image cannot be read (protected) are still
   found through their parent.
3. **Stage 1, cooperative.** `WM_CLOSE` to the visible, unowned top-level windows of the set, then poll every 250 ms
   until the grace (`XMB_QUIT_GRACE_MS`, default 10000) ends.
4. **Stage 2, terminate.** `TerminateProcess` on the root processes of what is still alive (the processes whose
   parent is not in the set). Their children usually follow. Wait 3 s.
5. **Stage 3, kill.** `TerminateProcess` on every survivor, retried each poll for up to 5 s. If something is still
   alive (anti-cheat, higher rights) an on-screen message names the processes (names only).
6. **Orphans.** The set is re-selected on every poll and merged with the processes already known (same pid, same name,
   same image, same start time), so a helper whose parent exited is still part of the game.
7. **Return.** Steam clears `RunningAppID` / `Apps\<id>\Running` itself once the processes are gone; the existing
   registry watcher (`pollSteamGame`, two clear polls) returns the XMB with the same row selected. Nothing under
   Steam is edited. If the flag is still set 8 s after the last process ended, the log says so and the watcher stays
   in charge.
8. **Fallback.** If no process is found in the folder (a launcher stub, a different folder) or the install folder is
   unknown, the old `+app_stop` is attempted, the log says why, and the reason is shown on screen: with the stop
   error when `steam.exe` could not be started, or, 15 s later, "still running, close it from its own menu". It never
   does nothing silently.

Both entry points reach the same routine: the PS menu row calls `requestQuitGame`, the in-game `ingame:quit` row calls
`requestQuitGame` (`Action::QuitGame`). The confirmation ("Do you want to quit the game?", Yes/No, default No) is
unchanged and no pinned gate frame changed.

## Log lines

Names and counts only; the folder is named by its leaf (the game folder name).

```
ingame: Quit Game asks: Do you want to quit the game? title=The Quarry
ingame: quit stage 1 WM_CLOSE posted to 1 window(s) of 3 process(es) [TheQuarry.exe x3] in game folder "The Quarry", waiting up to 10000 ms (Steam registry running=1)
ingame: quit stage 2 terminate (the title did not exit within the grace period): 1 of 1 root process(es) ended (...), N process(es) were still running
ingame: quit stage 3 kill: 2 of 2 process(es) ended (...)
ingame: quit: every game process is gone after stage <cooperative (WM_CLOSE)|terminate|kill> title=...
returned from The Quarry (Steam reported it stopped)
ingame: quit ended by stage <...> (Steam cleared the running flag) title=...
```

## Test seams (never used by a normal run)

`XMB_STEAM_FAKE=1` with `XMB_STEAM_ROOT=<scratch fake library>`: launching a library title starts
`<install dir>\<XMB_STEAM_FAKE_EXE>` (or an absolute path) with `XMB_STEAM_FAKE_ARGS` instead of steam.exe, and the
title counts as running while any process runs from its install folder (`XMB_STEAM_FAKE_FORCE_RUNNING=1` forces
running). The fallback in this mode cannot start steam.exe because the fake root has none.

## Verification (2026-11-14, this PC, no Steam touched)

- `tests/steam_quit_tests.cpp` (CMake option `XMB_STEAM_QUIT_BUILD_TESTS`, wired in `tests.yml`): pure selection
  tests (sibling folder with a shared prefix, Steam client processes and subtree, bundled steam.exe, recycled parent
  id, orphans, protected pids, empty result) plus live tests against `tests/steam_quit_standin.cpp`, a Win32 window
  program: cooperative with following helpers (ended by stage 1 in 0.27 s), stubborn root plus follower (stage 2),
  cooperative root with orphan helpers that ignore the close (stage 2), stubborn root with orphan helpers (stage 3);
  a bystander copy outside the folder survived every scenario.
- Scripted XMB runs against a scratch fake library (`XMB_STEAM_ROOT`, `XMB_STEAM_FAKE=1`), using `pshold` then the
  Quit Game row and Yes: cooperative, stubborn plus follower, stubborn plus two orphans (ended by kill), the in-game
  row path (seven DOWN, same confirmation), and the fallback (no process found: log plus on-screen reason). Each ended
  with `returned from The Quarry`, `inGame=false`, the same row (`steam:208650`) focused, fullscreen true, and no
  stand-in process left.

## Not verified

A real Steam game: anti-cheat or protected processes, games that respawn a launcher, whether the real
`RunningAppID` clears promptly after the processes end, and whether the XMB window returns to the foreground over
a real exclusive-fullscreen game (the stand-in is a small window). Only the tracked appid's install folder is ever used,
so a game that runs from a different folder than its manifest says is reported through the fallback.

## Guide button (Steam setting, not XMB)

While a Steam game is in front and Steam Input is enabled for the controller, Steam handles the Guide tap (Big Picture
or the overlay), so turn off Steam's "Guide Button Focuses Steam" and Steam Input for Xbox controllers for the XMB to
see a tap. The hold already reaches the XMB.
