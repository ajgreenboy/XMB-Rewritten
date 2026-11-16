// Local stand-in for a Jellyfin 10.10+/12 server, for scripted XMB runs (audit/JELLYFIN-PLAYBACK.md).
//
//   bun work/jellyfin-standin.ts [--port 8099] [--log <file>] [--mode strict|lenient] [--hls-status <n>]
//
// Phase 2 (audit/JELLYFIN-PLAYER.md) adds /Items/Latest, favourites (Filters=IsFavorite and POST/DELETE /UserFavoriteItems), DELETE
// /UserPlayedItems, SeriesId on episodes, a real video on the direct-play stream (--clip <file>) and the flags --no-latest --no-favorites
// --no-flags --legacy-flags --episodes-direct --series-done.
// It answers the routes the XMB's Jellyfin client uses: sign-in (any user name and password), views, a
// folder of movies with a distinct backdrop each, PlaybackInfo (DirectPlay, and Transcode with a
// TranscodingUrl in the shape Jellyfin 12 returns: relative, ApiKey in the query), the HLS endpoints
// (master.m3u8, main.m3u8, segments) and the playback reports. It is STRICT about what a real server
// requires (an Authorization header or ApiKey, mediaSourceId on master.m3u8, a valid DeviceId, a
// PlaySessionId) and answers a deviation with a 4xx/5xx JSON body naming the problem, so a client bug shows
// up as a precise line in the log instead of a generic failure. Every request is logged as one JSON line
// (method, path, query KEYS only, whether auth was present, user agent). Tokens and values never are.
import { createServer } from "node:http";
import { deflateSync } from "node:zlib";
import { appendFileSync, readFileSync, writeFileSync } from "node:fs";

const args = process.argv.slice(2);
const opt = (name: string, dflt: string) => {
  const i = args.indexOf(`--${name}`);
  return i >= 0 && args[i + 1] ? args[i + 1] : dflt;
};
const port = Number(opt("port", "8099"));
const logFile = opt("log", "");
const strict = opt("mode", "strict") === "strict";
const hlsStatus = Number(opt("hls-status", "0")); // force master.m3u8 to answer this status (diagnostics test)
const hlsBody = opt("hls-body", "Error processing request.");
const noLyrics = args.includes("--no-lyrics");                 // /Audio/{id}/Lyrics answers 404 (a server or track without lyrics)
const plainLyrics = args.includes("--plain-lyrics");           // lyrics without timestamps (the XMB shows only synced lyrics)
const noMusic = args.includes("--no-music");                 // the server has no music library (the Music column says so)
const secondLibrary = args.includes("--second-library");       // a second music library (the Music page lists the libraries first)
const audiobook = args.includes("--audiobook");                 // one 25 minute audio item ("The Long Story") that keeps a resume position
const musicTranscode = args.includes("--music-transcode");     // audio is served through a Transcode answer (/Audio/{id}/universal)
const extraSongs = Number(opt("extra-songs", "0"));            // filler songs, to page through a large library
const musicStatus = Number(opt("music-status", "0"));          // force every music list to answer this status (error text test)
const noHome = args.includes("--no-home");                     // the server has no Resume / NextUp routes at all (they answer 404)
const legacyHome = args.includes("--legacy-home");             // only the pre-10.9 user-scoped Resume route exists
const noChapters = args.includes("--no-chapters");             // neither PlaybackInfo nor /Items carries chapter marks
const clipFile = opt("clip", "");                              // a real video file served (with Range) on the direct-play stream route instead of garbage bytes
const episodesDirect = args.includes("--episodes-direct");     // every episode direct-plays (with --clip, the whole Play Next Episode chain plays real video)
const seriesDone = args.includes("--series-done");             // /Shows/NextUp?seriesId= answers an empty list (the series has nothing left)
const noLatest = args.includes("--no-latest");                 // the server has no /Items/Latest routes (they answer 404)
const noFavorites = args.includes("--no-favorites");           // the server answers the Filters=IsFavorite list and the favourite routes with 404
const noFlags = args.includes("--no-flags");                   // the watched and favourite write routes answer 404 (a server that refuses them)
const legacyFlags = args.includes("--legacy-flags");           // only the pre-10.9 /Users/{id}/PlayedItems and /FavoriteItems routes exist

// A fixed token (STANDIN_TOKEN) lets a scripted run restore a stored sign-in instead of typing credentials through the keyboard.
const TOKEN = process.env.STANDIN_TOKEN || "standin-" + Math.random().toString(16).slice(2) + Math.random().toString(16).slice(2);
const USER = "0123456789abcdef0123456789abcdef";
const VIEW = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaa01";

function log(entry: Record<string, unknown>) {
  const line = JSON.stringify(entry);
  console.log(line);
  if (logFile) appendFileSync(logFile, line + "\n");
}

// ---- Items ---------------------------------------------------------------------------------------
type Movie = { id: string; name: string; color: [number, number, number]; backdrop: boolean; method: "direct" | "transcode"; sub?: number; ticks: number };
const movies: Movie[] = [
  { id: "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbb01", name: "Alpha Direct", color: [200, 40, 40], backdrop: true, method: "direct", ticks: 200000000 },
  { id: "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbb02", name: "Bravo Transcode", color: [40, 200, 40], backdrop: true, method: "transcode", ticks: 200000000 },
  { id: "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbb03", name: "Charlie Burn In", color: [40, 40, 200], backdrop: true, method: "transcode", sub: 3, ticks: 200000000 },
  { id: "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbb04", name: "Delta No Backdrop", color: [200, 200, 40], backdrop: false, method: "direct", ticks: 200000000 },
  { id: "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbb05", name: "Echo", color: [200, 40, 200], backdrop: true, method: "direct", ticks: 200000000 },
];

