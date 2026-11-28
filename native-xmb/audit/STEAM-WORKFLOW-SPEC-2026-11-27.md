# Steam workflow specification (draft, 2026-11-27)

Status: DRAFT for owner review. Nothing here is implemented by this document. Baseline is v0.3.42 (`b5cdec7`).
Owner decisions so far (2026-11-27): controller-first install (D1, mechanism pending spikes), show non-Steam shortcuts
(D3), drop Family Shared (D2), PS-arbitration row in Game Settings (D5), in-XMB SteamCMD sign-in with the XMB holding the
password in memory and handling Steam Guard, one-time keyboard session as setup and fallback (D7), and a built-in
virtual Xbox pad ("Controller mode") for Xbox-only games with the XMB owning the PS button (D9, section 4.11).
D4 (no Quit Steam row) and D6 (Windows only) are taken as recommended; D8 is open.
HTPC facts and the spike steps are in `STEAM-HTPC-SPIKES-2026-11-27.md`.
Sources: a read-only code and docs survey of `native-xmb/src/steam*.{h,cpp}`, `steamcmd*`, the Steam sections of
`shell.cpp`, `launch_cover.*`, the in-game code, and the audit docs `STEAM-*.md`, `STEAMCMD.md`, `GAME-LAUNCH-FADE.md`,
`INGAME-*.md`. Line numbers are deliberately left out: they drift.

## 1. Goal and principles

The XMB is the HTPC's front end. A person on the couch with a controller or the BD remote must be able to, without ever
touching the mouse or the Steam window: see their library, install or update a game, start it, leave it, and come back
to the XMB, for whichever Steam account is signed in.

Principles every requirement below is written against:

1. **Truthful.** A row, count or message states only what the app knows. Unknown stays unknown. No fake progress, no
   claimed sign-in, no empty list dressed up as "nothing owned".
2. **Follows the signed-in account.** Everything account-specific (library, collections, play records, artwork
   overrides, family games) follows `ActiveUser` with no stale data, whether the switch happened in the XMB or in Steam.
3. **Works without a key, better with one.** The keyless local list is a real feature, not an error state. The Web API
   key upgrades it to the complete owned list.
4. **Steam stays the authority.** The XMB reads Steam's files and registry and hands work to the Steam client. It never
   types a password, never stores one, and writes to Steam's own files only for the account switch.
5. **Verified on the real HTPC, not only on fixtures.** Today almost everything outside the render gate is verified
   against stand-ins. This spec ends with the matrix that changes that (section 7).

## 2. Where we are (summary of the survey)

| Stage | Today | Real-Steam verified |
|---|---|---|
| Detect Steam root | registry, Program Files, `--steam-root`, env; Linux read-only discovery | partly (dev PC and HTPC read-only) |
| Accounts and active account | registry `ActiveUser`, `MostRecent`, `AutoLoginUser`, newest timestamp, sole account | read-only checks only |
| Account switch (Users > Steam Account) | close Steam, edit `loginusers.vdf` and registry, restart, verify | **never run against a real client** |
| Owned library | Web API list (key) > keyless local list > installed manifests; collections; family file | keyless scan read-only on dev PC |
| Artwork | local librarycache and `userdata/<id>/config/grid`, CDN fallback, logo tiles | rendering yes; override files not traced |
| Install / update / validate | SteamCMD jobs or `steam://install` hand-off; update/validate via SteamCMD | **SteamCMD only against a fake** |
| Uninstall | `steam://uninstall` hand-off only | not run |
| Downloads | Download Management page, live row detail, completion toast; no pause or resume | scanner read-only; no automated tests |
| Launch | `steam.exe -applaunch`, launch cover, registry tracking | works on the HTPC in practice (log shows a real run); not under a test matrix |
| In-game XMB | PS tap or hold over the running title, live DComp overlay, Quit Game stages | not verified with real games across modes |
| Store | featured categories, search, item pages; no key needed | no automated tests |
| Background / kiosk | hidden client start, window sweep | **"nothing has run against a real client"** |
| Friends | Discord only; Steam friends removed | n/a |

