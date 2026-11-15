// Local stand-in for a Jellyfin 10.10+/12 server, for scripted XMB runs (audit/JELLYFIN-PLAYBACK.md).
//
//   bun work/jellyfin-standin.ts [--port 8099] [--log <file>] [--mode strict|lenient] [--hls-status <n>]
//
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
import { appendFileSync, writeFileSync } from "node:fs";

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
const noMusic = args.includes("--no-music");                 // the server has no music library (the Music column says so)
const secondLibrary = args.includes("--second-library");       // a second music library (the Music page lists the libraries first)
const musicTranscode = args.includes("--music-transcode");     // audio is served through a Transcode answer (/Audio/{id}/universal)
const extraSongs = Number(opt("extra-songs", "0"));            // filler songs, to page through a large library
const musicStatus = Number(opt("music-status", "0"));          // force every music list to answer this status (error text test)

const TOKEN = "standin-" + Math.random().toString(16).slice(2) + Math.random().toString(16).slice(2);
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

function itemJson(m: Movie) {
  return {
    Id: m.id, Name: m.name, Type: "Movie", IsFolder: false, MediaType: "Video", RunTimeTicks: m.ticks,
    UserData: { Played: false, PlaybackPositionTicks: 0 },
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
const albumJson = (a: Album) => ({ Id: a.id, Name: a.name, SortName: a.name.toLowerCase(), Type: "MusicAlbum", IsFolder: true, AlbumArtist: a.artist.name, ProductionYear: a.year, ImageTags: { Primary: imageTag(a.id) } });
const trackJson = (t: Track) => ({
  Id: t.id, Name: t.name, Type: "Audio", MediaType: "Audio", IsFolder: false, RunTimeTicks: t.ticks, IndexNumber: t.no, ParentIndexNumber: 1,
  Artists: [t.artist.name], AlbumArtist: t.artist.name, Album: t.album.name, AlbumId: t.album.id, AlbumPrimaryImageTag: imageTag(t.album.id), ProductionYear: t.album.year,
  UserData: { Played: false, PlaybackPositionTicks: 0 }, ImageTags: {},
});
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
      const list = album ? tracks.filter((t) => t.album === album) : [...tracks].sort(byName);
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
    return send(res, 200, JSON.stringify({ Items: movies.map(itemJson), TotalRecordCount: movies.length, StartIndex: 0 }));
  }
  let m: RegExpExecArray | null;
  if ((m = /^\/Items\/([0-9a-f]{32})\/Images\/(Primary|Backdrop)$/.exec(path))) {
    const movie = movies.find((x) => x.id === m![1]);
    if (!movie || (m[2] === "Backdrop" && !movie.backdrop)) return fail(404, "no image");
    // A short artificial delay for backdrops, longer for the first item, so a stale completion can arrive late.
    const delay = m[2] === "Backdrop" ? (movie === movies[0] ? 700 : 150) : 0;
    const img = m[2] === "Backdrop" ? png(640, 360, movie.color) : png(240, 360, movie.color);
    setTimeout(() => send(res, 200, img, "image/png"), delay);
    return;
  }
  if (req.method === "POST" && (m = /^\/Items\/([0-9a-f]{32})\/PlaybackInfo$/.exec(path))) {
    const movie = movies.find((x) => x.id === m![1]);
    if (!movie) return fail(404, "no item");
    let info: any = {};
    try { info = JSON.parse(body.toString("utf8")); } catch { return fail(400, "bad json"); }
    const profile = info.DeviceProfile ?? {};
    const burn = typeof info.SubtitleStreamIndex === "number" ? info.SubtitleStreamIndex : -1;
    const deviceId = String(info.DeviceId ?? "");
    if (strict && !deviceId) return fail(400, "DeviceId missing");
    const playSessionId = "ps" + Math.random().toString(16).slice(2, 18);
    const direct = movie.method === "direct" && burn < 0 && info.EnableDirectPlay !== false;
    const streams = [
      { Index: 0, Type: "Video", Codec: "hevc", DisplayTitle: "1080p HEVC" },
      { Index: 1, Type: "Audio", Codec: "dts", Language: "eng", Channels: 6, DisplayTitle: "English DTS-HD MA 5.1", IsDefault: true },
      { Index: 3, Type: "Subtitle", Codec: "pgssub", Language: "eng", DisplayTitle: "English PGS", IsTextSubtitleStream: false },
    ];
    const source: any = { Id: movie.id, Protocol: "File", Container: "mkv", MediaStreams: streams, SupportsDirectPlay: direct, SupportsDirectStream: false, SupportsTranscoding: true };
    if (!direct) {
      // The shape 10.9+ builds in StreamInfo.ToUrl: relative path, query beginning with DeviceId, ApiKey appended.
      const tq = new URLSearchParams();
      tq.set("DeviceId", deviceId);
      tq.set("MediaSourceId", movie.id);
      tq.set("VideoCodec", "h264");
      tq.set("AudioCodec", "aac");
      tq.set("AudioStreamIndex", "1");
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
  if (req.method === "POST" && path.startsWith("/Sessions/Playing")) return send(res, 204, "");
  if (path === "/Videos/ActiveEncodings" || path === "/Sessions/Logout") return send(res, 204, "");
  return fail(404, "Not found: " + path);
}).listen(port, "127.0.0.1", () => {
  if (logFile) writeFileSync(logFile, "");
  console.log(`jellyfin stand-in listening on http://127.0.0.1:${port} (${strict ? "strict" : "lenient"})`);
});
