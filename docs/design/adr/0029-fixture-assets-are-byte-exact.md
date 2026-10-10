# 0029 — Fixture assets are byte-exact in the working tree

## Status
accepted

## Context
Several tests hash fixtures byte-for-byte and compare them to checked-in manifests:
`tests/unit/FixtureManifestTests.cpp` for `interactive-viewer/test-assets/corpus`, and
`tests/import-isolation/FixtureManifestTests.cpp` / `UsdProtocolTests.cpp` for the `.usda` files in
`tests/fixtures/usd-spike`. Those manifests are generated with LF (e.g. `tests/fixtures/generate.py`
writes via `write_bytes`). The repository had no `.gitattributes`, so on a Windows checkout with
`core.autocrlf=true` Git rewrote the text fixtures (`.gltf`, text `.ply`, `.usda`, `manifest.json`)
to CRLF while the manifests still hashed the committed LF bytes. The byte-exact tests therefore
failed on every such checkout, independent of any parser code — this was the failing assertion
behind the harness `npm test` (exit 42) verification failure and most of what had been misrecorded
as USD "fixture SHA drift".

## Decision
Pin byte-exact fixture trees against end-of-line conversion with
`.gitattributes`: `interactive-viewer/test-assets/** -text` and `tests/fixtures/** -text`. The assets
are data files, not source; Git must not normalize them. Rejected: regenerating fixtures at build
time (would erase the checked-in immutability guarantee and add a build dependency), and normalizing
CRLF in the tests (would weaken the integrity checks the tests exist to provide).

## Consequences
- `FixtureManifestTests` passes on any platform regardless of `core.autocrlf`.
- Regenerating the corpus with `generate.py` after checkout produces identical LF bytes, so
  `git status` stays clean (the generator writes LF and no conversion is applied).
- Later tasks must not remove this attribute, and any new byte-hashed asset tree should be covered
  by the same rule rather than relying on Git's text detection.