Known gaps already visible: no pause/resume of Steam downloads, no SteamCMD uninstall, the family-shared file has no
known producer, no support for Steam's own hidden games, Add-a-Game has no edit/remove, and the docs contradict the
code in nine places (section 8).

## 3. Title state model (the spine of the spec)

Every Steam title has exactly one state at a time, and every surface (row detail, Triangle options, info page, toast)
derives from it. Today the state is spread across `Entry` fields and several pollers.

| State | How it is known | Row detail line | Primary action (Cross) |
|---|---|---|---|
| Not owned (store item) | store page only | price/none | open store item page |
| Owned, not installed | owned list minus manifests | "Not installed" | Install |
| Queued / Installing | Steam download queue | "Installing 43%" | open Download Management |
| Update available | manifest `StateFlags` / `BytesToDownload` | "Update required" | Update |
| Updating | Steam download queue | "Updating 43%" | open Download Management |
| Installed | complete manifest | collection label only (no play time or date: owner removed them) | Launch |
| Launching | launch tracking | "Starting..." | none (ignore second press) |
| Running | registry `Running=1` | "Running" | return to game / in-game XMB |
| Stopping | quit stages in progress | "Closing..." | none |
| Unavailable | manifest or launch error | the specific reason | show reason |

- **R-S1.** One reducer produces the state (extend the existing `steamTrackObservation` reducer style, pure and
  unit-tested). Rows, options and toasts read it; they do not re-derive it.
- **R-S2.** "Update available" is a first-class state. Today the XMB has no way to show that an installed game is out of
  date (it can only run Update with SteamCMD or the Steam client).
- **R-S3.** Unavailable always names its reason in the user's words ("Needs Steam to be signed in", "Steam is offline",
  "Not enough disk space on <drive>", "Steam did not confirm this title started").

## 4. Workflow requirements by stage

### 4.1 Detect Steam and its health
- **R-D1.** The Game column has a single Steam health model: `NotInstalled`, `Installed-NotRunning`, `Starting`,
  `Running-SignedOut`, `Running-SignedIn(account)`, `Offline-Mode`. It is read-only and cheap (registry plus process
  check) and is shown once, in the Steam hub detail.
- **R-D2.** Every message that today says "Steam could not be started" distinguishes: no `steam.exe`, start refused,
  signed out, offline.
- Acceptance: each health state has a unit test of the reducer and one scripted capture against the stand-in.

### 4.2 Accounts and sign-in
- **R-A1.** The XMB never enters credentials. Signing in a new or non-remembered account is done in Steam's own window;
  the XMB raises it, says so, and follows the result (already true for the signed-out case).
- **R-A2.** Switching accounts is offered only for accounts with `RememberPassword=1`; others show "Needs sign-in in
  Steam" with a row that raises Steam's login window.
- **R-A3.** After any account change (XMB switch, Steam-client switch, restart) the library, collections, play records,
  artwork overrides and family games match the new account within one refresh, with no stale rows. (Implemented; needs
  real-client verification, T4 and T5.)
- **R-A4.** If the signed-in Steam account differs from the account whose library the XMB shows (race during a switch),
  Launch is disabled with "Steam is signed in as a different account" rather than launching a game the account may not
  own.
- **R-A5.** The Steam Account page shows which account is active, which are switchable, and the switch outcome in plain
  words; it never claims a sign-in it did not observe.

### 4.3 Library: sources and honesty
- **R-L1.** Source priority is fixed and shown: Web API list (key) > keyless local list > installed manifests. The hub
  row says which one is on screen ("288 owned" vs "N from this PC's Steam data").
- **R-L2.** Without a key, the "Not Installed" count is labelled as partial ("games you have played or collected") and
  the API Keys page is one Cross away from that row.
- **R-L3.** Titles sort by default most-recently-played first, then title; the Sort panel's seven modes stay.
- **R-L4.** Steam collections: Favorites, user collections and Hidden come from Steam's own cloud-storage data. The
  XMB overlay (`steam-collections-local.json`) is clearly separate: changes made in the XMB are local to the XMB and
  never written into Steam's files. (Whether Steam's own `hidden` and `favorite` entries are read as such is
  **to verify** against the real file; the render gate already shows a "Hidden" folder.)
