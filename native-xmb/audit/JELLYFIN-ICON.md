# Jellyfin icon

The Jellyfin rows (TV/Video Services > Jellyfin, Music > Jellyfin Music), the Set Up Jellyfin page and its server rows
show Jellyfin's own icon: the user's server icon when it has been fetched, otherwise a 256x256 PNG embedded in
`src/jellyfin_icon_data.cpp` (no runtime download), wired through `serviceIcons::jellyfin()` the same way as the Home
Assistant icon (see HOME-SERVICES.md). It is drawn through the same DPI-scaled icon path, so it stays crisp at 4K.
Generic action rows (Sign Out, Search, More, library and folder glyphs) keep their firmware icons on purpose.

## Source

`branding/SVG/icon-transparent.svg` from https://github.com/jellyfin/jellyfin-ux (master), rendered at 256 px width
with `@resvg/resvg-js` under bun (a one-off, not a repo build step).

## Licence finding (read 2026-11-15)

- The `jellyfin-ux` repository's `LICENSE` file is the full text of **Creative Commons Attribution-ShareAlike 4.0
  International (CC BY-SA 4.0)**. The SVG's own header says: "All copyright belongs to the Jellyfin contributors ...
  licensed under the Creative Commons Attribution-ShareAlike 4.0 International License".
- Its README says only that the repo holds shared UX elements (logos, fonts) and points to the branding guide at
  https://jellyfin.org/docs/general/contributing/branding/ . I could not extract that page's text (it is a scripted
  site), so any separate trademark or usage rule in the guide is **unread**. The README labels `branding/SVG` as
  deprecated; the same mark is in `logos/SVG`.
- Obligations of CC BY-SA 4.0: attribution (creator, licence link, note of changes) and share-alike for adaptations. The
  PNG is a straight raster of the unmodified SVG. Whether the share-alike clause reaches the whole application is a
  legal question I have not settled; the icon file itself stays CC BY-SA 4.0. Attribution is in THIRD-PARTY.md.
- "Jellyfin" the name may also be a trademark; the licence does not grant trademark rights. The owner should judge this.
