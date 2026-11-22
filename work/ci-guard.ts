// CI guard: the unit-test wiring in native-xmb/CMakeLists.txt, tests/*.cpp and
// .github/workflows/tests.yml must agree, so a runner can never be added (or dropped) in one place and
// forgotten in the others. A missing piece only shows up in CI after a release tag otherwise.
//
//   bun work/ci-guard.ts            # exits 1 and names every mismatch
//
// It is a static check. To prove the configuration also BUILDS, run work/ci-config-build.ps1.
import { readFileSync, readdirSync } from "node:fs";
import { join, resolve } from "node:path";

const root = resolve(import.meta.dir, "..");
const cmake = readFileSync(join(root, "native-xmb", "CMakeLists.txt"), "utf8");
const yml = readFileSync(join(root, ".github", "workflows", "tests.yml"), "utf8");
const problems: string[] = [];

// Deliberately NOT in CI: needs a real interactive desktop (it opens fullscreen windows). Run by hand, see
// native-xmb/audit/INGAME-OVERLAY-DESIGN.md. The Steam Linux discovery tests are likewise out: they
// exercise the $HOME-based ~/.steam/steam and ~/.local/share/Steam paths that only the Linux port hosts,
// which the windows-latest CI runner never builds. Add a name here only with a reason.
const manualOptions = new Set(["XMB_OVERLAY_BUILD_TESTS", "XMB_STEAM_LINUX_BUILD_TESTS"]);
const manualTargets = new Set(["xmb-overlay-standin", "xmb-steam-linux-tests"]);

const options = [...new Set([...cmake.matchAll(/^\s*option\((XMB_[A-Z0-9_]+_BUILD_TESTS)\b/gm)].map((m) => m[1]))];
const enabled = new Set([...yml.matchAll(/-D(XMB_[A-Z0-9_]+_BUILD_TESTS)=ON/g)].map((m) => m[1]));
for (const o of options) if (!enabled.has(o) && !manualOptions.has(o)) problems.push(`option ${o} is declared in CMakeLists.txt but tests.yml never sets -D${o}=ON`);
for (const o of enabled) if (!options.includes(o)) problems.push(`tests.yml sets -D${o}=ON but CMakeLists.txt declares no such option`);

// Targets: every xmb-*-tests executable defined in CMake, plus the steam-quit stand-in the quit tests spawn.
const defined = new Set(
  [...cmake.matchAll(/(?:qt_)?add_executable\((xmb-[a-z0-9-]+)\b/g)].map((m) => m[1]).filter((n) => /-tests$|-standin$/.test(n)),
);
const targetBlock = yml.match(/--target `\r?\n([\s\S]*?)\r?\n\s*if \(\$LASTEXITCODE\)/);
const built = new Set((targetBlock ? targetBlock[1] : "").replace(/`/g, " ").split(/\s+/).filter((t) => t.startsWith("xmb-")));
if (!targetBlock) problems.push("could not find the '--target ...' build step in tests.yml");
for (const t of defined) if (!built.has(t) && !manualTargets.has(t)) problems.push(`target ${t} is defined in CMakeLists.txt but tests.yml does not build it`);
for (const t of built) if (!defined.has(t) && t !== "xmb-manager") problems.push(`tests.yml builds ${t} but CMakeLists.txt defines no such target`);

// Runs: every -tests target must be executed.
const ran = new Set([...yml.matchAll(/Exe = '(xmb-[a-z0-9-]+-tests)\.exe'/g)].map((m) => m[1]));
for (const t of defined) if (t.endsWith("-tests") && !ran.has(t) && !manualTargets.has(t)) problems.push(`target ${t} is built but never run by tests.yml`);
for (const t of ran) if (!defined.has(t)) problems.push(`tests.yml runs ${t}.exe but CMakeLists.txt defines no such target`);

// Sources: every tests/*_tests.cpp must be referenced by a target.
for (const f of readdirSync(join(root, "native-xmb", "tests")).filter((n) => /_tests\.cpp$/.test(n)))
  if (!cmake.includes(`tests/${f}`)) problems.push(`tests/${f} exists but no CMake target compiles it`);

// Seerr->Jellyfin production wiring: the Shell's call into the resolver must hand it the request's TMDB id for
// BOTH media types. Stage 1's TV Series identity depends on the id reaching resolveSeerrSeries(); a call that
// passes the id only for movies (a `type==movie ? QString::number(id) : QString()` conditional) leaves the TV id
// empty, so a same-title wrong Series could match on the title fallback. This is a source-level guard because the
// wiring lives in Shell::refreshSeerrDownloads, which no headless test instantiates.
const seerrShell = readFileSync(join(root, "native-xmb", "src", "seerr_shell.cpp"), "utf8");
if (!seerrShell.includes("resolveSeerrJellyfin(identifier,title,type,QString::number(id),is4k,seasons)"))
  problems.push("seerr_shell.cpp must pass the request's TMDB id (QString::number(id)) to resolveSeerrJellyfin for both movie and TV");
if (/type==QLatin1String\("movie"\)\?QString::number\(id\):QString\(\)/.test(seerrShell))
  problems.push("seerr_shell.cpp drops the TMDB id for TV in the resolveSeerrJellyfin call (Stage 1 regression)");

if (problems.length) {
  console.error(`ci-guard: ${problems.length} mismatch(es)`);
  for (const p of problems) console.error(`  - ${p}`);
  process.exit(1);
}
console.log(`ci-guard: ok (${options.length} options, ${defined.size} targets, ${ran.size} runners)`);