- **R-L5.** Hidden games (Steam's own) are excluded from the main Game column and appear only inside the Hidden folder.
- **R-L6.** Non-Steam shortcuts (**decided D3: show them**): the account's `userdata/<id>/config/shortcuts.vdf` (the
  HTPC has one) lists emulators and other apps added to Steam. They appear as game rows in the Game column, ordered with
  the others, launched through Steam's shortcut id (`-applaunch` with the shortcut's app id, or `steam://rungameid/`
  with the 64-bit shortcut id; the exact form is verified in T1), with their name and icon from the shortcut and
  Steam's grid artwork when present. They follow the signed-in account and are labelled as non-Steam in the info page.
  They are never mixed into owned-games counts.
- **R-L7.** Family-shared titles (**decided D2: drop**): remove the Family Shared folder and the `family-shared*.json`
  reader's UI until a real data source exists, so no always-empty folder is shown. (Research item: whether Steam's local
  data lists borrowed titles for the signed-in account; revisit only if it does.)

### 4.4 Artwork
- **R-W1.** Resolution order per title: user override > Steam librarycache > grid folder > CDN (once, cached) >
  generated card. Logo tile for the focused/column look, header as fallback, hero as backdrop.
- **R-W2.** Trace and either wire or delete `steam_overrides` (`<data>/artwork/<appid>[_icon|_bg].png`): the docs call
  per-game ICON0/PIC1 "future work" and the survey could not confirm the shell uses the overrides.
- Acceptance: a fixture title per resolution rung; a missing image never leaves a blank tile.

### 4.5 Install, update, validate, uninstall
- **R-I1.** **Controller-first.** Owner decision (D1): the Steam-client hand-off pulls the user out of the XMB into a
  dialog that needs a mouse and keyboard, so it cannot be the primary path. Install, update and validate must complete
  from the XMB with a controller or the BD remote. The mechanism is **not yet settled**: section 4.5.1 lays out the
  candidates and the spikes that decide between them. Until a spike proves a path, the existing behaviour stays
  (SteamCMD when set up, Steam-client hand-off otherwise) and the hand-off row says plainly that it needs Steam's own
  window.
- **R-I2.** Whatever the mechanism, the XMB shows Steam's real progress (queue or SteamCMD output), never invented
  progress, and the title state model (R-S1) is the single source for the row.
- **R-I3.** Uninstall has no SteamCMD equivalent for a client-managed game. Candidates: delete the install folder and
  its `appmanifest_<id>.acf` after an XMB confirmation (only when Steam is not running that title and the client is
  not mid-operation), or the `steam://uninstall` hand-off when a keyboard-free path exists (spike S4). Never delete
  anything without the existing "this cannot be undone" confirm, and never touch a library folder other than the
  title's own.
- **R-I4.** Install location: shown only with more than one library folder; the chosen folder is passed on the hand-off
  where Steam supports it, otherwise the row explains that Steam decides.
- **R-I5.** Download Management shows Steam's queue (title, state, bytes, percent), release jobs and SteamCMD jobs in
  one list. Pause and resume are not offered (Steam gives no external control); the page says so once.
- **R-I6.** Completion: a toast and a library refresh when a tracked download finishes (implemented in v0.3.40;
  needs a test and real-Steam verification, T7).
- **R-I7.** Disk space: before an install hand-off the XMB compares `SizeOnDisk`/download size (when known from the
  store or manifest) with free space on the target library and warns, but does not block.

#### 4.5.1 Controller-only install: candidates, risks, spikes

The goal is "press Cross on a not-installed title, watch it install, play it", with the XMB in front the whole time.