// Episodes for the Next Up row (a second, unrelated show is not needed: one show with two unwatched episodes is enough).
const episodes: Movie[] = [
  { id: "cccccccccccccccccccccccccccccc01", name: "Pilot", color: [90, 160, 220], backdrop: true, method: "direct", ticks: 300000000 },
  { id: "cccccccccccccccccccccccccccccc02", name: "The Second One", color: [220, 160, 90], backdrop: true, method: "transcode", ticks: 300000000 },
];
const isEpisode = (m: Movie) => m.id.startsWith("cccc");
const everything = [...movies, ...episodes];
// What the server remembers about this user's progress. Reports from the XMB change it, exactly as the real
// server's UserDataManager would: a stop beyond 90 percent is "played" with no resume point.
const userData = new Map<string, { ticks: number; played: boolean; count?: number; last?: number }>();
let playClock = 0;   // a counter standing in for LastPlayedDate: bigger = more recent
const favorites = new Set<string>();   // items this user flagged as favourite
const SERIES = "eeeeeeeeeeeeeeeeeeeeeeeeeeeeee01";
const reports: Array<{ kind: string; item: string; ticks: number | null; paused: unknown; session: string }> = [];
const dataOf = (m: { id: string }) => userData.get(m.id) ?? { ticks: 0, played: false };
// Keeps the play count and the last-played stamp when a flag or position changes (the real server's UserData is one record).
const setData = (id: string, patch: { ticks: number; played: boolean }, counted = false) => {
  const old = userData.get(id);
  userData.set(id, { ...patch, count: (old?.count ?? 0) + (counted ? 1 : 0), last: counted ? ++playClock : old?.last });
};
// Chapters of a title: four marks, the second one unnamed.
const chaptersOf = (m: Movie) => noChapters ? [] : [0, 1, 2, 3].map((i) => ({ StartPositionTicks: Math.floor(m.ticks / 4) * i, Name: i === 1 ? "" : "Chapter mark " + (i + 1) }));

function itemJson(m: Movie, withChapters = false) {
  const ud = dataOf(m);
  return {
    Id: m.id, Name: m.name, Type: isEpisode(m) ? "Episode" : "Movie", IsFolder: false, MediaType: "Video", RunTimeTicks: m.ticks,
    ...(isEpisode(m) ? { SeriesName: "The Stand-in Show", SeriesId: SERIES, ParentIndexNumber: 1, IndexNumber: Number(m.id.slice(-2)) } : {}),
    ...(withChapters ? { Chapters: chaptersOf(m) } : {}),
    UserData: { Played: ud.played, PlaybackPositionTicks: ud.ticks, IsFavorite: favorites.has(m.id) },
    ImageTags: { Primary: "p" + m.id.slice(-2) },
    BackdropImageTags: m.backdrop ? ["b" + m.id.slice(-2)] : [],
  };
}

// ---- PNG (solid colour with a diagonal so frames are not flat) --------------------------------------
function crc32(buf: Uint8Array) {
  let c, crc = 0xffffffff;
  for (let n = 0; n < buf.length; n++) {
    c = (crc ^ buf[n]) & 0xff;
    for (let k = 0; k < 8; k++) c = c & 1 ? 0xedb88320 ^ (c >>> 1) : c >>> 1;
    crc = (crc >>> 8) ^ c;
  }
  return (crc ^ 0xffffffff) >>> 0;
}
function chunk(type: string, data: Uint8Array) {
  const out = Buffer.alloc(12 + data.length);
  out.writeUInt32BE(data.length, 0);
  out.write(type, 4, "ascii");
  Buffer.from(data).copy(out, 8);
  out.writeUInt32BE(crc32(out.subarray(4, 8 + data.length)), 8 + data.length);
  return out;
}
function png(w: number, h: number, rgb: [number, number, number]) {
  const raw = Buffer.alloc((w * 3 + 1) * h);
  for (let y = 0; y < h; y++) {
    raw[y * (w * 3 + 1)] = 0;
    for (let x = 0; x < w; x++) {
      const o = y * (w * 3 + 1) + 1 + x * 3;
      const shade = (x + y) % 160 < 80 ? 1 : 0.6;
      raw[o] = rgb[0] * shade; raw[o + 1] = rgb[1] * shade; raw[o + 2] = rgb[2] * shade;
    }
  }
  const ihdr = Buffer.alloc(13);
  ihdr.writeUInt32BE(w, 0); ihdr.writeUInt32BE(h, 4); ihdr[8] = 8; ihdr[9] = 2;
  return Buffer.concat([Buffer.from([137, 80, 78, 71, 13, 10, 26, 10]), chunk("IHDR", ihdr), chunk("IDAT", deflateSync(raw)), chunk("IEND", new Uint8Array())]);
}

// ---- helpers -------------------------------------------------------------------------------------
function authOf(req: any, query: URLSearchParams): { ok: boolean; via: string; deviceId?: string } {
  const header = String(req.headers["authorization"] ?? req.headers["x-emby-authorization"] ?? "");
  const m = /Token="([^"]*)"/.exec(header);
  const d = /DeviceId="([^"]*)"/.exec(header);
  if (m && m[1] === TOKEN) return { ok: true, via: "header", deviceId: d?.[1] };
  const key = query.get("ApiKey") ?? query.get("api_key");
  if (key === TOKEN) return { ok: true, via: "query" };
  return { ok: false, via: "none" };
}

function send(res: any, status: number, body: string | Buffer, type = "application/json") {
  res.writeHead(status, { "Content-Type": type, "Content-Length": Buffer.byteLength(body) });
  res.end(body);
}

