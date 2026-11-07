# 0.8.0 task set

This is the `0.8.0` task set — the valid-file coverage plan. It makes the
formats Preview 3D already claims to support open the valid files users
actually have, instead of only the happy-path subset each importer was built
against. It runs alongside the base/MVP, `security`, and `0.7.0` sets without
touching them.

The plan starts with a `[valid-file]` scoreboard, closes extension aliases and
catalog-driven content detection, adds Assimp point/line and CAD edge/wire
geometry, exposes per-format import options and reader selection, then adds a
real-world per-format corpus and a final support-matrix qualification.

```powershell
./.symphony/symphony.ps1 doctor --set 0.8.0
./.symphony/symphony.ps1 run    --set 0.8.0
./.symphony/symphony.ps1 status --set 0.8.0
```

The set is registered in `.symphony/symphony.config.json` as:

```json
{
  "name": "0.8.0",
  "docs": "docs/0.8.0",
  "tasks": "docs/tasks/0.8.0",
  "design": "docs/design",
  "logs": "docs/0.8.0/logs"
}
```

`.symphony/` is gitignored in this repository, so that config entry is local to
the machine. Add the same entry if you run this set from another checkout.

- Roadmap: [`ROADMAP.md`](ROADMAP.md) (execution order, grouped by phase).
- Task files: [`../tasks/0.8.0/`](../tasks/0.8.0/).
- Plan ground rules, phase map and verify commands: [`../tasks/0.8.0/README.md`](../tasks/0.8.0/README.md).
- Shared design docs: [`../design/`](../design/).
- Progress notebook: [`PROGRESS.md`](PROGRESS.md).

Task ids are per-set, so `T01` here is unrelated to `T01` in the MVP, security,
or `0.7.0` sets. Every task carries a `verify:` front matter command, so the
harness runs that exact scoped check instead of the inferred root `npm test`.

The first task deliberately measures only. Do not flip a valid-file case from
`failed` to `opened` until the owning importer task lands.
