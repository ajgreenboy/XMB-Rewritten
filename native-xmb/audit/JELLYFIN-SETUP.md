# Jellyfin setup from inside the XMB

Written 2026-11-14. Everything below was checked against the build in `C:\dev\out\jf-build` (branch `wt/jf`) unless marked.

## The problem

Jellyfin only appeared in TV/Video Services after the owner hand-imported a manifest into `<data>\apps.json`
(`examples\jellyfin.json`, kind `jellyfin`, category `tv`). On a fresh install there was no Jellyfin row and no hint that
Jellyfin exists, so the feature could not be found.

## What it does now

| Step | Where | Detail |
|---|---|---|
| 1. The row | TV/Video Services, first row | `Jellyfin`, "Set up your Jellyfin server", the Jellyfin mark. Listed only while no app of kind `jellyfin` is registered, in the spot the registered Jellyfin row uses. On the first load the column opens on it (a registered Jellyfin keeps the old behaviour). |
| 2. The page | Cross on the row | An ordinary page of rows (the sign-in page's model): `Server Address`, a result row, `Add Jellyfin`, `Check Server`, `Look for Servers on This Network`, then one row per server found. |
| 3. The address | Cross on `Server Address` | The XMB keyboard (`requestJellyfinFieldEdit`, field `jfsetup:address`; no keyboard code was touched). When the keyboard is accepted the address is checked at once. A missing scheme means `http://`. |
| 4. The check | automatic, or `Check Server` | `GET <address>/System/Info/Public` through the shell's own `QNetworkAccessManager`, 5 s transfer timeout, answer capped at 64 KB, same-origin redirects only. Success shows the server's name and version and moves the focus to `Add Jellyfin`. |
| 5. The save | `Add Jellyfin` | `ContentRegistry::upsert` on `<data>\apps.json`: id `jellyfin`, title `Jellyfin`, kind `jellyfin`, category `tv`, the url. That is the writer `--import-apps` uses; a test proves the file is byte-identical to the import of the same manifest. Then `refreshContent()` (the F5 path) replaces the setup row with the normal Jellyfin row in place; no restart. A toast says "<server name> added". |
| 6. Discovery | `Look for Servers on This Network` | See below. A found server is a row; choosing it fills the address and checks it. Cross while it runs stops it. |
| 7. Change later | item menu (Triangle) of a registered Jellyfin row: `Change Server Address` | The same page, prefilled; `Save Address` changes only the url of that app (title, id, artwork and other fields are kept). If the address changed, the in-memory and remembered sign-in are dropped (they belong to the old server) and the cached server icon file is removed. |

## Failure messages

One plain sentence each, shown on the result row (the focus moves to it so the second line is on screen):

| Case | Message |
|---|---|
| Empty | Type the address of your Jellyfin server. |
| Not http(s) | Use an address that starts with http:// or https://. |
| Credentials in the address | Leave the user name and password out of the address. You sign in after it is added. The address is shown back with them removed. |
| Query or fragment | Leave off everything after a ? or a #. |
| Name does not resolve | The name X was not found. ... |
| Connection refused | X refused the connection. Check the port, and that Jellyfin is running. |
| No answer in 5 s | X did not answer in time. ... |
| HTTP 404 | X answered, but it is not Jellyfin (nothing at /System/Info/Public). ... |
| Other HTTP error | X answered with an error (N), so it is not a working Jellyfin server. |
| Redirect to another origin | X redirects somewhere else. Type the address it redirects to. |
| Not JSON, or other product (Emby) | The address answered, but not like a Jellyfin server. / That is a ..., not a Jellyfin server. |

`Save Anyway` replaces `Add Jellyfin` only when nothing answered at all (refused, unknown name, timeout): the server may
simply be off, and the address can be changed later. An address that answered as something else cannot be saved.
Safe mode (D6) refuses the check and the search like every other network action.

## Address rules

`ContentRegistry::isValidServerUrl` is now the one definition (http or https, a host, no credentials; for Jellyfin also no
query or fragment). The importer, the app editor and this page share it, and `parseAddress` only accepts what it
accepts. A trailing slash is removed; a reverse-proxy subpath is kept.

## Discovery (the optional part, done)

Jellyfin's own UDP auto-discovery, [TRACED to the server's protocol, not to firmware]: the datagram `who is JellyfinServer?`
to port 7359; a server replies with JSON (`Address`, `Id`, `Name`, `EndpointAddress`).

