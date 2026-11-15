# OSK paste: the Windows clipboard into the on-screen keyboard

Date: 2026-11-14. Scope: `src/osk_paste.{h,cpp}` (pure sanitising and limits), `View::oskPaste` and the keyboard and pad
hooks in `src/view.cpp`, test-only script tokens in `src/main.cpp`, `tests/osk_paste_tests.cpp`.

Why: the long values this app asks for (Home Assistant long-lived tokens, URLs, API keys, folder paths, client ids,
Jellyfin and Wi-Fi credentials) are painful to type with a pad. The HTPC is driven by a physical keyboard, Moonlight or
a remote desktop, and the clipboard that matters is the HTPC's own.

## What it does

| Input | Effect |
|---|---|
| Ctrl+V, Shift+Insert (physical keyboard, Moonlight, remote desktop) | Pastes the clipboard text at the caret. |
| Pad: Right from Enter (bottom row) to the Paste key, then A / ✕ | Same paste. Left, Right, Up or Down leave the key again. |
| Any other Ctrl chord | Ignored (it used to insert nothing anyway; now it cannot insert a control character). |

Works in every field the OSK serves: System Name, user and playlist names, the Jellyfin sign-in page, Real-Debrid key,
Home Assistant and DLNA fields, setup paths, and the in-game "Enter Text" row (ZF2; that field's 256-unit limit
applies and the confirmed text is delivered as before).

The OSK has no text selection, so there is nothing to replace: text goes in at the caret. The field's own limit
(`osk_.maxLength`: 32 names, 256 user names and in-game text, 2,048 server addresses, 4,096 passwords and keys) applies
to the whole field, and a paste that does not fit is cut at the limit on a code point boundary and says so.

## Cleaning rules (`xmb::oskpaste::prepare`)

1. CR, LF, U+2028, U+2029 at either end are trimmed. Spaces and tabs at the ends are kept, because a value may start
   with a space; only the line breaks a copy usually drags along are removed.
2. Inside the text each line break (CRLF counts once) becomes one space in a single-line field, which is every field in
   this build (`Request::multiline` keeps them as LF for a future multiline field). A tab becomes a space. Other control
   characters (below U+0020, U+007F..U+009F) are dropped.
3. Clipboard text over 65,536 UTF-16 units is refused before anything is scanned: "Clipboard text is too large to
   paste". The biggest field is 4,096 units, so a larger clipboard is a file or a log, and a refusal beats a stall.
4. The result is cut to the room left, never splitting a surrogate pair.

Choice made: an inner line break becomes a space, not nothing. A wrapped token copied from a narrow terminal will gain a
space where it wrapped; the owner removes it with Backspace. The alternative (strip) silently glues two words or two
path parts together, which is worse for URLs and paths with spaces and no easier to spot.

## Messages

Shown in the keyboard's footer strip for 2.5 s (the strip is empty in the captured keyboard; nothing is drawn there unless a
paste just happened, so the default frame is unchanged):

| Case | Text |
|---|---|
| Pasted | `Pasted N characters` (code points; `1 character`) |
| Cut at the limit | `Pasted N characters (field limit reached)` |
| Clipboard text empty or only line breaks | `Clipboard is empty` |
| Clipboard holds an image or files, no text | `Clipboard has no text` |
| Field already full | `The field is full` |
| Over 65,536 units | `Clipboard text is too large to paste` |

Pad discoverability (decision). The firmware keyboard already draws a Paste key (`KEYTOP_PASTE`, x 1322 y 678) next to the
Copy key; the capture shows it dimmed because the console's clipboard was empty (OSK-FIRMWARE 4.4). It stays exactly as
captured until a paste finds text on the clipboard, then it is drawn enabled; that keeps every pinned frame identical at
the default state (the build never polls the clipboard to decide how to draw it). The Paste key is a focus stop in the
bottom row (it sits level with Enter, so Right from Enter reaches it), drawn with the firmware's own SBTN_FOCUS ring and a
brighter face. I did not map a pad button: the badges on the keys already bind L1/R1 (caret), L2/R2 (accent, shift),
Square, Triangle, Select (Input Mode) and Start, so a "spare" button would contradict a visible badge; reaching the
visible Paste key is the firmware's own design. The Copy key stays inert (no selection); it logs `osk: copy is not wired`.

## Privacy and logging

- The clipboard is read in `View::oskPaste` only, on an explicit paste action. There is no polling, no clipboard-changed
  handler and no read at startup or when the keyboard opens.
- The pasted text is not logged anywhere. The paste log line is `osk: paste verdict=<n> characters=<n> removed=<n>
  masked=<0|1>`; the verdict is the `Verdict` enum, `no-text` for a clipboard without text. Masked (password) fields
  accept paste and draw bullets as always; the UI signature carries the bullet count, and the footer message carries a
  count only. The existing logging scrub keeps working because nothing new carries text. Verified below.
- Text-entry delivery to a running title (`text_entry.h`) was already count-only.

## Verification (2026-11-14, build of this branch, scratch data dir, fixtures only)

Pure logic: `xmb-osk-paste-tests` (`-DXMB_OSK_PASTE_BUILD_TESTS=ON`, also in `tests.yml`): 27 checks, all pass. They cover
end trimming, inner breaks and tabs, control characters, multiline, empty and break-only input, the limit with an
existing suffix, surrogate pairs at the cut, the full field, the size cap (accepted at 65,536, refused above, a 5 MB
clipboard refused fast) and the message strings.

Scripted runs (`XMB_TEST_CLIPBOARD=1`; test-only tokens `clip=<percent-encoded text>`, `clipfill=<n>`, `clipimage`,
`PASTE`, `SHIFTINSERT`; they do nothing without the variable and they never read the real clipboard):

| Frame | Case |
|---|---|
| a0 / a1 | System Name, before and after Ctrl+V of a fixture token (cut at 32, "field limit reached") |
| a2 | Shift+Insert into a full field ("The field is full") |
| a3 | `clip=` empty clipboard ("Clipboard is empty") |
| a4 | image on the clipboard ("Clipboard has no text") |
| a5 | footer back to empty after 2.5 s |
| b0 / b1 | Real-Debrid API Key (masked), before and after a fixture with CRLF at the ends, two leading spaces, an inner CRLF and a tab: 32 bullets, no text |
| b2 / b3 | pad path: focus on the Paste key, then Enter presses it: second fixture pasted |
| b4 | 6,000 characters into the 4,096 field: cut, no stall |
| b5 | 70,000 characters: refused with the message |
| b6 | Escape cancelled (nothing saved) |

The log of each run was searched case-sensitively for every fixture string: no match. Only the verdict lines appear.

## Not verified

- A real physical Ctrl+V keystroke and a real pad A press (scripted runs send the same key events, and pad Enter through
  `keyPressEvent`; the XInput edge path calls the same `oskPress`).
- Moonlight or remote desktop clipboard sync itself (whatever puts text on the HTPC's clipboard is outside this app).
- A multiline field: none exists; the `multiline` mode is unit-tested only.
