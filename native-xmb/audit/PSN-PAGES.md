# PSN column pages, older firmware look (2026-11-22)

Target: the older recording (m2) per the owner's decision in `VIDEO-GAP-ANALYSIS-2026-11-22.md` section 9. The newer
recording (R) was used only for the Account Management tree layout. There is no PSN backend: every page here is
presentation, and none of them can show an account, a balance, a user or a signed-in state.

## What was added

| Surface | Source | Truthful ending |
|---|---|---|
| Sign In page (`View::drawPsnSignInPage`) | m2 51 s and 52 s | Sign In shows "Signing in... Please wait." for 900 ms, then the existing "This application does not connect to PlayStation Network..." page. Forgot your password? gives its own not-connected page. Nothing typed is sent, kept, logged or compared; the password is masked and zeroed when the page closes. |
| Account Management row (PSN column) | m2 48 s | Pushes the column page below. Its description line is the firmware string `explore_category_psn msg_can_change_psn_account_psstore_setteing`. |
| Account Management page (`psn_shell.cpp`) | R 96-99 s layout | Account Information (opens its own list), Redeem Codes, Transaction Management, System Activation, Facebook, Privacy Settings. Every leaf names itself and says it needs a PlayStation Network connection this application does not make. |
| Account Information page | R tree | Billing Information, Sign-In ID, Password, Name, Address, Profile, Notification Preferences, all explanatory leaves. Avatar is not built (it needs a signed-in account's own pictures). |

Pure logic (focus walk, check boxes, mask, wording, tree) is `src/psn_pages.h`, tested by
`tests/psn_pages_tests.cpp` (`xmb-psn-pages-tests`).

## Firmware assets used

- Strings: `npsignin_plugin` msg_id_email, msg_pw, msg_automatic_signin_from_next_time, msg_save_pw2, msg_signin, msg_reset_pw, msg_sigining_in; `system_plugin` msg_enter, msg_back, msg_cancel.
- Art: `npsignin_plugin` tex_button / tex_button_focus (glassy buttons), `system_plugin` tex_default_text_field(_focus) and the 44x252 tex_default_check strip (seven 44x36 cells: empty, three drawing-in frames, short tick, full tick, lit tick), `explore_category_psn` item_tex_account (the magnifier-over-face icon), `newstore_plugin` ButtonIconRedeem, ButtonIconCart, FacebookLogo, `explore_plugin_full` item_tex_ps3sd_folder and tex_lock_icon.

## PSN category icon

Checked every PSN texture in the 122 containers (`--rco-images`): `xmb_plugin_normal`, `explore_category_psn`,
`explore_plugin_full`, `xmb_ingame`, `friendtrophy_plugin(_game)`, `npsignin_plugin`, `np_multisignin_plugin` all hold the
same blue PlayStation-logo sphere (the 4.93 icon). The purple globe of the older recording is not in this firmware, so it
cannot be drawn from the asset catalogue; the icon is unchanged. (`regcam_plugin:Globe` is a grey globe, a different
object.)

## Geometry (1080p, measured from the m2 720p frames at 1.5x)

Rules y 199 / 879; fields x 556..1362, y 378..416 and 489..526; labels at x 558; check boxes centred x 751.5, y 562.5 and
603; Sign In button x 906..1014, y 642..690; Forgot button x 814..1106, y 723..767; footer glyphs x 726 and 1155. Text ink of
the labels, buttons and footer lands within 1 to 2 px of the frame at 720p (the Auto Sign-In caption is about 4% wider).

## Inferred, not shown by a frame

- Save Password is dimmed and ticked while Auto Sign-In is on (one frame shows the dimmed box under a ticked Auto Sign-In).
- Nothing is pre-ticked, because nothing was ever saved.
- The 900 ms pause length and the focus walk skipping the dimmed box.
