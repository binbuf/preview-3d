# T52 — Complete release acceptance and support documentation

## Goal
Close the program honestly: run every retained gate against the candidate build, record which passed,
update the public format/support documentation to state thumbnails are installed, and hand over the
final evidence and any remaining limitations.

## Context (read first)
- `docs/design/05-thumbnail-provider.md` — provider scope and limits.
- `docs/design/10-delivery-plan.md` — Gate 6/7 exit criteria.
- `docs/design/09-quality-performance-and-security.md` — retained quality gates.
- `docs/design/adr/0001`–`0007` — the decisions that constrain what may be claimed.
- `docs/FORMAT-SUPPORT.md`, root `README.md` — active public text. The historical baseline remains in `docs/legacy/FORMAT-SUPPORT.md` and must not be edited.

## Scope
- [ ] Build clean Debug/Release x64 through `Preview3D.slnx`. Run `Tests.Unit.exe`, `Tests.ProviderHost.exe`, the provider/COM/golden/surrogate suites and per-family fuzz smokes in both configurations. Run `Tests.ImportIsolation.exe "[stl-import],[ply-import],[gltf-import]"` in both configurations as the baseline regression gate, plus any new provider-relevant import filters. Capture a full `Tests.ImportIsolation.exe` run separately and classify every failure against the documented pre-existing USD protocol/spike and generated hidden large-scan cases; any new failure blocks acceptance.
- [ ] Re-run the retained Gate 6 exit checks (routing per CLSID, measured time/process-commit targets, accounted scratch cap, no sidecar access, surrogate isolation, soak, `DllCanUnloadNow`, GDI ownership) against the candidate.
- [ ] Update `docs/FORMAT-SUPPORT.md`, root `README.md` and support/limitations text to state Explorer thumbnails for all eight families only after T34/T44/T51 pass, with per-family exclusions made explicit (e.g. external sidecar/composition files fall back to the icon).
- [ ] Record the final evidence — commands, results, binary/fixture hashes, machine used — and list every unverified or deliberately deferred item without marking it passed.
- [ ] Note the eventual MSI/Gate 7 adoption of the same CLSID identities and rules.

## Out of scope
- Viewer Gate 3/4 qualification still open elsewhere; this report does not claim it.
- Any new feature, format or scope not already accepted by the ADRs.

## Design notes
- A failed retained gate stays blocking unless deliberately changed in scope via an ADR with revised acceptance; compilation is not completion.
- The known unrelated full import-isolation failures are tracked by the viewer program, not silently treated as passes. Record their exact test names and baseline evidence; an expanded failure set is a regression.
- The current release candidate is NSIS-installed. Report NSIS same-version rerun/upgrade/uninstall evidence separately from future WiX/MSI Gate 7 work.
- Public text must describe the supported static subsets, not general format/authoring fidelity.
- Keep deferred items in the risk register with an owner and trigger; do not drop them.

## Done when
- [ ] The acceptance report lists every retained check as passed, failed or explicitly deferred with evidence.
- [ ] Public documentation accurately states thumbnail availability and limits.
- [ ] Final artifact hashes are recorded.
- [ ] Hand-off below filled in.

## Hand-off
_(filled in by the implementing session: what landed, what deviated and why, what the next task must know)_
