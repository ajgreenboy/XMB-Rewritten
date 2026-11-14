# In-XMB folder browser (2026-11-14)

Parity sweep 5 left one item as a design decision: Choose Media Folder, and the Video, Music and Photo Library rows while
no folder is set, opened the native Windows folder dialog (`QFileDialog::getExistingDirectory`, English title). A pad cannot
drive it, and it blocks the XMB until closed (a scripted run hung on it). It is now an ordinary XMB list page.

## Behaviour

| Input | Result |
|---|---|
| Cross on Choose Media Folder, or on an empty Video / Music / Photo Library row | the browser page opens on the start folder (below) |
| Up / Down | move through the rows |
| Cross on a drive or folder row | go into it (the page stays, its rows change) |
| Cross on **Use This Folder** (the first row) | the page closes and the folder goes to `Shell::chooseLibrary()`, exactly where the dialog's answer went: stored as `library/<column>` and opened |
| Circle | up one folder; from a drive root to the drive list ("This PC"); from the drive list the page closes. The folder just left is focused again |

- Top level: the drives of this PC, each "label (X:)" with its free space. No Use row there (a drive can be chosen from
  inside it).
- Folders only; hidden and system folders are left out; names sort naturally and without case ("Disc 2" before "Disc 10").
- Long names are elided by the row renderer (one line, no overlap); the Use row's second line shows the full path shortened
  by `folderbrowser::elidePath()` to 56 characters.
- A folder Windows will not open shows one row, "This Folder Cannot Be Read" (Cross says why), and no Use row; Circle goes up.
  An empty folder shows "No Folders Inside" and keeps Use This Folder. More than 2000 folders are cut and say so.
- Start folder: the folder already chosen for the column if it still exists, otherwise the Videos, Music or Pictures folder,
  otherwise the nearest existing parent, otherwise the drive list.
- Nothing is created, changed or deleted by browsing; only the final Use This Folder writes the existing `library/<column>`
  preference.

## Where it lives

| File | What |
|---|---|
| `src/folder_browser.*` | drive list, listing, parent, elision, start folder; Qt Core only, no Shell |
| `src/folder_browse_shell.cpp` | the page (`Page::browse`), rows, `Action::BrowseOpen` / `BrowseUse`, `Shell::browseUp()` |
| `src/shell.cpp` | `back()` calls `browseUp()` before it pops a page; `requestLibrary()` is gone (Shell opens the page itself) |
| `tests/folder_browser_tests.cpp` | `-DXMB_FOLDER_BROWSER_BUILD_TESTS=ON`, target `xmb-folder-browser-tests`; run in `tests.yml` as `folder-browser` |

`XMB_FOLDER_START` is a test-only start folder for scripted runs (`roots` or `this-pc` starts at the drive list). It is read
only when the browser opens and overrides the start rule above.

## Evidence

- Unit runner: 37 checks, all pass (drives, folder-only natural order, hidden folder left out, row cap, missing folder, a
  file path, an access-denied folder made with a scratch `icacls /deny`, parents, `clean`, elision at 8 to 56 characters,
  start-folder rules).
- Scripted look-only runs, `--script` on a scratch data directory, 1920x1080 and `size=800x450`: drive list at top level
  (six drives with labels and free space), a scratch tree with a 120-character folder name (elided with "..."), natural order,
  Cross into a folder, Circle back with focus restored, two more Circles up and a Circle from a drive root to the drive list,
  and the access-denied folder page. Cross on Use This Folder wrote `library/video` in the scratch `preferences.json` and
  opened the folder. The log line `folder browser: showing "<path>" rows=N` and `folder browser: chose "<path>"` record each
  step. No Cross was pressed on anything that acts on the system or network.
- No gate state was added: a state would need a fixed folder, and the Use row prints that folder's path (the worktree path
  differs between checkouts), and it would need a new saved reference. No existing frame changed: the browser opens only from
  rows that previously opened the dialog.

## Left alone

- Choose Wallpaper and Choose Theme (`view.cpp`) still use `QFileDialog::getOpenFileName`: they pick files, and the task was
  folders. The same page could list files with a filter; that is a follow-up.
- `app_editor.cpp` and the manager (`manager/manager_ui.cpp`) are separate desktop windows meant for keyboard and mouse; their
  dialogs stay.
- There is no type-to-jump; the list's own paging is all there is.
