# The firmware's predictive-text dictionaries (Zi8 `.z8d`): format, walk and ranking (2026-11-23)

The on-screen keyboard's Prediction column lists candidates from `dev_flash/data/dic/Zi8Dat<LANG>.z8d`. This file
documents the format and the lookup as reversed from `vsh/module/ime_zi.sprx` (the decrypted `ime_zi.prx` of 4.93,
repo-external), and the evidence that the native reader reproduces the console's lists. Native code:
`src/zi8_dictionary.h/.cpp` (reader), `src/view.cpp` (`View::oskDictionary`, `View::oskPredictions`, the column draw),
tests `tests/zi8_dictionary_tests.cpp`.

Evidence classes: **M** traced in the module (function address given), **D** observed in the data files, **R** observed in
the real-console recording (`C:\dev\out\gapvid\m2-osk\frame-*.jpg`, see `OSK-RECORDED-SPEC-2026-11-22.md`), **FIT** chosen to
match the recording where the module was not traced that far.

The dictionaries are the owner's firmware. They are read at runtime from the configured dev_flash, like every other
firmware asset, and are never copied, bundled, converted or committed. The tests use synthetic dictionaries built by the
test itself.

## 1. Tools and method

* `ime_zi.prx` imported into a scratch Ghidra project (PowerPC:BE:64:A2ALT-32addr, `Ps3Exports` / `Ps3ResolveImports` from
  `ghidra_scripts/`, functions split at every `stdu r1,-n(r1)` prologue, whole module decompiled). The module imports only one
  libc function: the dictionary is handed to it in memory by `ime.sprx`, so the format had to be read from the code that walks
  the buffer, not from file I/O. (REA's ELF loader does not accept the PPC64 PRX.)
* A throw-away Bun decoder (scratchpad only, not in the repo) implemented the traced rules and dumped the tries to check them;
  the C++ reader is the same rules.

## 2. Header (M: block accessor `0x1e9e4`, its stubs `0x1eb8c` pointer, `0x1eb98` size, `0x1eba4` size & 0xffff, `0x1ebb0` byte 1, `0x1ebe0` byte 3; dictionary table lookup `0x1e918`)

| Offset | Size | Meaning |
|---|---|---|
| 0 | 1 | language id; also the key `0x1e918` looks the loaded dictionary up by. ENUK 0x3a, ENAM 0x3b, DE 0x05, SV 0x09, FI 0x0a, NO 0x0b, DA 0x0c, NL 0x0d, PL 0x16, RU 0x27, IT 0x2f, FREU 0x3f, FRCA 0x40, ESSA 0x41, ESEU 0x42, PTEU 0x43, PTBZ 0x44, ZH* 0x01 (D) |
| 1 | 1 | 0x80 in every alphabetic file (read by `0x1ebb0`; meaning not traced) |
| 3 | 1 | read by `0x1ebe0`; for id 1 (Chinese) a value > 3 switches to an alternate slot table at 0x8b. 0 in the alphabetic files |
| 4 + 6k | 3 + 3 | slot k (k = 0..31): 24-bit big-endian size, then 24-bit big-endian absolute offset |
| 0xc8 | | first block (slot 0) |

Slots seen in the English files (D; the reader uses only 0 and 1):

| Slot | ENAM size @ offset | Content |
|---|---|---|
| 0 | 0x14278 @ 0xc8 | the main word trie (section 3) |
| 1 | 0x3c8 @ 0x14340 | the two-key index of slot 0 (section 4); `0x89c0` always pairs slot n with slot n+1 |
| 4 | "size" 0x1e @ 0x14fd8 | 30 length-prefixed lower-case words, all offensive: a block list (the size field holds the count). Its use was not traced |
| 6, 7, 8 | 0x208, 0x190, 0x1f0 | character conversion and character-to-key range tables (read by `0x1bc`, `0x2ec`, `0x4ac`; not traced further) |
| 11, 12 | 8, 0x60 | slot 12 is a second trie in the same format holding the suffixes `'s 'll 've 'd 're 'm .us .ca .com .net .org` |
| 29, 30 | 0x170 each | multi-tap key cycles for the phone keypad (` 0.,?!@'"-()/:_;+&%*=<>€£$¥¤[]{}\~^¿¡§#1abcàâæç2def...`) |
| 31 | "size" 2 | a flags word read by `0x1eba4(id,0x1f)` (`& 4`, `& 0x40` tests in the engine) |

## 3. The word trie (slot 0)

Block layout (M: `0x89c0` sets up its state from it; `0x15650` returns the root; `0x155cc` reads a node's character):

| Offset in block | Size | Meaning |
|---|---|---|
| 0 | 2 | 0x0001 in the English files (not read by the walk) |
| 2 | 2 | T = byte length of the two character tables = 3 x n |
| 4 | 2n | the n characters, UTF-16BE, in index order |
| 4 + 2n | n | the same characters, 8-bit |
| 4 + T | | the first root node |

ENAM's 40 characters: `e a i r n t s o l c d u p m b h g f k v w y z j q x . - ' é 2 0 8 1 4 / (space) 3 ç 5`; ENUK's:
`e i a r n t o s l c d u p m b h g f k v w y - j q z x ' . 8 / 0 é 2 1 4 (space) 7 3 è 5` (D: letter-frequency order, so the
common letters fit in one nibble).

### 3.1 Node (M: `0x155cc` character, `0x152e8` first child, `0x153cc` next sibling, `0x155b0` word end)

The first byte's low nibble is the character index; 15 means "15 + the next byte". The high nibble selects one of 16 flag
words from the module's table at 0x2fa60 (reached through TOC-0x7f54):

| Nibble | Flags | Word end | Child | Sibling |
|---|---|---|---|---|
| 0 | 0x10 | yes | - | - |
| 1 | 0x01 | | inline | - |
| 2 | 0x03 | | pointer | - |
| 3 | 0x14 | yes | - | inline |
| 4 | 0x05 | | inline | inline |
| 5 | 0x07 | | pointer | inline |
| 6 | 0x1c | yes | - | pointer |
| 7 | 0x0d | | inline | pointer |
| 8 | 0x0f | | pointer | pointer |
| 9 | 0x11 | yes | inline | - |
| 10 | 0x13 | yes | pointer | - |
| 11 | 0x15 | yes | inline | inline |
| 12 | 0x17 | yes | pointer | inline |
| 13 | 0x1d | yes | inline | pointer |
| 14 | 0x1f | yes | pointer | pointer |
| 15 | 0x35 | yes | inline | inline (plus 0x20, which no node of the English files uses) |

Bytes of a node, in order: the character byte(s); the child pointer if the child is a pointer; the sibling pointer if the
sibling is a pointer. An inline child starts right after those bytes. A pointer is a forward offset from the node's first
byte: two bytes big-endian when the top bit is clear (0..0x7fff), else three bytes whose low 23 bits plus 0x8000 are the
offset.

An inline sibling is the node after the current node's whole subtree. `0x153cc` finds it by walking the bytes with a level
count: starting at 0, a node with an inline child adds one, a node with no sibling or a pointer sibling (the end of a
contiguous run) subtracts one, and the first position where the count is below one is the sibling.

A word is the characters on the path from a root node to a node whose flags have 0x10.

### 3.2 Shape of the English tries (D)

The trie is built for the phone keypad (T9): the root level is not one node per letter but a long run of groups (`. (space)
' . a b a b a ...` in ENAM), each a small trie, and the same letter repeats across groups. Depth-first order across the
groups is the frequency order the IME presents. Decoded totals: ENAM 31,105 words, ENUK 30,991 (all lower case; no word
occurs twice).

## 4. The two-key index (slot 1) (M: `0x15674`)

| Offset | Size | Meaning |
|---|---|---|
| 0 | 2 | k = number of key codes (equal to the character count) |
| 2 | 2k | the phone key of each character: 0xeff1..0xeff9 = keys 1..9, 0xeffa = key 0 (`a b c` -> 0xeff2, space -> 0xeffa, `' - .` -> 0xeff1) |
| base = 2 + 2k | 2 + 2 | must both be 0, or `0x15674` returns 0 and the walk lists nothing |
| base + 4 | 2 | number of entries (all levels) |
| base + 10 | 10 each | entries: {u16 child entry count, u16 key, u24 start, u24 stop}; start and stop are offsets from the root node; stop 0 means none |

The top level is one entry per first key, each followed by its child entries (one per second key). For the typed word's
first one or two characters `0x15674` maps each to its key, finds the entry (skipping a non-matching entry's children), and
returns its start node; the walk ends when it reaches the stop node. ENAM has 87 entries, ENUK 89.

## 5. The walk (M: `0x89c0`, callers `0x92d4`, `0x9594`, `0x9c8c`, `0x9dcc`)

State: one 16-byte frame per depth (node, character info, matched input). From the start node:

* At a depth inside the typed word, a node matches when its character's key equals the input or its character equals the
  input (`uVar13 == input || input == char`). The full keyboard sends characters, so only same-letter nodes match (R: after "h"
  every candidate starts with h, none with g or i).
* A match below the word's length descends to the first child; a mismatch moves to the next sibling, popping frames when a
  level has none.
* Exact pass (6th argument 1): the maximum depth is the word's length; a word end there is reported and the walk does not go
  deeper.
* Completion pass (6th argument 0): the maximum depth starts at length + 1; every word end below the typed word is reported in
  depth-first order and the walk continues into its children.
* The walk stops at the index entry's stop node or when the root level runs out. Each call returns one word; the callers resume
  it to collect more.

So the console's candidate order is: the exact-length words, then the completions in the trie's own depth-first order. There
is no separate frequency field and no re-sorting.

## 6. The candidate list the keyboard shows (R, with the module order of section 5)

1. the typed composition word itself (R: "h" is listed first although no word "h" exists in either file; not traced in
   `ime.sprx`/the OSK plugin, which add it);
2. the exact-length dictionary words (R: "hi" heads the "hi" list although the completion order has it fourth);
3. the completions, without repeating a word already listed.

Native: `zi8Candidates` does exactly that, matching case-insensitively (the files are lower case) and keeping the typed word as
typed; at most `kOskMaxCandidates` = 100 lines (FIT: see 7).

### Validation against the recording (R)

| Typed | Recording (eight visible lines) | Native, ENUK | ENAM would give |
|---|---|---|---|
| h | h, here, had, have, has, ha, hack, habit | h, here, had, have, has, ha, hack, habit | h, him, had, have, has, having, half, ha |
| hi | hi, his, him, himself, hi-fi, hibernate, hibernates, hibernation | hi, his, him, himself, hi-fi, hibernate, hibernates, hibernation | hi, him, his, himself, high, hi-fi, hibernate, hibernates |

Both recorded lists are reproduced exactly, in order, by the English (UK) file; the American file does not reproduce either.
So the recorded console was set to English (United Kingdom). The check runs in `xmb-zi8-tests` when `XMB_DEV_FLASH` points at a
dev_flash (it is skipped in CI).

## 7. The column (R: `OSK-RECORDED-SPEC-2026-11-22.md` 4)

* Eight lines visible, 18 px pitch, text at x 821 at 720p (1231.5 / 27 px at 1080p), white 235. Native glyph size 18 px at
  1080p: the recorded line inks (x 822..887 for "hibernation") are matched to +-1 px.
* No line is highlighted in any frame, and no frame shows a candidate being chosen: the native keyboard draws the list and
  binds nothing to it (no pad or key selects a candidate). The `a 0` cells stay as drawn (the recording reads 0 with eight
  candidates showing, so the counter is not the candidate count; its meaning is UNKNOWN).
* Scroll knob: x 939..949, y 270..282 at 720p, the same in the "h" and "hi" frames, drawn only while there are candidates. FIT:
  its height is the visible share of the list (8 of at most 100 lines), which gives the recorded 13 of 154 px for both words
  (each has more than 100 completions). Shorter lists draw a longer knob under the same rule (not seen in the recording). The
  list is never scrolled.
* Masked (password) fields never query the dictionary or show candidates.

## 8. Dictionary choice

`View::oskDictionary` reads `data/dic/Zi8DatENUK.z8d` (or `Zi8DatENAM.z8d` when the Windows locale's territory is the United
States), lazily on the first composition word, from the configured `--dev-flash`. `XMB_OSK_DICTIONARY=<code>` (e.g. ENUK, ENAM)
picks one by name. A missing or unreadable file leaves the column empty, as before.

## 9. Not verified / open

* Other languages: the reader is format-generic for the alphabetic `Zi8Dat*.z8d` files (same header and node scheme, D), but no
  non-English list was compared with a console. The Chinese files (id 1) use a different block 0 (12-byte records) and are
  refused by the reader.
* Learned words: the module keeps a 100-entry recent-word ring (`0xbed8`) and the engine has user-dictionary paths; whether the
  console promotes typed or chosen words is not reproduced (the native list is the static dictionary only).
* Capitalisation: candidates are drawn as stored (lower case); what the console shows after typing a capital is UNKNOWN.
* The block-word list (slot 4), the suffix trie (slot 12) and the phone-keypad (T9) mode are decoded or identified but unused.
* The 100-line cap and the knob rule are FIT (section 7); paging or scrolling the list and choosing a candidate are not in the
  recording.
