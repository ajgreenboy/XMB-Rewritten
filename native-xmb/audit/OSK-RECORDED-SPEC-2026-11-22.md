# The on-screen keyboard as recorded: measured spec (2026-11-22)

Source: the dense frame set `C:\dev\out\gapvid\m2-osk\frame-00035500.jpg ... frame-00041500.jpg` (66 frames, 100 ms
step, 35.5-41.5 s of the older real-console recording, 1280x720 JPEG; the text-entry dialog "Comment" opens the
keyboard at about 36.4 s and Circle closes it at about 39.8 s). Every value below was measured on those frames with
pixel scans (`edges`, `ink`, `series` scripts under `C:\dev\out\osk-1to1\tmp\`) unless it is marked FIT (fitted to the
100 ms sampling) or UNKNOWN (not visible in the frames). The recording is 720p; 1080p design-space values are the 720p
value x 1.5, which is the unit the native code works in. JPEG blur is +-1 px and +-3 levels.

This pass is the "make it 1:1 with the recording" pass. It supersedes `OSK-FIRMWARE.md` where the two disagree
(listed in section 10); everything else in that document (layout recovered from the module, behaviour, the 1080p RPCS3
captures) stands.

## 1. Window

| Element | 720p | 1080p (x1.5) | Notes |
|---|---|---|---|
| Window | x 329..951, y 193..527 (622 x 334) | x 493..1427, y 290..790 | same rect as OSK-FIRMWARE 4.1 scaled by 2/3, centred on the screen centre (640,360) |
| Drop shadow | right and bottom only: x 951..954 reads 128 -> 102 -> 61 -> 40, bottom edge 527 -> 77 -> 34 | ~6 px | nothing darkens the left or top edge |
| Plate between the bands | (148,148,148) strip y 263..270 | (157,154,157) | the colour the key seams show |
| Footer band | y 489..527 | y 733..790 | dark, (69,69,67) with a light line at its bottom (104,100,99) |

## 2. Title bar and entry row

| Element | Value |
|---|---|
| Title bar | y 194..224, flat (149,149,147) with a darker sheen line at its bottom edge (y 223-225: 145 -> 116 -> 68) |
| Pen icon | solid white pen tilted to the right with a dark hole at the lower end, x 336..349, y 198..215; the firmware image `osk_plugin.rco / osk_icon` has exactly this shape |
| Title text | white (250,250,250), starts x 354, vertical centre y 207, "Comment" is 71 px wide (23 px font at 1080p), soft drop shadow 1 px right / 1.5 px down |
| Dialog text behind | the calling dialog's own title ("Comment", centred at x 640, y 210) shows through the title bar at about 12% when the window is at rest, and at full strength during the open and close fades |
| Entry row | y 225..262, vertical gradient (86,84,85) at the top -> (70,70,70) at 60% -> (44,44,44) at the bottom |
| Text | white, left edge x 341 (the caret shaft's x), "hi" at 22 px |
| I-beam caret | bright green (130,249,12), shaft 1-2 px at x 341, 22 px tall (y 231..252), 4 px wide serifs top and bottom (33 / 7 / 2 px at 1080p; OSK-FIRMWARE 4.5 had 27) |
| Cursor marker | hollow grey (190) triangle pointing left, apex x 344, base x 351, y 236..246 (8 x 11 px, 1 px stroke); present whether or not the I-beam is lit |
| Blink | period ~936 ms, lit ~470 ms (OSK-FIRMWARE 13.1); the 100 ms frames agree (the caret is lit in some frames and dark in others) but cannot refine it |
| Composition underline | measured on the 100 ms frames: a solid grey (145,145,145) bar 2 px thick at y 251..252 (720p; 3 px at field top + 39 in 1080p), spanning the typed word's advance ("h": x 341..353, "hi": x 341..355), below the baseline of the text. It appears in the first frame after "h" is typed (37.3 s) and stays through "hi" (to 39.4 s); the frame after Space is typed (39.5 s) has no underline and the caret has moved right of the space. So it marks the run of word characters ending at the caret (the composition word) and ends at a space. UNKNOWN: whether digits and punctuation compose, and what Enter/Left/Right do to it. Native: `osk_composition.h` (`oskCompositionRange`: letters, digits, and an apostrophe or hyphen between two of them, ending at the caret), drawn for every non-masked composition word whether or not candidates exist |

## 3. Key grid

Ten columns, pitch 48 px (72 at 1080p), face 44 px wide (66), seam 4 px; the faces' left edges at y=282 are
331, 379, 427, 475, 523, 571, 619, 667, 715, 763. Rows are 36 px apart (54): the four character rows start at y 271,
307, 343, 379; the function rows follow at 36 px pitch (row 4 caption centre y 436, row 5 caption centre y 472).
Faces are rounded (about 3 px at 720p), a vertical gradient with a bright band at 40% and a dark lower lip. Exact face
heights of the two function rows are +-2 px (the seam is blurred in the JPEG).

| Role | Rest colours (top / 40% / 75% / bottom) | Caption ink |
|---|---|---|
| Letters and digits | (164,168,164) / (196,195,196) / (190,188,190) / (152,150,152) | (40,42,41) |
| Olive (`*%#`, blank, a/a-grave, a/A) | (152,157,117) / (175,183,144) / (179,183,156) / (122,125,129) | (62,64,50) |
| Blue (Backspace, Space, blank) | (121,152,160) / (145,177,182) / (152,174,179) / (129,129,131) | (6,48,84) |
| Arrows | (116,150,156) / (140,169,177) / (140,161,167) / (110,107,109) | (0,42,78) |
| Mint (grid key, blank, Input Mode, Enter) | (109,151,132) / (137,175,158) / (148,175,164) / (110,107,109) | grid squares (2,46,45), captions (14,48,46) |

(The rest colours are the OSK-FIRMWARE 15 values; this pass re-sampled four of them in the recording and they agree to
+-6.)

Captions:

* Character keys: right-of-centre (the glyph centre is 53.5 px right of the cell's left edge at 1080p), 24 px.
* Function-row captions are right-aligned inside the key with a 6-8 px margin at 720p (9-10 px at 1080p): Backspace
  ink x 545..608 in a key 523..615, Space 668..703 in 619..711, Input Mode 639..706 in 619..711, Enter 768..800 in
  715..807, a/a-grave 441..464 in 427..470, a/A 489..513 in 475..518. 18 px font at 1080p ("Backspace" is 63 px wide).
* The symbol key reads `*%#` in the recording (OSK-FIRMWARE had `@#:` from the module).
* Arrow keys: solid triangles, no glyph font: up/down 12 x 7 px, left/right 9 x 12 px, centred about 9-10 px right of
  the key centre and 5 px below it; centres (359,472) up, (407,472) down, (456,471) left, (506,471) right.
* Palette key (row 5, col 4): a grid of 3 columns x 3 rows of flat cells in (2,46,45), 19 x 14 px overall, centred on
  the face. (The 1080p capture's 4x3 is not what the recording shows.)
* Badges (L2, R2, L1, R1, square, triangle, SELECT, START): dark (36,37,31) pills with light text, the L2 pill is 17 x
  10 px at x 429..445, y 414..423, straddling the top edge of its key, left of the key by 2 px.

Focused key (the highlight is a white fill on the face plus a soft white halo about 3 px beyond it; the firmware ring
`SBTN_FOCUS` / `LBTN_FOCUS` is that halo):

* Letter key fill, down the clear column: top (200,203,202), dips to (190,190,190) at 20%, rises to (236,238,237) at 60%,
  (232,234,233) at 80%, (214,216,215) at the bottom.
* Blue key (Space) fill: (150,190,200) top, (122,177,198) at 20%, (140,205,228) at 50%, (180,222,236) at 75%, (194,208,212)
  bottom.
* Mint key (Enter) fill: (150,190,180) top, (138,210,190) 10%, (148,221,199) 30%, (176,226,214) 60%, (186,216,209) 85%,
  (195,199,198) bottom.
* Olive, arrow and grid keys were never focused in the frames: UNKNOWN.
* Default focus after opening is `g`.

## 4. Prediction column

| Element | 720p | Value |
|---|---|---|
| Candidate list | x 812..940, y 271..424 | flat (114,112,113); candidate lines white (235), 13 px font, 18 px line pitch, text at x 821, first line centre y 285 |
| Scroll strip | x 940..949 | (121,115,115) -> (141,137,138) left to right; a lighter 2-line knob at the top of the strip when candidates exist |
| Header | y 426..446 | (89,89,87) top -> (82,80,81) bottom, "Prediction" light (235) at x 821, 16 px |
| Mode and count wells | "a" x 908..926, "0" x 928..945 | lighter wells (130,130,128) with white (234) text; the mode is `a` (lower) |
| Paste | one wide key x 812..948, y 447..482 | blue face (116,124,127) edge -> (135,165,176) middle -> (124,144,151); "Paste" right-aligned at the bottom, drawn faded (115,118,125) because nothing is on the clipboard |
| Copy | not drawn | there is no copy key in any frame; UNKNOWN whether it appears with a selection |

Candidate content seen: after "h": h, here, had, have, has, ha, hack, habit; after "hi": hi, his, him, himself, hi-fi,
hibernate, hibernates, hibernation (eight lines, no selection highlight). The dictionary is the console IME's. The firmware does
ship English dictionaries (`dev_flash/data/dic/Zi8DatENAM.z8d` and `Zi8DatENUK.z8d`, the Zi eZiText format used by
`vsh/module/ime_zi.sprx`), but they are stored compressed and this pass could not read them (OSK-FIRMWARE 8.2.1), so the native
list stays empty and no candidates, counter values or word list are fabricated. The "a 0" counter reads 0 in the recording
with eight candidates showing, so an empty native list does not change the counter.

## 5. Dim behind the window

The darkening is a black layer whose final alpha is about 0.68-0.70 (background 131 -> 42 at (100,100), multiplier
0.32). It is not part of the window's fade: it eases up over about a second after the window opens and is held while
the window fades out.

Background luma at (100,100) per 100 ms frame: 36.3 s 131; 36.4 124; 36.5 102; 36.6 86; 36.7 73; 36.8 65; 36.9 59; 37.0 55;
37.1 50; 37.2 48; 37.3 46; 37.4 43; 37.5 42. The remaining fraction shrinks by about 0.74 per 100 ms (the paf accel curve
at 60 Hz): `dim(t) = 0.70 * (1 - 0.74^(t/100 ms))`, t from the open. At 39.9 s (window nearly gone) the dim is still full; at
40.0 s the calling dialog is bright again, so the release takes under 100 ms (modelled as 80 ms, FIT).

## 6. Focus move and key highlight timing

* 'k' (37.5 -> 38.0 s): 189 at 37.4, 195, 213, 221 at 37.7, then 212, 195, 188 at 38.0. Rise ~150 ms, fall ~200 ms.
* 'i' (37.8 -> 38.5 s): 189, 195, 209, 215 from 38.0 to 38.3, then 202, 189 at 38.5: rise ~200 ms, fall ~200 ms.
* Space (38.7 -> 39.2 s): (142,175,180) -> (146,198,211) -> (153,212,230) at 38.9 -> (145,187,199) -> (142,173,178) at 39.2.
* So a move is a cross-fade: the old key's highlight falls over ~200 ms while the new one rises over ~150 ms (FIT: linear).
* The first focus (g) appears after the window is in: no highlight at 36.4 s, ring plus partial fill at 36.5, full fill
  at 36.7, steady after.
* No sound or slide on the move is visible. The panel does not move on a focus change.

## 7. Open and close

Open (frame 36.4 s, the first frame with the window): the 622 px window is 654 px wide (scale 1.051) about the screen
centre and about half transparent; the keys show as pale pink blocks over the dialog behind (only "1" is already inked);
the next frame (36.5 s) is at rest. Close (39.8 -> 39.9 s): 39.8 is scale 1.036 and nearly opaque; 39.9 is scale 1.106
and about 25% opaque with only the frame and the dialog behind visible. 40.0 s: gone, dialog fully bright.

Model used (FIT; the 100 ms step cannot separate scale from alpha): alpha 0 -> 1 over 80 ms on open and 1 -> 0 over
104 ms on close (the 45 fps title-bar luma of OSK-FIRMWARE 13.2), scale `1 + 0.10 * (1 - alpha)` both ways, centred on
the screen centre. Key captions staggering in after the frame is UNKNOWN (one frame shows the "1" first).

## 8. States not in the recording (UNKNOWN)

Caps page, accent page, symbol palette, tenkey, masked entry, key repeat, Input Mode, copy and paste with content, the
composition commit, and the footer toast: no frame shows them. Their native drawing is unchanged by this pass and
still follows OSK-FIRMWARE / the 1080p captures.

## 9. Footer of the calling dialog

The `X Edit` and `O Back` pair (white (255) at 60% when dimmed, x 490 / 777, y 604) belongs to the text-entry dialog
behind the keyboard, not to the keyboard; it dims with the rest of the dialog. The native keyboard is opened by many
callers and draws no such pair.

## 10. Where this differs from OSK-FIRMWARE.md

| Topic | OSK-FIRMWARE | Recording | Native after this pass |
|---|---|---|---|
| Symbol key caption | `@#:` | `*%#` | `*%#` |
| Caret | 27 px, 12 px serifs | 22 px (33), 4 px serifs (7) | recording's |
| Palette key icon | 4 x 3 squares | 3 x 3 flat cells | 3 x 3 |
| Prediction column | copy 62 px + paste 62 px | one wide Paste, list y 271..424 | one wide Paste; candidates still absent (no readable dictionary) |
| Composition underline | not recorded | 2 px grey bar under the typed word, ends at a space | drawn (`oskCompositionRange`) |
| Function captions | centred | right-aligned | right-aligned |
| Title text | 27 px at x 520 | 23 px at x 531 | 23 px at x 531 |
| Open/close | alpha only | alpha and scale 1.10 -> 1.00, dim eased separately | both |
| Focus | instant brighten | cross-fade, own fill gradients | cross-fade, recorded fills |
