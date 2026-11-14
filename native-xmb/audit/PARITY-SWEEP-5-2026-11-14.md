# Render-parity sweep 5 (2026-11-14)

Sweep 4 walked every column at depth 0 and the Settings sub-pages. This sweep went into what it left: the sub-pages of
the Network, PlayStation Network and Friends columns, the empty and unavailable states of the media columns, the text
entry screens, back-out and fast input, odd window sizes, and the power confirmations (dry run, never confirmed).
Method as before: `--script` runs in a scratch data directory (`XMB_POWER_DRY_RUN=1`, `XMB_AUDIO_DRY_RUN=1`,
`XMB_STEAM_ARTWORK=0`, `XMB_STEAM_ACTIVITY=0`, `XMB_SETUP_SCREENS=1`), grabs read as contact sheets. A wrapper refused any
script containing Cross until a look-only run had printed the focused row ids. Windows: 1920x1080 (`full`), 800x450,
1280x720, 1280x1000, 2400x700. Grabs and fixtures are outside the repository.

## Fixed

| Defect | Evidence | Fix |
|---|---|---|
| Cross on Video > BD Data Utility or Video Editor & Uploader said "This feature is not available in the native recreation yet." although each row carries its own reason (no Blu-ray drive, no capture device) | grab after Cross on both rows | the Unavailable page uses the row's own reason for those two ids (`shell.cpp`), as it already did for Continue Watching and Save Data rows |
| Cross on Friends > Message Box > Create Message gave the same generic line (sweep 3 gave every other PSN and Friends row a reason) | grab | "Messages need PlayStation Network, which this application does not connect to." (`unavailableReason`) |
| Friends > Discord Connection: step 4 ("The friends list needs Discord to approve the relationships.read permission: request it on ...") was split at its colon into a label and a value, so the label was elided mid-word ("...relationships.read pe...") | grab at 1920 and 800x450 | an information-page line whose text before the colon is wider than the label column is drawn as a plain wrapped line (`dialogs.cpp`). Label/value rows with short labels (System Information, host reports) are unchanged |

None of these is in a pinned frame.

## Walked, nothing to change

- **Network**: Online Instruction Manuals, Remote Play, Internet Browser, Internet Search (Cross, message page, Circle back to
  the same row), Home Assistant and Weather unconfigured (setup row, information page, back out). Download Management was
  sweep 4's.
- **PSN**: Sign In, Trophy Collection, PlayStation Store: each says why, Circle returns.
- **Friends**: Add a Friend, Players Met, Chat Room, Start New Chat, Block List, Message Box (Received, Sent, Create Message),
  Discord Connection. The list does not wrap at either end.
- **Media columns, empty library** (scratch data directory): Video > Continue Watching shows the in-column "There are no content
  items." line; Photo > Photo Gallery shows "There are no images." and Circle returns; Music > Playlists > Create New Playlist
  opens the keyboard.
- **On-screen keyboard** (playlist name, user name): arrows wrap (checked down and left), the blank key right of Space takes focus,
  the 32-character limit holds when 52 characters are typed, Circle closes the keyboard first and the page second, Back on the
  user-name page creates nothing (profiles.json unchanged). Resized to 800x450, 2400x700 and 1280x720 while open: scaled
  contain-fit, nothing overlaps.
- **Power rows, dry run, never confirmed**: Restart System, Sleep, Switch to Desktop, Exit to Desktop each open the confirm box
  with No focused; Circle cancels and the cursor stays on the row. Turn Off System opens the black power menu; Circle goes one
  step back to the menu and a second Circle to the column. Cross was never pressed on a confirm box.
- **Fast input**: 60 random UP/DOWN/Cross/Circle taps 35 ms apart in the PSN column, 60 random arrow/Circle/Triangle taps across
  all columns, Cross/Circle mashing on the Message Box page, and 3 to 6 second held directions in Friends and across columns:
  no crash, no stuck page, the cursor ends on a valid row, every column end stops without wrapping.
- **Window sizes**: 800x450 and 2400x700 for the Friends column, information pages, message pages, the user-name page and the
  keyboard; the layout is the 16:9 design space contain-fitted (black side bars on the wide window), as in sweep 3.
- The labels of unfocused rows fade after a few seconds without input (the idle dim, sweep 4); a grab taken after a long pause
  shows icons without labels. That is not a stuck state: any key brings them back.

## Left alone, decisions for the owner

1. **Choose Media Folder / an empty library uses a native Windows folder dialog** (`QFileDialog::getExistingDirectory`, English
   title, `view.cpp`). It is opened by Video, Music and Photo Library when no folder is set and by Choose Media Folder. With a
   pad and no keyboard or mouse it cannot be driven, and it blocks the XMB until closed (a scripted run hangs on it). Fixing it
   needs an in-XMB folder browser: a design decision, not a small fix.
2. The setup pages (Set Up Weather, Set Up Home Assistant, Discord Connection) show a generic folder icon in the page header
   while the row has its own icon. Page header icons come from the page icon source; changing it is wider than this sweep.
3. The Turn Off System flow takes two Circle presses to leave (confirm, then the black power menu). It looks intentional, but
   there is no console frame of that sequence here to compare.

## Not reviewed

Tailscale, Wake on LAN, Phone Remote, Wi-Fi Networks, Sign In to Discord / Refresh Discord Friends, the Spotify and Jellyfin
rows, the DLNA scan (all act on the system or network and were not pressed); a confirmed Sleep, Restart or Turn Off; the
keyboard's caps page and symbol palette beyond opening it (the keys are traced, not re-checked); a real 4K window and any window
smaller than 800x450 (the script refuses smaller sizes).

## Gate

Full gate on the final build (MSVC /W4, 0 warnings): 26 of 27 states pass; the one failure is `xmb-volume-hud`, whose log
assertion needs "muted 0" and this PC's playback is muted (the same environment failure as sweep 4; this sweep touched no
audio setting). No reference was saved or changed; the reference folder is the junction to the real one.