// ---- Music (audit/JELLYFIN-MUSIC.md) -----------------------------------------------------------------
const MUSIC_VIEW = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaa02";
const MUSIC_VIEW2 = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaa03";
const hex = (prefix: string, n: number) => prefix.repeat(28) + n.toString(16).padStart(4, "0");
type Artist = { id: string; name: string; hue: number };
type Album = { id: string; name: string; artist: Artist; year: number; hue: number; added: number };
type Track = { id: string; name: string; artist: Artist; album: Album; no: number; hz: number; ticks: number };
const artists: Artist[] = ["Aurora Lane", "Blue Harbor", "Cedar Quartet"].map((name, i) => ({ id: hex("1", i + 1), name, hue: 30 + i * 110 }));
const albumDefs: [string, number, number, string[]][] = [
  ["Morning Static", 0, 2019, ["Wake Up Call", "Coffee Hum", "Signal Fade", "Late Bus", "Sunrise Loop"]],
  ["Night Signals", 0, 2021, ["Streetlight", "After Hours", "Neon Rain", "Last Train"]],
  ["Harbor Lights", 1, 2018, ["Low Tide", "Foghorn", "Pier Nine"]],
  ["Quartet No. 1", 2, 2020, ["Allegro", "Adagio", "Scherzo"]],
];
const albums: Album[] = albumDefs.map(([name, a, year], i) => ({ id: hex("2", i + 1), name, artist: artists[a], year, hue: 20 + i * 80, added: 4 - i }));
const tracks: Track[] = [];
albumDefs.forEach(([, , , names], ai) => names.forEach((name, ti) => tracks.push({
  id: hex("3", tracks.length + 1), name, artist: albums[ai].artist, album: albums[ai], no: ti + 1,
  hz: 220 + 40 * tracks.length, ticks: 300000000, // 30 s of a sine tone, one pitch per track
})));
const LONG_AUDIO_TICKS = 12000000000;   // 20 minutes
if (audiobook) {
  const book: Album = { id: hex("2", 98), name: "Audiobooks", artist: artists[2], year: 2015, hue: 140, added: 0 };
  albums.push(book);
  tracks.push({ id: hex("3", 2000), name: "The Long Story", artist: book.artist, album: book, no: 1, hz: 262, ticks: 15000000000 });   // 25 minutes of metadata; the file is the 30 s tone
}
if (extraSongs > 0) {
  const filler: Album = { id: hex("2", 99), name: "Filler Collection", artist: artists[1], year: 2000, hue: 200, added: 0 };
  albums.push(filler);
  for (let i = 0; i < extraSongs; i++) tracks.push({ id: hex("3", 1000 + i), name: `Filler Song ${String(i + 1).padStart(4, "0")}`, artist: filler.artist, album: filler, no: i + 1, hz: 330, ticks: 300000000 });
}
const playlists = [{ id: hex("4", 1), name: "Road Trip", items: [tracks[0], tracks[5], tracks[9], tracks[12], tracks[2], tracks[7]] }];
const musicIds = new Map<string, { hue: number }>();
for (const a of artists) musicIds.set(a.id, a);
for (const a of albums) musicIds.set(a.id, a);
for (const p of playlists) musicIds.set(p.id, { hue: 300 });

const imageTag = (id: string) => "t" + id.slice(-3);
const artistJson = (a: Artist) => ({ Id: a.id, Name: a.name, SortName: a.name.toLowerCase(), Type: "MusicArtist", IsFolder: true, ImageTags: { Primary: imageTag(a.id) } });
const albumJson = (a: Album) => ({ Id: a.id, Name: a.name, SortName: a.name.toLowerCase(), Type: "MusicAlbum", IsFolder: true, AlbumArtist: a.artist.name, ProductionYear: a.year, ImageTags: { Primary: imageTag(a.id) }, UserData: { IsFavorite: favorites.has(a.id) } });
// Any item a flag route can name: a video, a track, an album or an artist.
const lookup = (id: string): { id: string } | undefined => everything.find((x) => x.id === id) ?? tracks.find((x) => x.id === id) ?? albums.find((x) => x.id === id) ?? artists.find((x) => x.id === id);
const trackJson = (t: Track) => {
  const ud = dataOf(t);
  return {
  Id: t.id, Name: t.name, Type: "Audio", MediaType: "Audio", IsFolder: false, RunTimeTicks: t.ticks, IndexNumber: t.no, ParentIndexNumber: 1,
  Artists: [t.artist.name], AlbumArtist: t.artist.name, Album: t.album.name, AlbumId: t.album.id, AlbumPrimaryImageTag: imageTag(t.album.id), ProductionYear: t.album.year,
  UserData: { Played: ud.played, PlaybackPositionTicks: ud.ticks, PlayCount: ud.count ?? 0, IsFavorite: favorites.has(t.id), ...(ud.last ? { LastPlayedDate: new Date(Date.UTC(2026, 10, 16, 0, 0, ud.last)).toISOString() } : {}) }, ImageTags: {},
  };
};
const playlistJson = (p: { id: string; name: string }) => ({ Id: p.id, Name: p.name, Type: "Playlist", MediaType: "Audio", IsFolder: true, ImageTags: {} });

// A mono 16-bit 22.05 kHz sine tone in a WAV container: the audio file the player can decode.
const tones = new Map<number, Buffer>();
function wav(hz: number, seconds = 30) {
  let b = tones.get(hz);
  if (b) return b;
  const rate = 22050, n = rate * seconds, data = Buffer.alloc(n * 2);
  for (let i = 0; i < n; i++) {
    const fade = Math.min(1, i / 2000, (n - i) / 2000);
    data.writeInt16LE(Math.round(Math.sin((2 * Math.PI * hz * i) / rate) * 9000 * fade), i * 2);
  }
  const head = Buffer.alloc(44);
  head.write("RIFF", 0); head.writeUInt32LE(36 + data.length, 4); head.write("WAVEfmt ", 8); head.writeUInt32LE(16, 16);
  head.writeUInt16LE(1, 20); head.writeUInt16LE(1, 22); head.writeUInt32LE(rate, 24); head.writeUInt32LE(rate * 2, 28);
  head.writeUInt16LE(2, 32); head.writeUInt16LE(16, 34); head.write("data", 36); head.writeUInt32LE(data.length, 40);
  b = Buffer.concat([head, data]);
  tones.set(hz, b);
  return b;
}