| Candidate | How | Controller-only? | Main risks |
|---|---|---|---|
| **A. SteamCMD** (exists, fake-tested only) | `+force_install_dir <library root> +login <name> +app_update <id> validate`, XMB parses output | yes | login and Steam Guard (below); same-account session conflict with the running client; the client must see the result |
| **B. Client console command** | the Steam client's own `app_install`/`app_update` console commands, if they can be invoked without typing (launch args, `steam://` form) | yes, if it exists | unverified (`STEAM-CLI.md`); may still show a confirm dialog |
| **C. Hand-off + Big Picture** | `steam://install/<id>` while Steam runs in Big Picture, whose dialogs a controller can answer | partly (Steam takes over the screen) | breaks the "XMB stays in front" goal; needs Steam Input to be active |
| **D. Synthesised key into Steam's dialog** | send Enter to the install dialog | yes | fragile (Steam's UI is not an accessible window); last resort |

Questions that decide between A and B (each is a spike, section 7):

1. **Login (S1, decided D7).** SteamCMD needs `+login <name>`; the first login needs the password and a Steam Guard
   confirmation, later logins reuse a cached token. Owner decision: the XMB **may hold the password in memory** for
   the login, and it **handles Steam Guard itself**; a **one-time keyboard session** in a visible SteamCMD console
   stays available as the first-setup path and as the fallback when the in-XMB login does not work (section 4.5.2).
