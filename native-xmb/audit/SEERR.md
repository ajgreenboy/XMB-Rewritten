# Media Requests

**TV/Video Services → Seerr** connects XMB to your existing Seerr server. Enter the server's final HTTP or
HTTPS address, including its reverse proxy prefix if it has one. Paste the API key from Seerr's **Settings → General**,
test the connection, then save. Use HTTPS outside a trusted network. XMB masks the key and protects it together with
the address in a Windows DPAPI record in the native data directory.

Search for a movie or series, select a result for its details, and confirm the request. Choose individual seasons
for television. Seerr uses its configured default Sonarr or Radarr server and quality policy. **My Requests** shows
the requests visible to the configured key. Network → Download Management refreshes approved, active requests every five seconds while the page is open. It shows progress when Seerr reports it. The Seerr-managed rows provide Status in their Triangle menu.

For SABnzbd controls, open **TV/Video Services → Seerr → Download Client**. Save the SABnzbd base URL,
including any reverse proxy prefix, and its full API key from **Config → General**. XMB encrypts these settings
in the native data directory. Pause, Resume and Cancel appear in Download Management when SABnzbd confirms
the transfer IDs for the request. Cancel keeps downloaded files and can clear temporary data. Seerr retains the
request and may find a replacement.

Seerr handles approval, release selection, the download client, and library import. Playback uses your Jellyfin
library once Jellyfin has indexed the imported title.

API contract: [Seerr OpenAPI specification](https://github.com/seerr-team/seerr/blob/develop/seerr-api.yml).

A local fixture verified configuration, search, cancellation before submission, one movie request, one request
for TV season 2, and a tracked download percentage in My Requests at 960×540. A live Seerr server was outside
that check.

Search and detail posters come from TMDB paths supplied by Seerr. XMB fetches the images from the TMDB
image host without a Seerr API key. The bundled application icon is Seerr's own 192 px application icon, distributed
with its upstream license.

- [Seerr icon source](https://github.com/seerr-team/seerr/blob/main/public/android-chrome-192x192.png)
- [TMDB image URL format](https://developer.themoviedb.org/docs/image-basics)