- Asynchronous (`QUdpSocket`, no blocking call anywhere), 3 s, cancellable (Cross again, or leaving the page), at most 12
  servers, replies deduplicated by server id, datagrams over 4 KB ignored.
- LAN only by construction: one datagram per up, running, broadcast-capable IPv4 interface whose own address is private
  (10/8, 172.16/12, 192.168/16, 169.254/16), sent to that subnet's directed broadcast. Never to a public address. Only a LAN
  sender is believed.
- Nothing is sent but that fixed string. The reply is untrusted: the address offered is the **sender's** IPv4 address with
  the scheme, port and path the reply announced, never the host the reply names. (This also fixes the common Docker case:
  a containerised Jellyfin announces its bridge address, for example `172.17.0.2`; the sender's address is the one that
  answers.) Text is stripped of control characters and bounded.
- Choosing a row runs the normal check; nothing is saved without it.

Limit, measured on the owner's server: Servatron (a Docker container on the main server) **answers a unicast
`who is JellyfinServer?` but does not receive the broadcast**, because Docker's bridge network does not forward LAN
broadcasts. So on that setup the search finds nothing and the owner types the address (one line). A Jellyfin on the host
network, on bare metal or on a VM with a bridged adapter answers. The reply parsing was checked against the shape of that server's real
reply text (it is a unit test sample) and the whole search-and-pick flow against a local stand-in responder.

## Privacy

Only the server's public information route is requested. No credential is typed on this page, sent or logged: the log line
for a refused address does not echo what was typed, and accepted addresses cannot contain credentials by rule. Nothing is
written to disk until `Add Jellyfin` / `Save Address`.

## Files

| File | Role |
|---|---|
| `src/jellyfin_setup.h/.cpp` | Pure logic (address, public-info parse, failure text, LAN test, discovery reply, registration save) and the two asynchronous helpers `ServerProbe` and `Discovery` |
| `src/jellyfin_setup_shell.cpp` | The page: rows, state, activation, save, refresh |
| `src/shell.cpp`, `src/shell.h` | The TV-column row, `Action::JellyfinSetup*`, `OptionCommand::JellyfinChangeAddress`, Back handling, the `jfsetup:address` field |
| `src/content_registry.*` | `isValidServerUrl` (one shared rule) |
| `tests/jellyfin_setup_tests.cpp` | `xmb-jellyfin-setup-tests <scratch dir ending in jellyfin-setup>`, option `XMB_JELLYFIN_SETUP_BUILD_TESTS`, in CI |

## Gate

No default pinned frame changed. Only `jellyfin-signin` shows the TV/Video Services column, and its seed `apps.json`
already registers Jellyfin, so the setup row is not listed there. Every other state is on another column. `XMB_NO_SETUP_ROWS=1`
(documented in INSTALL.md) hides the setup row for any future recipe that needs a clean TV column. No reference was saved.
Gate result: `bun work/ab-layers.ts --all` on this build: `states: 27/27 pass` (2026-11-14, PC unmuted).

## Not done / limits

- The static Jellyfin icon image is process-wide: after Change Server Address to a different server the new server's own
  icon appears at the next start (the old cached file is removed; the drawn mark is used meanwhile).
- No Quick Connect or any sign-in change; sign-in is the existing page.
- Real-world discovery success depends on the network (see the Docker note).