2. **Same-account session conflict (S2).** If SteamCMD logging in as the account the client is signed in as signs the
   client out ("logged in elsewhere"), A is unusable while the client runs. Workarounds: pause the hidden client
   around an install (only when no game runs), or use a dedicated Steam account for SteamCMD (not for installing the
   user's own library).
3. **Does the client adopt the install (S3)?** SteamCMD writes `steamapps/appmanifest_<id>.acf` under the
   `force_install_dir`; the client sees it only if that folder is a registered library (`libraryfolders.vdf`) and the
   manifest is complete and consistent. Needs a real run and a check that Steam does not re-download or flag
   "update required".
4. **Update detection without a network call (S5).** `appinfo.vdf`, which the keyless library already parses, holds the
   latest public `buildid` per app; the installed `appmanifest` holds the installed `buildid`. Comparing them gives the
   "Update available" state locally, valid as of the last time the Steam client refreshed its app info.
5. **No dialog at all (S4).** Whether any client-side form installs without a dialog (B). If one works, it beats A:
   it uses the signed-in client, needs no second login and no session conflict.

#### 4.5.2 In-XMB SteamCMD sign-in (decided D7)

Applies only if the spikes choose SteamCMD (candidate A). The user signs in to SteamCMD from the XMB with a controller or
the BD remote, or with a keyboard once.

- **R-I8. Credential handling.** The password is typed on the XMB's masked on-screen keyboard (or a keyboard), is held
  only in memory for the duration of the login, and is then wiped. It is never: written to disk, put in a preference or
  config file, put on a command line, included in a script file, logged, shown in a message, included in a dump, or kept
  after the login call returns. The stored account stays the name only (`steamcmd/account`). Handing the password to
  SteamCMD goes through its standard input, not an argument, because arguments are visible to other processes.
  - If spike S1 shows SteamCMD does not read a password from a pipe on Windows, the options are a pseudo-console
    (ConPTY) around SteamCMD, or the keyboard-session fallback. Passing it on the command line or in a script file is a
    last resort and needs a fresh owner decision (it exposes the password to other local processes for a moment).
- **R-I9. Steam Guard.** The XMB recognises SteamCMD's prompt for each Guard type and shows the matching step:
  an emailed code, an authenticator-app code (both typed on the on-screen keyboard), or "approve this sign-in on your
  phone" (the XMB waits, with a cancel, and says so). Prompt wording is not documented, so the recognised strings come
  from S1 against the real SteamCMD, and the parser fails closed: an unrecognised prompt aborts the login with "SteamCMD
  asked for something the XMB does not understand" and offers the keyboard-session fallback.
- **R-I10. Failure and rate limits.** Wrong password, wrong or expired code, rate limiting and timeouts each end the
  login with a specific message. The XMB never retries a password automatically and allows one attempt per user action,
  so it cannot trigger Steam's rate limit or a lockout. A password is never re-sent from memory after a failure.
- **R-I11. Cached login.** After a good login the XMB uses `+login <name>` with no password. When Steam rejects the
  cached login it says "SteamCMD needs you to sign in again" and offers the in-XMB sign-in and the keyboard session. How
  long the cache lasts is measured in S1.
- **R-I12. Testing.** The stand-in SteamCMD gains scripted prompt variants for every Guard type and failure above, and
  the tests assert the password never reaches a log line, an argument list, a dump or a file (a canary value is
  searched for everywhere the XMB writes).
- **R-I13. Cancel and back-out.** Back at any step abandons the login, wipes the password and leaves SteamCMD's stored
  state as it was.

Decision gate: run S1-S5 on the HTPC with a small free title. Outcome table:

| S4 (client command) | S1+S2+S3 (SteamCMD) | Primary path |
|---|---|---|
| works | any | **B**, SteamCMD optional |
| fails | all pass | **A** |
| fails | S2 fails | A with "pause the client around installs" if acceptable, else C |
| fails | S1 or S3 fails | C, with the XMB documenting that Steam's own screen takes over |

### 4.6 Launch
- **R-X1.** Launch sequence: state check (R-D1, R-A4) > launch cover > ensure client (hidden start if allowed) >
  `-applaunch` with the user's launch options > tracking > return. Windows uses `-applaunch`; Linux the `steam://`
  URI.
- **R-X2.** Launch options (max 1024 characters) are stored in the XMB overlay, shown on the info page, and are never
  logged.
- **R-X3.** Failure paths each end in a visible message and an unchanged XMB: Steam missing, sign-in needed, launch not
  confirmed within the wait, status unreadable, second press ignored.
- **R-X4.** The launch cover is cosmetic: if it fails or `XMB_LAUNCH_COVER=0`, launch still works.

### 4.7 In-game and return
- **R-G1.** PS tap or hold over a running Steam title opens the in-game XMB. PS arbitration with Steam's Guide button
  (**decided D5: yes**) becomes a visible row in Game Settings, default "XMB handles the PS button"; the other choice
  leaves the PS tap to Steam. Today it is only the preference key `input/psArbitration`.
- **R-G2.** Quit Game stages (close request, terminate, kill) apply only to the game's own processes under its install
  folder, never to Steam; the fallback `+app_stop` is used only when no game process is found. After quit, the XMB
  regains focus and rescans.
- **R-G3.** `runningTitlePid()` returns 0 for Steam titles today, so in-game features that need the game pid
  (Enter Text placement, some arbitration) are unverified for Steam. Either resolve the pid from the game process found
  for quit, or document the limit.
- **R-G4.** The in-game dim level is the code default 0.8; the older design text (50%) is stale.

### 4.8 Store
- **R-T1.** Store pages need no key or account. Top sellers, new releases, specials and search stay as they are.
- **R-T2.** A store item the account already owns shows its library state (R-S1), not a generic "Install".
- **R-T3.** Add automated tests for the store parser and the item page (none today).

### 4.9 Big Picture, kiosk and background mode
- **R-K1.** The XMB is the front end; Big Picture is never started by the XMB. The `steam/background` setting keeps the
  Steam client hidden (`-silent -nofriendsui -nochatui`), and a signed-out client is never hidden.
- **R-K2.** Window sweep: stray Steam windows (store popups, updates, "Special Offers") are hidden only during a launch
  window and are restored when the user leaves the XMB. Real window class and title behaviour is **unverified**.
- **R-K3.** No "Quit Steam" is offered unless a decision to add it is made. **DECISION D4.**

### 4.10 Friends
- Out of scope: the Friends column is Discord only. Steam friends code remains a CLI report; its stale docs are fixed.

### 4.11 Controller mode (Xbox compatibility) (owner choice C, 2026-11-27)

Problem: Xbox-only games (for example Black Ops 3) do not see a DualSense, and Steam Input fixes that only by taking the
PS button. The XMB already reads every DualSense report, so it can present a **virtual Xbox 360 pad** to the game while
keeping the PS button for itself.

- **R-C1. Behaviour.** While a title launched from the XMB runs, the XMB exposes one virtual Xbox 360 controller whose
  state follows the physical DualSense. The PS button is never forwarded; it stays with the XMB (tap or hold opens the
  in-game menu). Steam Input is not involved and is not needed.
- **R-C2. Mechanism.** A virtual-gamepad driver. ViGEmBus is the only maintained-in-practice option known to the spec,
  but the project is retired (no further updates), so its use on this Windows build is a spike (C1), not an assumption.
  The XMB talks to the driver through the small client library (loaded at runtime; absent driver means the feature is
  "not installed", never an error at start).
- **R-C3. Lifetime.** The virtual pad is created **before** the game starts (games enumerate pads at startup) and
  destroyed when the game exits or the title is left. It does not exist on the desktop or while the XMB is navigating, so
  nothing else sees an extra pad. The XMB's own navigation keeps reading the DualSense directly.
- **R-C4. Mapping.**

  | DualSense | Virtual Xbox 360 |
  |---|---|
  | Cross, Circle, Square, Triangle | A, B, X, Y |
  | L1, R1 | LB, RB |
  | L2, R2 (analog) | LT, RT (0-255) |
  | L3, R3, sticks, D-pad | the same |
  | Create, Options | Back, Start |
  | PS | not forwarded (XMB) |
  | Touchpad click, Mute | not forwarded |

- **R-C5. Rumble.** Games send XInput rumble (large and small motor) to the virtual pad; the XMB forwards it to the
  DualSense as an output report (USB report `0x02`, Bluetooth report `0x31` with its sequence tag and CRC-32). Phase 2:
  the first version may ship without rumble if the Bluetooth output path is not proven (C4). A malformed output report
  must never leave the pad in a bad state: write only on a rumble change and stop writing on any error.
- **R-C6. Steam Input conflict.** If Steam Input for PlayStation controllers is on for the running title, Steam also
  presents a pad and the game sees two. The XMB reads Steam's setting read-only and shows "Steam Input is on for this
  game; turn it off to avoid double input" on the Controller row. It never edits Steam's settings.
- **R-C7. Other pads.** A physical Xbox pad that is also connected keeps its own XInput slot; the virtual pad takes the
  next free slot. Which one a game uses is the game's choice (spike C5). Games with native DualSense support see the real
  pad as well as the virtual one; the per-game override (R-C8) turns the mode off for them.
- **R-C8. Settings.** Game Settings gets a **Controller Mode** row: Off / On, default On while the driver is installed,
  with a per-game override stored in the XMB overlay file (like launch options). The row also shows the driver state
  (Not installed / Ready / Active / Error) and the Steam Input warning (R-C6).
- **R-C9. Driver install (owner decision 2026-11-27: ViGEm is part of the XMB).** The release ships the vendor's signed
  ViGEmBus installer (v1.22.0, BSD-3-Clause, SHA-256 pinned in `packaging/vigem-driver.json`, Authenticode signer
  Nefarius Software Solutions e.U.) in the zip (`drivers\`) and in the setup exe. The installer binary is never
  committed: packaging fetches it from the pinned URL and fails on a hash or signature mismatch. The setup exe offers an
  optional task that is on by default only when the ViGEmBus service is missing (an existing 1.21.442 or newer is left
  alone). The Controller Mode row's "Install controller driver" action runs the bundled file after a confirmation naming
  file and version, re-verifying hash and signature just before; it never downloads at run time. The UAC prompt needs a
  keyboard or mouse once; the XMB itself never runs elevated. The client library (ViGEmClient v1.21.222.0, MIT) is
  vendored under `native-xmb/third_party/ViGEmClient` and built into the app on Windows only. Both upstream projects are
  archived (2023), which stays a recorded risk (R-C2).
- **R-C10. Truthful states.** The row never says "Active" unless the virtual pad exists, and a failed create
  (driver missing, access denied, slot unavailable) is shown with its reason, with the game launching normally anyway.
- **R-C11. Testing.** The mapping, the state machine and the rumble report builder (including the CRC) are pure and
  unit-tested in CI. Creating a real virtual pad needs the driver, so that part is an HTPC test, read back with a
  plain XInput reader.

Phasing: P1 create/destroy per launch plus mapping (no rumble); P2 rumble; P3 per-game override and the Steam Input
warning; P4 optionally hide the physical pad from games (HidHide) if double input turns out to matter (**decision D8**).

Spikes (section 7): C1 the driver installs and works on the HTPC's Windows build and an unelevated process can create a
pad; C2 Black Ops 3 sees the virtual pad with Steam Input off; C3 the PS button stays with the XMB and Steam does
nothing; C4 rumble reaches the DualSense over Bluetooth; C5 which pad a game picks when a real Xbox pad is also connected.

## 5. Settings and configuration surface

Keep a single place: **Settings > Network Settings > API Keys** for keys, **Game Settings** for Steam options
(background mode, PS arbitration if D5 says yes, SteamCMD). The environment variables (`XMB_STEAM_*`) stay test seams and
are listed once in `STEAM-CLI.md`; user-facing behaviour must never need one.

## 6. Open decisions (owner)

| # | Decision | Status |
|---|---|---|
| D1 | Install / update / uninstall path | **DECIDED (owner): controller-first.** The client hand-off needs a mouse and keyboard, so SteamCMD is the intended primary path, but the mechanism needs the spikes in 4.5.1 before it is locked |
| D2 | Family-shared titles | **DECIDED: drop the folder** until a real data source exists |
| D3 | Non-Steam shortcuts from `shortcuts.vdf` | **DECIDED: show them** in the Game column |
| D4 | "Quit Steam" row | **taken as recommended (2026-11-27, work started): no**, the client stays an invisible helper |
| D5 | PS-button arbitration setting in the UI | **DECIDED: yes**, Game Settings, default "XMB handles the PS button" |
| D6 | Linux scope for this spec | **taken as recommended (2026-11-27, work started): Windows only here**, Linux stays read-only discovery |
| D8 | Controller mode: also hide the physical DualSense from games (HidHide)? | open; recommend no for now (phase 4), decide after C2 and C5 show whether double input is a real problem |
| D9 | Controller mode for Xbox-only games | **DECIDED (owner): option C**, a built-in virtual Xbox pad with the XMB owning the PS button (4.11) |
| D7 | SteamCMD sign-in | **DECIDED (owner): the XMB may hold the password in memory (never stored or logged) and handles Steam Guard itself; a one-time keyboard session is fine as first setup and as the fallback.** Rules in 4.5.2 |

## 7. Verification plan (real HTPC)

Nothing below needs new code to *start*; each item is run by the owner at the TV (games start) with read-only evidence
collected over SSH (log lines, registry reads, screenshots). Every item records the Steam version, the XMB version and
the account used (never names or ids in the repo).

| ID | Scenario | Evidence |
|---|---|---|
| T1 | Launch with Steam not running, signed in (auto-login) | log launch lines, time to running, cover timing |
| T2 | Launch with Steam already running | same |
| T3 | Launch when another account is signed in (R-A4) | message shown, no launch |
| T4 | Switch account in the XMB (remembered account) | log, library swap, no stale rows |
| T5 | Switch account inside the Steam client | library follows within the poll interval |
| T6 | Install an owned game via the hand-off | download rows, percent, completion toast |
| T7 | Update an out-of-date game | R-S2 state shown |
| T8 | Uninstall via hand-off | row moves to Not installed |
| T9 | Quit Game from the in-game XMB with a real game | stage log, focus return |
| T10 | In-game XMB in windowed, borderless, exclusive fullscreen | which modes work |
| T11 | Hidden client start at boot (kiosk) | which windows appear |
| T12 | No key vs key: library on screen | hub detail text |
| T13 | Steam offline mode / no network | messages |
| T14 | Steam not installed | Game column state |
| T15 | A non-Steam shortcut launches from the XMB (R-L6) | launch form that works, tracking |

Spikes that decide the install mechanism (4.5.1), run with one small free title, owner at the TV for anything that
signs in or installs:

| ID | Spike | Evidence |
|---|---|---|
| S1 | SteamCMD sign-in, in three parts: (a) does it read a password and a Guard code from a pipe on Windows or only from a console (decides stdin vs ConPTY); (b) the exact prompt text for each Guard type (email code, app code, phone approval) and each failure; (c) after one good login, does `+login <name>` run with no prompt, and how long does that last | SteamCMD output with the password and codes redacted, whether the call blocks on a pipe, prompt strings |
| S2 | Client signed in as account A while SteamCMD logs in as A: is the client disconnected? | client state before and after, XMB log |
| S3 | SteamCMD install into an existing registered library: does the client list the game as installed with no re-download and no "update required"? | `appmanifest` fields, client view, library scan |
| S4 | Any client-side install form that needs no dialog (console command, `steam://` variants): which exist and what appears? | what the client shows, log lines |
| S5 | Local "update available" check: installed manifest `buildid` against the public `buildid` in `appinfo.vdf`; how fresh is the cached app info? | values for a game known to have an update pending |
| C1 | Controller mode: does the virtual-gamepad driver install and work on this Windows build, and can the unelevated XMB create a pad? | installer result, a pad visible to an XInput reader |
| C2 | Black Ops 3 with Steam Input off for PlayStation controllers: does it see the virtual pad and respond? | in-game result, XInput reader |
| C3 | PS button with the virtual pad active: XMB in-game menu opens, Steam does nothing | observed behaviour, Steam overlay state |
| C4 | Rumble from the game reaches the DualSense over Bluetooth | felt result, output report log |
| C5 | A real Xbox pad is also connected: which pad does the game take, and does the virtual pad get a sane slot? | XInput slot list, in-game result |

Automated coverage to add (no tests exist today): `scanSteamDownloads` and the Download Management rows, the live
download poll and toast, `SteamStore` parsing and the item page, `SteamOwned` response parsing, `steam_family`,
`steam_activity`, `steam_overrides`, `sortGameEntries`, the collections menu and launch-options UI, the game info page.
Scripted e2e for the title state model against the stand-in.

## 8. Stale documentation to fix (from the survey)

1. `STEAM-INTEGRATION.md` and `STEAM-CLIENT-FRIENDS.md` still describe Steam friends in the Friends column.
2. `STEAM-INTEGRATION.md` says the owned list needs a key in the launch environment; the code also reads the saved key
   file and has a keyless fallback.
3. `STEAMCMD.md` says games launch through `steam://rungameid`; on Windows it is `steam.exe -applaunch`.
4. The Game-column shape in `STEAM-INTEGRATION.md` (separate Store row and All Games folder) is now one `steam-hub`.
5. `INGAME-XMB-DESIGN.md` and `PS-INGAME-HOOK.md` still say Steam titles quit with `+app_stop`; the code uses process
   stages.
6. `INGAME-XMB-DESIGN.md` says dim 50%; the code default is 0.8.
7. "Never alters Steam" wording in `STEAM-INTEGRATION.md` / `STEAM-ACCOUNTS.md` predates the account switch.
8. `HANDOFF-2026-11-25.md` names version 0.3.36; the tree is 0.3.42.
9. Commit `213ea58` calls the Linux discovery test "not in CI" while `tests.yml` runs it.

## 9. Suggested order of work (after approval)

1. Run T1-T15 and the spikes S1-S5 on the HTPC (owner at the TV). Fix what they find first; they decide how much of the
   rest is real, and S1-S5 lock the install mechanism (D1).
2. Title state model (R-S1..S3) with unit tests, then rows and options read it. "Update available" uses the S5 result.
3. Account-mismatch guard (R-A4) and the Steam health model (R-D1).
4. Controller-first install, update, validate and uninstall on the mechanism the spikes chose, with progress from the
   real source.
5. Hidden games, non-Steam shortcuts (D3), drop Family Shared (D2), PS-arbitration row (D5).
6. Controller mode (4.11), after spikes C1-C5: phase 1 first, because it fixes the owner's Black Ops 3 problem.
7. Tests for the uncovered modules; doc cleanup (section 8).
