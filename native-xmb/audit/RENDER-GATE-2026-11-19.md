# Render gate verification — 2026-11-19

This note records the release-gate result for the follow-up commit containing
the mandatory mutation regression and its clean
candidate build. It contains commands and scalar results only; no captured
frames, firmware assets or machine-private data are stored here.

## Candidate provenance

- Source revision: `85e849c`
- Build metadata: `sourceDirty: false`
- Candidate executable: `out/native-xmb-followup/re-ps3-native.exe`
- Candidate SHA-256: `5E66EF34EFE71F50CEEA070809D8C1ED05CE8E5609B5F0A5887013E7863671AE`

## Full gate

Exact command, run from the repository root:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File work\release-check.ps1 -GateExe C:\Users\Albert\.local\share\opencode\worktree\5d5811\render-gate-determinism\out\native-xmb-followup\re-ps3-native.exe
```

Result:

```text
PASS  gate    exit 0; whole set: PASS
states: 28/28 pass
PASS  mutation guide, focused row and setup-step mutations rejected
RESULT PASS (7 passed, 0 failed, 0 skipped, 1 report-only)
```

The report-only parity count was 31 cells without references. It did not gate
the release.

## Deterministic-region review

The affected captures differed only in compositor/animation bands. The gate
keeps the rest of each frame and explicit key-surface regions:

- `ingame-xmb`: frame below the compositor title, game frame, and guide.
- `settings-autologon-row`: list above/below the focus-edge band, the stable
  left edge, and the focused row. The former overlapping right-edge band was
  removed because it was already covered by the focused-row region and was
  deterministic.
- `first-run-autologon-step`: upper/lower frame, the narrow stable compositor
  edge beside the animated strip, and step rows. The step-row region remains
  an explicit assertion of the wizard content.

The committed `work/ab-layers-mutation.ts` check changes pixels inside the
guide, focused row, and setup step rows and requires each configured metric to
fall below the 55 dB / 0.9995 floor. It uses a synthetic frame and resolves
paths from `import.meta.dir`, so it runs without private references and from
outside the repository cwd.

Observed mutation results:

```text
ingame-xmb: guide mutation rejected (25.28 dB / 0.9933)
settings-autologon-row: focused row mutation rejected (28.64 dB / 0.9969)
first-run-autologon-step: step rows mutation rejected (33.00 dB / 0.9989)
```

## CI checks

```text
ci-guard: ok (28 options, 31 targets, 27 runners)
ci-config-build: built 31 targets
ci-config-build: ok
```
