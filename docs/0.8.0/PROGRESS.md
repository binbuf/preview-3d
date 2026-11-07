# 0.8.0 progress notes

Shared notebook for the `0.8.0` task set. Earlier sessions record findings,
decisions, and things later tasks must know here; the harness maintains the
"Key facts" digest at the top.

The plan, ground rules, and per-task verify commands are in
[`../tasks/0.8.0/README.md`](../tasks/0.8.0/README.md); execution order is in
[`ROADMAP.md`](ROADMAP.md).

<!-- symphony:digest:start -->
## Key facts (maintained by symphony — do not edit)

- **T01 — Valid-file coverage baseline and failure triage**: `tests/import-isolation/CompatManifest.{h,cpp}` — shared, dependency-free; `tests/compat/valid-file-manifest.json` — schema-1 valid-file scoreboard.
- **T02 — Extension aliases and catalog/installer drift**: `catalog_detail::kStepExtensions` = `step`, `stp`, `stpnc`, `p21`, `210`.; NSIS install/uninstall lists, `FamilyRouting.h`, and the staged

## Task notes (maintained by symphony — do not edit)

- [T01](progress/T01.md) — `tests/import-isolation/CompatManifest.{h,cpp}` — shared, dependency-free
- [T02](progress/T02.md) — `catalog_detail::kStepExtensions` = `step`, `stp`, `stpnc`, `p21`, `210`.
<!-- symphony:digest:end -->
