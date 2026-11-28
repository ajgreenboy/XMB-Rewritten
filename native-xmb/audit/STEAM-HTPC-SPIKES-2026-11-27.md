# Steam workflow: HTPC spike runbook (2026-11-27)

Companion to `STEAM-WORKFLOW-SPEC-2026-11-27.md` (sections 4.5.1, 4.5.2, 4.11 and 7). Purpose: settle the install
mechanism (S1-S5) and the controller-mode risks (C1-C5) on the real HTPC before the code that depends on them is built.

Rules for every spike:
- Use **one small free game** for anything that installs, so nothing owned is at risk.
- **No passwords, Steam Guard codes or keys ever go in chat, in a log or in the repo.** Where a login is needed the
  owner types it at the HTPC (keyboard) or over their own SSH session, and logs are redacted before they are shared.
- Evidence is log lines, registry reads and screenshots; account names and SteamIDs are masked in anything kept.
- The HTPC is changed only where a step says so, with the old state recorded first.

## What is already known (read-only probe, 2026-11-27)

| Fact | Value |
|---|---|
| Windows | 11 Pro, build 26300 |
| Steam client | 2.10.91.91 (`steam.exe` 11.23.65.88) |
| SteamCMD | **not installed** (no `steamcmd.exe` found; no `steamcmd/*` preference set) |
| Virtual gamepad driver | **ViGEm Bus Driver 1.21.442 installed and running** ("Nefarius Virtual Gamepad Emulation Bus") |
| Pad emulators/remappers | none installed (no DS4Windows, HidHide, reWASD) |
| DualSense | connected over Bluetooth |
| Steam Input settings | none recorded in `config.vdf` (Steam defaults) |
| Black Ops 3 (app 311210) | install not found in the default library folders; the owner says which drive |
| XMB on the HTPC | v0.3.34 (`4cc24ba`) |

Update 2026-11-27 (later): the owner approved bundling ViGEm into the XMB. The HTPC keeps its existing 1.21.442 driver
(the bundled v1.22.0 installer only installs when the service is missing). Black Ops 3's location was skipped for now.

Consequences: C1 is half answered (driver present and running on this Windows build); S1-S3 need SteamCMD set up first;
the XMB on the HTPC is older than anything built from this spec.

## Controller mode checks (C1-C5): do these first, they fix Black Ops 3

Prerequisite: a test build of the controller-mode core (phase 1) on the HTPC. Until it exists, C1 can still be
checked with a stand-alone XInput reader.

| ID | Steps | Pass |
|---|---|---|
| C1 | Confirm an **unelevated** process can create a virtual Xbox pad (the XMB test build's "create pad" check, or a tiny stand-alone tool), then read it back with an XInput reader | one extra XInput pad appears and disappears with the process |
| C2 | Steam: leave Steam Input for PlayStation controllers **off**. Start Black Ops 3 from the XMB with Controller Mode on. In the game, move a stick and press Cross | the game responds as if an Xbox pad were used |
| C3 | With the virtual pad active and a game running, tap PS, then hold PS | XMB in-game menu opens, Steam does nothing (no overlay, no Big Picture) |
| C4 | In a game with rumble, trigger it (for example a hit or a menu rumble test) | the DualSense vibrates (Bluetooth) |
| C5 | Repeat C2 with the Xbox Wireless Controller switched on, then off | note which pad the game uses and the XInput slots |

## Install mechanism spikes (S1-S5)

Prerequisite for S1-S3: SteamCMD set up. The XMB's own "Set Up SteamCMD" downloads it from Valve, which needs the
owner's yes first (the file is `steamcmd.zip` from Valve's CDN; the XMB shows file, source and size before it runs).

| ID | Steps | What to record |
|---|---|---|
| S4 (first) | With the Steam client signed in and idle, try each client-side install form for the small free game: `steam://install/<id>` and, if Steam accepts commands, the console forms `app_install <id>` / `app_update <id>` (open the console with `steam://open/console`, type with the keyboard) | what appears: a dialog, or the download starting on its own |
| S2 | Client signed in as account A. Run SteamCMD `+login <A>` (cached or typed) then `+quit`. Look at the Steam client right after | client still signed in, or "logged in elsewhere" / offline |
| S1a | Run SteamCMD with the password on **stdin from a pipe** (a file of redacted test input first, then the real one at the keyboard) | does it read it, or only from a console |
| S1b | At the interactive login, note the **exact prompt text** for each Guard type (email, authenticator code, phone approval) and for a wrong password and a wrong code | strings, redacted |
| S1c | After one good login run `+login <A> +quit` with no password; repeat on later days | does it prompt, and after how many days it asks again |
| S3 | `+force_install_dir <a registered library root> +login <A> +app_update <free game id> validate +quit`, then open the Steam client | game shown as installed, no re-download, no "update required"; the `appmanifest` fields |
| S5 | For a game with a known pending update, read the installed manifest `buildid` and the public `buildid` in `appinfo.vdf` | the two values, and when Steam last refreshed app info |

Order: S4, then S2 (these can rule SteamCMD out), then S1, S3, S5.

## What can be done over SSH, read-only

Steam and driver state, manifests, `loginusers.vdf` flags (account ids masked), `appinfo.vdf` build ids, XInput device
listing, the XMB log. Anything that signs in, installs, launches a game or changes a setting is done by the owner at the
HTPC, or with the owner's explicit go-ahead for that step.

## Results

Fill in after each run: date, Steam and XMB versions, account label (A or B, never a name), pass/fail, evidence
location. The install-mechanism decision table in the spec (4.5.1) is applied to the S4, S1, S2 and S3 results.
