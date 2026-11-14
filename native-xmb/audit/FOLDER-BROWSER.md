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

## File mode: Choose Wallpaper and Choose Theme (2026-11-14)

Choose Wallpaper and Choose Theme called `QFileDialog::getOpenFileName` from `view.cpp`; a pad cannot drive it and it blocks
the XMB. They now open the same browser page in file mode (`Shell::openFilePicker("wallpaper" | "theme")`; the
`requestWallpaper` / `requestTheme` signals and their `view.cpp` connections are gone).

| What | Wallpaper | Theme |
|---|---|---|
| Opened from | Settings > Theme Settings > Background > Select Wallpaper; the Background row when it needs a file; `Action::Wallpaper` | Settings > Theme Settings > Theme > Install; `Action::Theme` |
| Page title | Choose Wallpaper | Choose Theme |
| Filter (the dialog's own) | `*.png *.jpg *.jpeg *.bmp *.webp` | `*.p3t` |
| Cross on a file | page closes, `Shell::chooseWallpaper(file)` | page closes, `Shell::chooseTheme(file)` |
| Start folder | folder of the current wallpaper, else Pictures | folder of the current theme, else Downloads |

Same behaviour as the folder mode otherwise: drive list at the top (Circle from a drive root goes there, from there closes
the page), Cross on a folder goes in, Circle goes up with the folder just left focused. Differences:

- No Use This Folder row. Rows are the folders first, then the files that match the filter; each group sorts naturally
  without case. Hidden and system files are left out like folders; a file row shows its size on the second line ("2 KB").
- Image files draw a picture glyph and theme files a page glyph (`glyphs::picture()`, `glyphs::document()`), so a file is
  not mistaken for a folder. Thumbnails are not decoded: a folder can hold thousands of images.
- The row cap (2000) counts folders and files together; folders come first, so a cut drops files and the last row says
  "More Entries Than Can Be Listed".
- A folder with neither folders nor matching files shows "No Matching Files" (Information row; Circle goes up). A folder with
  only sub-folders just lists them. An unreadable folder shows "This Folder Cannot Be Read" as before.
- From a Theme or Background choice band the band page is replaced by the picker, so the band is not left drawn beside it;
  Circle from the drive list returns to the Theme Settings row. (The dialog left the band open.) The choice and its
  validation are unchanged: `chooseWallpaper` rejects an unreadable image and `chooseTheme` an unloadable `.p3t` with the
  same message as before.
- `XMB_FOLDER_START` overrides the start folder in file mode too (test-only).

`folderbrowser::list()` takes the filter (`list(path,limit,filePatterns)`; empty = folder mode, unchanged), plus
`matchesFilter()` and `fileSizeText()`. `Item::isFile` tells the rows apart. The runner is the same
`xmb-folder-browser-tests` (target and CI name unchanged); it now has 53 checks (the 37 folder-mode checks untouched, 16
for file mode: filter case and whole-name match, folders-then-files natural order, hidden and non-matching files left out,
size line, theme filter, folder mode unchanged, combined cap and its boundary, empty and missing folders, drive list,
start folder of a chosen file, size text).

Evidence: scripted look-only runs at 1920x1080 and `size=800x450` on a scratch data directory and a scratch fixture folder
(PNG, JPG, BMP, a text file, a `.p3t` stub, a 130-character folder name, an empty folder): the Install and Select Wallpaper
pickers opening over the choice band, folders then files in natural order ("Green 2" before "Green 10"), long folder and file
names elided with "...", the size line, Cross into an empty folder giving "No Matching Files", Circle back with focus
restored, and Cross on a file writing `appearance/wallpaper` (and `appearance/background` 2) only to the scratch
`preferences.json`; a Cross on a stub `.p3t` reached `chooseTheme` (the log line `file picker: chose "<path>" for theme`)
and wrote nothing because the stub is not a theme. No gate state was added: a state would need a fixed fixture folder whose
path is printed, and a new saved reference; no saved reference was touched.

## Where it lives

| File | What |
|---|---|
| `src/folder_browser.*` | drive list, listing (folder mode and file mode), parent, elision, start folder; Qt Core only, no Shell |
| `src/folder_browse_shell.cpp` | the page (`Page::browse`, `Page::browseFiles` for file mode), rows, `Action::BrowseOpen` / `BrowseUse` / `BrowseFile`, `Shell::openFilePicker()`, `Shell::browseUp()` |
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

- Other `QFileDialog` users, all desktop windows meant for keyboard and mouse and not converted: `src/app_editor.cpp` (lines
  184, 190, 195, 326, 381: game executable, working directory, artwork, import registry) and `manager/manager_ui.cpp` (the
  Manager's own folder, file, ROM and save pickers). `src/view.cpp` has none left.
- There is no type-to-jump; the list's own paging is all there is.