// Pages a list the way Jellyfin does: StartIndex and Limit in, the window and the whole count out.
function windowOf<T>(list: T[], q: URLSearchParams, json: (x: T) => unknown) {
  const start = Math.max(0, Number(q.get("StartIndex") ?? 0));
  const limit = Math.min(1000, Number(q.get("Limit") ?? 100));
  return { Items: list.slice(start, start + limit).map(json), TotalRecordCount: list.length, StartIndex: start };
}
const byName = <T extends { name: string }>(a: T, b: T) => a.name.toLowerCase().localeCompare(b.name.toLowerCase());
const likes = (name: string, term: string) => name.toLowerCase().includes(term.toLowerCase());
function hsl(h: number): [number, number, number] {
  const f = (n: number) => { const k = (n + h / 30) % 12; return Math.round(255 * (0.55 - 0.35 * Math.max(-1, Math.min(k - 3, 9 - k, 1)))); };
  return [f(0), f(8), f(4)];
}

// Returns true when it answered. Strict: a music list needs UserId, as a real server's user-scoped lists do.
function musicRoutes(req: any, res: any, path: string, q: URLSearchParams, body: Buffer): boolean {
  const fail = (status: number, msg: string) => { send(res, status, JSON.stringify({ Message: msg })); return true; };
  const reply = (obj: unknown) => { send(res, 200, JSON.stringify(obj)); return true; };
  const listRoute = path === "/Artists/AlbumArtists" || (req.method === "GET" && path === "/Items" && /MusicAlbum|Audio|MusicArtist|Playlist/.test(q.get("IncludeItemTypes") ?? "")) || /^\/Playlists\/[0-9a-f]{32}\/Items$/.test(path);
  if (listRoute) {
    if (musicStatus) return fail(musicStatus, "Error processing request.");
    if (noMusic) return fail(404, "no music library");
    if (strict && !q.get("UserId")) return fail(400, "UserId is required for a user scoped list");
  }
  if (path === "/Artists/AlbumArtists") return reply(windowOf([...artists].sort(byName), q, artistJson));
  let m: RegExpExecArray | null;
  if ((m = /^\/Playlists\/([0-9a-f]{32})\/Items$/.exec(path))) {
    const p = playlists.find((x) => x.id === m![1]);
    return p ? reply(windowOf(p.items, q, trackJson)) : fail(404, "no playlist");
  }
  if (req.method === "GET" && path === "/Items") {
    const types = (q.get("IncludeItemTypes") ?? "").split(",");
    const term = q.get("SearchTerm");
    const filters = (q.get("Filters") ?? "").split(",");
    if (filters.includes("IsFavorite") && (types.includes("Audio") || types.includes("MusicAlbum"))) {   // flagged songs and albums, by name
      if (noFavorites) return fail(404, "Not found: favourites");
      const list: Array<{ name: string; json: unknown }> = [
        ...albums.filter((a) => favorites.has(a.id)).map((a) => ({ name: a.name, json: albumJson(a) })),
        ...tracks.filter((t) => favorites.has(t.id)).map((t) => ({ name: t.name, json: trackJson(t) })),
      ].sort((a, b) => a.name.toLowerCase().localeCompare(b.name.toLowerCase()));
      return reply(windowOf(list, q, (x) => x.json));
    }
    if (filters.includes("IsPlayed") && types.includes("Audio")) {   // Recently Played (DatePlayed) and Most Played (PlayCount)
      const list = tracks.filter((t) => dataOf(t).played);
      const sort = q.get("SortBy") ?? "";
      if (sort.startsWith("PlayCount")) list.sort((a, b) => (dataOf(b).count ?? 0) - (dataOf(a).count ?? 0) || byName(a, b));
      else list.sort((a, b) => (dataOf(b).last ?? 0) - (dataOf(a).last ?? 0));
      return reply(windowOf(list, q, trackJson));
    }
    if (term && types.includes("MusicArtist")) {
      const found: unknown[] = [
        ...artists.filter((a) => likes(a.name, term)).map(artistJson),
        ...albums.filter((a) => likes(a.name, term)).map(albumJson),
        ...tracks.filter((t) => likes(t.name, term)).slice(0, 40).map(trackJson),
      ];
      return reply({ Items: found, TotalRecordCount: found.length, StartIndex: 0 });
    }
    if (types.includes("MusicAlbum")) {
      let list = [...albums];
      const artistId = q.get("ArtistIds");
      if (artistId) list = list.filter((a) => a.artist.id === artistId);
      const sort = q.get("SortBy") ?? "SortName";
      if (sort === "Random") list.sort(() => Math.random() - 0.5);
      else if (sort.startsWith("DateCreated")) list.sort((a, b) => b.added - a.added || byName(a, b));
      else if (sort.startsWith("ProductionYear")) list.sort((a, b) => a.year - b.year || byName(a, b));
      else list.sort(byName);
      return reply(windowOf(list, q, albumJson));
    }
    if (types.includes("Audio")) {
      const parent = q.get("ParentId");
      const album = albums.find((a) => a.id === parent);
      if (parent && !album && parent !== MUSIC_VIEW && parent !== MUSIC_VIEW2) return reply({ Items: [], TotalRecordCount: 0, StartIndex: 0 });
      const artistId = q.get("ArtistIds");
      const list = album ? tracks.filter((t) => t.album === album)
        : artistId ? tracks.filter((t) => t.artist.id === artistId).sort((a, b) => a.album.name.localeCompare(b.album.name) || a.no - b.no)   // an artist's songs, album by album
        : [...tracks].sort(byName);
      return reply(windowOf(list, q, trackJson));
    }
    if (types.includes("Playlist")) {
      if ((q.get("MediaTypes") ?? "") !== "Audio") return fail(400, "audio playlists are asked for with MediaTypes=Audio");
      return reply(windowOf(playlists, q, playlistJson));
    }
  }
  if ((m = /^\/Items\/([0-9a-f]{32})\/Images\/Primary$/.exec(path)) && musicIds.has(m[1])) {
    send(res, 200, png(300, 300, hsl(musicIds.get(m[1])!.hue)), "image/png");
    return true;
  }
  if (req.method === "POST" && (m = /^\/Items\/([0-9a-f]{32})\/PlaybackInfo$/.exec(path)) && tracks.some((t) => t.id === m![1])) {
    const t = tracks.find((x) => x.id === m![1])!;
    let info: any = {};
    try { info = JSON.parse(body.toString("utf8")); } catch { return fail(400, "bad json"); }
    const direct = (info.DeviceProfile?.DirectPlayProfiles ?? []).find((p: any) => p.Type === "Audio");
    if (strict && !info.DeviceId) return fail(400, "DeviceId missing");
    if (strict && !direct) return fail(400, "an audio DirectPlayProfile is required");
    const canDirect = !musicTranscode && String(direct?.Container ?? "").split(",").includes("wav");
    const playSessionId = "ps" + Math.random().toString(16).slice(2, 18);
    sessions.set(playSessionId, { item: t.id, transcodeStarted: false });   // so the start / progress / stopped reports of this play are accepted in strict mode
    const source: any = { Id: t.id, Protocol: "File", Container: "wav", RunTimeTicks: t.ticks, SupportsDirectPlay: canDirect, SupportsDirectStream: false, SupportsTranscoding: true,
      MediaStreams: [{ Index: 0, Type: "Audio", Codec: "pcm_s16le", Channels: 1, DisplayTitle: "WAV 22 kHz mono", IsDefault: true }] };
    if (!canDirect) {
      const tq = new URLSearchParams({ DeviceId: String(info.DeviceId ?? ""), MediaSourceId: t.id, PlaySessionId: playSessionId, ApiKey: TOKEN, Container: "wav", AudioCodec: "pcm_s16le" });
      source.TranscodingUrl = `/Audio/${t.id}/universal?${tq.toString()}`;
      source.TranscodingSubProtocol = "http";
      source.TranscodingContainer = "wav";
    }
    return reply({ MediaSources: [source], PlaySessionId: playSessionId });
  }
  // Lyrics (Jellyfin 10.9+): {Metadata, Lyrics:[{Text, Start(ticks)}]}; Start is absent on unsynced lyrics. The first track of every album has lyrics.
  if (req.method === "GET" && (m = /^\/Audio\/([0-9a-f]{32})\/Lyrics$/.exec(path)) && tracks.some((t) => t.id === m![1])) {
    const t = tracks.find((x) => x.id === m![1])!;
    if (noLyrics || t.no !== 1) return fail(404, "no lyrics");
    const words = ["Wake up, the signal fades", "", "Coffee hum in the morning light", "Late bus on a quiet street", "Sunrise loops again"];
    const lines = words.map((text, i) => ({ Text: text, ...(plainLyrics ? {} : { Start: i * 50000000 }) }));   // one line every 5 s
    return reply({ Metadata: { Title: t.name, Artist: t.artist.name, IsSynced: !plainLyrics }, Lyrics: lines });
  }
  if ((m = /^\/Audio\/([0-9a-f]{32})\/(stream|universal)$/.exec(path)) && tracks.some((t) => t.id === m![1])) {
    const t = tracks.find((x) => x.id === m![1])!;
    if (strict && m[2] === "stream" && q.get("static") !== "true") return fail(400, "static required");
    if (strict && !q.get("MediaSourceId")) return fail(400, "The mediaSourceId field is required.");
    const file = wav(t.hz);
    const range = /^bytes=(\d*)-(\d*)$/.exec(String(req.headers["range"] ?? ""));
    if (range) {
      const from = range[1] ? Number(range[1]) : 0, to = range[2] ? Math.min(Number(range[2]), file.length - 1) : file.length - 1;
      res.writeHead(206, { "Content-Type": "audio/wav", "Content-Range": `bytes ${from}-${to}/${file.length}`, "Content-Length": to - from + 1, "Accept-Ranges": "bytes" });
      res.end(file.subarray(from, to + 1));
    } else {
      res.writeHead(200, { "Content-Type": "audio/wav", "Content-Length": file.length, "Accept-Ranges": "bytes" });
      res.end(file);
    }
    return true;
  }
  return false;
}

const sessions = new Map<string, { item: string; transcodeStarted: boolean }>();

createServer(async (req, res) => {
  const url = new URL(req.url ?? "/", "http://x");
  const path = url.pathname;
  const q = url.searchParams;
  const body: Buffer = await new Promise((r) => {
    const parts: Buffer[] = [];
    req.on("data", (c) => parts.push(c));
    req.on("end", () => r(Buffer.concat(parts)));
  });
  const auth = authOf(req, q);
  log({ m: req.method, path: path.replace(/[0-9a-f]{32}/g, (s) => s.slice(0, 4) + "…"), keys: [...q.keys()], auth: auth.via, ua: req.headers["user-agent"] ?? "", range: req.headers["range"] ?? "" });

  const fail = (status: number, msg: string) => send(res, status, JSON.stringify({ Message: msg }));

  if (req.method === "POST" && path === "/Users/AuthenticateByName") {
    return send(res, 200, JSON.stringify({ AccessToken: TOKEN, User: { Id: USER, Name: "standin" }, ServerId: "standin" }));
  }
  if (path === "/System/Info/Public") return send(res, 200, JSON.stringify({ ServerName: "Stand-in", Version: "12.2.0", ProductName: "Jellyfin Server", Id: "standin" }));
  if (!auth.ok) return fail(401, "Unauthorized");
  if (musicRoutes(req, res, path, q, body)) return;

  if (path === "/UserViews") {
    const views: any[] = [{ Id: VIEW, Name: "Movies", Type: "CollectionFolder", IsFolder: true, CollectionType: "movies" }];
    if (!noMusic) views.push({ Id: MUSIC_VIEW, Name: "Music", Type: "CollectionFolder", IsFolder: true, CollectionType: "music" });
    if (!noMusic && secondLibrary) views.push({ Id: MUSIC_VIEW2, Name: "Classical", Type: "CollectionFolder", IsFolder: true, CollectionType: "music" });
    return send(res, 200, JSON.stringify({ Items: views, TotalRecordCount: views.length, StartIndex: 0 }));
  }
  if (req.method === "GET" && path === "/Items") {
    if (q.get("Filters") === "IsFavorite") {
      if (noFavorites) return fail(404, "Not found: favourites");
      const flagged = everything.filter((x) => favorites.has(x.id));
      return send(res, 200, JSON.stringify({ Items: flagged.map((x) => itemJson(x)), TotalRecordCount: flagged.length, StartIndex: 0 }));
    }
    const ids = q.get("ids");
    if (ids) {
      const found = everything.filter((x) => ids.split(",").includes(x.id));
      return send(res, 200, JSON.stringify({ Items: found.map((x) => itemJson(x, (q.get("fields") ?? "").includes("Chapters"))), TotalRecordCount: found.length, StartIndex: 0 }));
    }
    return send(res, 200, JSON.stringify({ Items: movies.map((x) => itemJson(x)), TotalRecordCount: movies.length, StartIndex: 0 }));
  }
  let m: RegExpExecArray | null;
  // Continue Watching and Next Up (audit/JELLYFIN-PLAYER.md). /UserItems/Resume is the 10.9+ route; --legacy-home
  // keeps only /Users/{id}/Items/Resume; --no-home answers 404 to all of them.
  if (req.method === "GET" && (path === "/UserItems/Resume" || path === "/Shows/NextUp" || path === "/Users/" + USER + "/Items/Resume")) {
    if (noHome || (legacyHome && path === "/UserItems/Resume") || (path === "/Users/" + USER + "/Items/Resume" && !legacyHome)) return fail(404, "Not found: " + path);
    if (strict && q.get("userId") !== USER) return fail(400, "userId is required");
    if (path === "/Shows/NextUp" && seriesDone && q.get("seriesId")) return send(res, 200, JSON.stringify({ Items: [], TotalRecordCount: 0, StartIndex: 0 }));
    if (path === "/Shows/NextUp") {
      const next = episodes.filter((x) => !dataOf(x).played).slice(0, 1);
      return send(res, 200, JSON.stringify({ Items: next.map((x) => itemJson(x)), TotalRecordCount: next.length, StartIndex: 0 }));
    }
    const started = everything.filter((x) => dataOf(x).ticks > 0 && !dataOf(x).played);
    return send(res, 200, JSON.stringify({ Items: started.map((x) => itemJson(x)), TotalRecordCount: started.length, StartIndex: 0 }));
  }
  // The newest additions: a bare JSON array, as the real route answers (10.9+: /Items/Latest, older: /Users/{id}/Items/Latest).
  if (req.method === "GET" && (path === "/Items/Latest" || path === "/Users/" + USER + "/Items/Latest")) {
    if (noLatest || (path === "/Items/Latest") === legacyHome) return fail(404, "Not found: " + path);
    if (strict && q.get("userId") !== USER && q.get("UserId") !== USER) return fail(400, "userId is required");
    const parent = q.get("ParentId");
    const pool = parent && parent !== VIEW ? [] : [...movies].reverse();
    return send(res, 200, JSON.stringify(pool.slice(0, Number(q.get("Limit") ?? 20)).map((x) => itemJson(x))));
  }
  // The user's own flags. Favourite: POST sets, DELETE clears (10.9+: /UserFavoriteItems/{id}, older: /Users/{id}/FavoriteItems/{id}).
  // Played: POST marks watched, DELETE marks unwatched. --no-flags answers 404 to every one of them; --legacy-flags keeps only the older routes.
  if ((req.method === "POST" || req.method === "DELETE") && ((m = /^\/UserFavoriteItems\/([0-9a-f]{32})$/.exec(path)) || (m = /^\/Users\/[0-9a-f]{32}\/FavoriteItems\/([0-9a-f]{32})$/.exec(path)))) {
    if (noFlags || noFavorites || (path.startsWith("/UserFavoriteItems/") && legacyFlags)) return fail(404, "Not found: " + path);
    if (strict && path.startsWith("/UserFavoriteItems/") && q.get("userId") !== USER) return fail(400, "userId is required");
    const target = lookup(m![1]);
    if (!target) return fail(404, "no item");
    if (req.method === "POST") favorites.add(target.id); else favorites.delete(target.id);
    reports.push({ kind: req.method === "POST" ? "favorite" : "unfavorite", item: target.id, ticks: null, paused: null, session: "" });
    return send(res, 200, JSON.stringify({ IsFavorite: favorites.has(target.id), Played: dataOf(target).played }));
  }
  if (req.method === "DELETE" && ((m = /^\/UserPlayedItems\/([0-9a-f]{32})$/.exec(path)) || (m = /^\/Users\/[0-9a-f]{32}\/PlayedItems\/([0-9a-f]{32})$/.exec(path)))) {
    if (noFlags || (path.startsWith("/UserPlayedItems/") && legacyFlags)) return fail(404, "Not found: " + path);
    const target = lookup(m![1]);
    if (!target) return fail(404, "no item");
    setData(target.id, { ticks: 0, played: false });
    reports.push({ kind: "unplayed", item: target.id, ticks: null, paused: null, session: "" });
    return send(res, 200, JSON.stringify({ Played: false, PlaybackPositionTicks: 0 }));
  }
  // Marking an item watched (10.9+ route, and the older user-scoped one).
  if (req.method === "POST" && ((m = /^\/UserPlayedItems\/([0-9a-f]{32})$/.exec(path)) || (m = /^\/Users\/[0-9a-f]{32}\/PlayedItems\/([0-9a-f]{32})$/.exec(path)))) {
    if (noFlags || (path.startsWith("/UserPlayedItems/") && legacyFlags)) return fail(404, "Not found: " + path);
    const target = lookup(m![1]);
    if (!target) return fail(404, "no item");
    setData(target.id, { ticks: 0, played: true });
    reports.push({ kind: "played", item: target.id, ticks: null, paused: null, session: "" });
    return send(res, 200, JSON.stringify({ Played: true, PlaybackPositionTicks: 0 }));
  }
  // The test harness reads what the server was told (no credentials in it).
  if (req.method === "GET" && path === "/__reports") return send(res, 200, JSON.stringify({ reports, userData: Object.fromEntries(userData), favorites: [...favorites] }));
  if ((m = /^\/Items\/([0-9a-f]{32})\/Images\/(Primary|Backdrop)$/.exec(path))) {
    const movie = everything.find((x) => x.id === m![1]);
    if (!movie || (m[2] === "Backdrop" && !movie.backdrop)) return fail(404, "no image");
    // A short artificial delay for backdrops, longer for the first item, so a stale completion can arrive late.
    const delay = m[2] === "Backdrop" ? (movie === movies[0] ? 700 : 150) : 0;
    const img = m[2] === "Backdrop" ? png(640, 360, movie.color) : png(240, 360, movie.color);
    setTimeout(() => send(res, 200, img, "image/png"), delay);
    return;
  }
  if (req.method === "POST" && (m = /^\/Items\/([0-9a-f]{32})\/PlaybackInfo$/.exec(path))) {
    const movie = everything.find((x) => x.id === m![1]);
    if (!movie) return fail(404, "no item");
    let info: any = {};
    try { info = JSON.parse(body.toString("utf8")); } catch { return fail(400, "bad json"); }
    const profile = info.DeviceProfile ?? {};
    const burn = typeof info.SubtitleStreamIndex === "number" ? info.SubtitleStreamIndex : -1;
    const deviceId = String(info.DeviceId ?? "");
    if (strict && !deviceId) return fail(400, "DeviceId missing");
    const playSessionId = "ps" + Math.random().toString(16).slice(2, 18);
    // AudioStreamIndex in the request selects that stream and cannot be a static direct play (as on a real server).
    const wantAudio = typeof info.AudioStreamIndex === "number" ? info.AudioStreamIndex : -1;
    const direct = (movie.method === "direct" || (episodesDirect && isEpisode(movie))) && burn < 0 && wantAudio < 0 && info.EnableDirectPlay !== false;
    const streams = [
      { Index: 0, Type: "Video", Codec: "hevc", DisplayTitle: "1080p HEVC" },
      { Index: 1, Type: "Audio", Codec: "dts", Language: "eng", Channels: 6, DisplayTitle: "English DTS-HD MA 5.1", IsDefault: true },
      { Index: 2, Type: "Audio", Codec: "ac3", Language: "eng", Channels: 2, DisplayTitle: "English Commentary Stereo", IsDefault: false },
      { Index: 3, Type: "Subtitle", Codec: "pgssub", Language: "eng", DisplayTitle: "English PGS", IsTextSubtitleStream: false },
      { Index: 4, Type: "Subtitle", Codec: "subrip", Language: "eng", DisplayTitle: "English SRT", IsTextSubtitleStream: true },
    ];
    if (strict && wantAudio >= 0 && !streams.some((s) => s.Index === wantAudio && s.Type === "Audio")) return fail(400, "AudioStreamIndex " + wantAudio + " is not an audio stream of this item");
    const source: any = { Id: movie.id, Protocol: "File", Container: "mkv", RunTimeTicks: movie.ticks, MediaStreams: streams, SupportsDirectPlay: direct, SupportsDirectStream: false, SupportsTranscoding: true };
    if (!noChapters && movie.id.endsWith("01")) source.Chapters = chaptersOf(movie);   // the first title of each list carries them in PlaybackInfo; the others only via /Items?fields=Chapters
    if (!direct) {
      // The shape 10.9+ builds in StreamInfo.ToUrl: relative path, query beginning with DeviceId, ApiKey appended.
      const tq = new URLSearchParams();
      tq.set("DeviceId", deviceId);
      tq.set("MediaSourceId", movie.id);
      tq.set("VideoCodec", "h264");
      tq.set("AudioCodec", "aac");
      tq.set("AudioStreamIndex", String(wantAudio >= 0 ? wantAudio : 1));
      tq.set("VideoBitrate", String(Math.min(Number(info.MaxStreamingBitrate ?? 20000000), 20000000) - 192000));
      tq.set("AudioBitrate", "192000");
      tq.set("MaxFramerate", "23.976025");
      tq.set("PlaySessionId", playSessionId);
      tq.set("ApiKey", TOKEN);
      if (burn >= 0) { tq.set("SubtitleStreamIndex", String(burn)); tq.set("SubtitleMethod", "Encode"); }
      tq.set("TranscodingMaxAudioChannels", "2");
      tq.set("RequireAvc", "false");
      tq.set("EnableAudioVbrEncoding", "true");
      tq.set("Tag", "9f3c1e5a7b");
      tq.set("SegmentContainer", "ts");
      tq.set("MinSegments", "1");
      tq.set("BreakOnNonKeyFrames", "True");
      tq.set("TranscodeReasons", burn >= 0 ? "SubtitleCodecNotSupported" : "AudioCodecNotSupported,VideoCodecNotSupported");
      source.TranscodingUrl = `/videos/${movie.id}/master.m3u8?${tq.toString().replace(/%2C/g, ",")}`;
      source.TranscodingSubProtocol = "hls";
      source.TranscodingContainer = "ts";
      if (!profile.TranscodingProfiles?.length) return fail(400, "no TranscodingProfiles");
    }
    sessions.set(playSessionId, { item: movie.id, transcodeStarted: false });
    return send(res, 200, JSON.stringify({ MediaSources: [source], PlaySessionId: playSessionId }));
  }
  if ((m = /^\/[Vv]ideos\/([0-9a-f]{32})\/stream$/.exec(path))) {
    if (strict && q.get("static") !== "true") return fail(400, "static required");
    if (clipFile) {
      const file = readFileSync(clipFile);
      const range = /^bytes=(\d*)-(\d*)$/.exec(String(req.headers["range"] ?? ""));
      if (range) {
        const from = range[1] ? Number(range[1]) : 0, to = range[2] ? Math.min(Number(range[2]), file.length - 1) : file.length - 1;
        res.writeHead(206, { "Content-Type": "video/mp4", "Content-Range": `bytes ${from}-${to}/${file.length}`, "Content-Length": to - from + 1, "Accept-Ranges": "bytes" });
        res.end(file.subarray(from, to + 1));
      } else {
        res.writeHead(200, { "Content-Type": "video/mp4", "Content-Length": file.length, "Accept-Ranges": "bytes" });
        res.end(file);
      }
      return;
    }
    return send(res, 200, Buffer.alloc(2048, 0x47), "video/x-matroska");
  }
  if ((m = /^\/[Vv]ideos\/([0-9a-f]{32})\/(master|main)\.m3u8$/.exec(path))) {
    if (hlsStatus && m[2] === "master") return send(res, hlsStatus, hlsBody, "text/plain");
    if (strict) {
      if (!q.get("MediaSourceId")) return fail(400, "The mediaSourceId field is required.");
      if (!q.get("DeviceId")) return fail(400, "DeviceId is required for a transcode.");
      if (!q.get("PlaySessionId")) return fail(500, "PlaySessionId missing (a real server would fail in StreamingHelpers)");
      if (!/^[a-z0-9]*$/i.test(q.get("SegmentContainer") ?? "ts")) return fail(400, "SegmentContainer invalid");
    }
    const sub = new URLSearchParams(q);
    const query = [...sub.entries()].map(([k, v]) => `${encodeURIComponent(k)}=${encodeURIComponent(v).replace(/%2C/g, ",")}`).join("&");
    if (m[2] === "master") {
      const text = `#EXTM3U\n#EXT-X-STREAM-INF:PROGRAM-ID=1,BANDWIDTH=8000000,CODECS="avc1.640028,mp4a.40.2",RESOLUTION=1920x1080,FRAME-RATE=23.976\nmain.m3u8?${query}\n`;
      return send(res, 200, text, "application/x-mpegURL");
    }
    const text = `#EXTM3U\n#EXT-X-VERSION:3\n#EXT-X-TARGETDURATION:6\n#EXT-X-MEDIA-SEQUENCE:0\n#EXT-X-PLAYLIST-TYPE:VOD\n#EXTINF:6.000000, nodesc\nhls1/main/0.ts?runtimeTicks=0&actualSegmentLengthTicks=60000000\n#EXTINF:6.000000, nodesc\nhls1/main/1.ts?runtimeTicks=60000000&actualSegmentLengthTicks=60000000\n#EXT-X-ENDLIST\n`;
    sessions.get(q.get("PlaySessionId") ?? "")!.transcodeStarted = true;
    return send(res, 200, text, "application/x-mpegURL");
  }
  if (/^\/[Vv]ideos\/[0-9a-f]{32}\/hls1\/main\/\d+\.ts$/.test(path)) return send(res, 200, Buffer.alloc(1880, 0x47), "video/mp2t");
  if ((m = /^\/Videos\/([0-9a-f]{32})\/([0-9a-f]{32})\/Subtitles\/(\d+)\/Stream\.(srt|sup)$/.exec(path))) {
    const delay = Number(process.env.STANDIN_SUBTITLE_DELAY_MS ?? 0); // a real server extracts every stream first
    setTimeout(() => send(res, 200, m![4] === "srt" ? "1\n00:00:01,000 --> 00:00:02,000\nHello\n" : Buffer.alloc(64, 0), "application/octet-stream"), delay);
    return;
  }
  if (req.method === "POST" && path.startsWith("/Sessions/Playing")) {
    // Playing = start, Playing/Progress, Playing/Stopped. Strict: the body must name the item, a PlaySessionId and
    // integer PositionTicks, as the real server's PlaybackStartInfo / PlaybackProgressInfo / PlaybackStopInfo do.
    let info: any = {};
    try { info = JSON.parse(body.toString("utf8")); } catch { return fail(400, "bad json"); }
    const kind = path === "/Sessions/Playing" ? "start" : path === "/Sessions/Playing/Progress" ? "progress" : path === "/Sessions/Playing/Stopped" ? "stopped" : "";
    if (!kind) return fail(404, "Not found: " + path);
    const target = everything.find((x) => x.id === info.ItemId) ?? tracks.find((x) => x.id === info.ItemId);
    if (strict && !target) return fail(400, "ItemId missing or unknown");
    if (strict && !sessions.has(String(info.PlaySessionId ?? ""))) return fail(400, "PlaySessionId is not a session this server opened");
    if (strict && !Number.isInteger(info.PositionTicks)) return fail(400, "PositionTicks must be an integer");
    reports.push({ kind, item: String(info.ItemId ?? ""), ticks: info.PositionTicks ?? null, paused: info.IsPaused ?? null, session: String(info.PlaySessionId ?? "") });
    if (target && kind !== "start") {
      const played = dataOf(target).played;
      const song = "album" in target && target.ticks < LONG_AUDIO_TICKS;   // the server keeps a position for long audio only (a stand-in assumption)
      if (kind === "stopped" && info.PositionTicks >= target.ticks * 0.9) setData(target.id, { ticks: 0, played: true }, "album" in target);   // MaxResumePct; a played audio item counts a play
      else if (info.PositionTicks > 0 && !song) setData(target.id, { ticks: info.PositionTicks, played: kind === "progress" ? played : false });
    }
    return send(res, 204, "");
  }
  if (path === "/Videos/ActiveEncodings" || path === "/Sessions/Logout") return send(res, 204, "");
  return fail(404, "Not found: " + path);
}).listen(port, "127.0.0.1", () => {
  if (logFile) writeFileSync(logFile, "");
  console.log(`jellyfin stand-in listening on http://127.0.0.1:${port} (${strict ? "strict" : "lenient"})`);
});
