# 0.7.0 task set

This is the `0.7.0` task set — the format-breadth and Studio-lighting plan derived from
[`../proposal.md`](../proposal.md). It runs alongside the base/MVP and `security` sets without
touching either.

```powershell
./.symphony/symphony.ps1 doctor --set 0.7.0
./.symphony/symphony.ps1 run    --set 0.7.0
./.symphony/symphony.ps1 status --set 0.7.0
```

- Roadmap: [`ROADMAP.md`](ROADMAP.md) (execution order, grouped by phase).
- Task files: [`../tasks/0.7.0/`](../tasks/0.7.0/).
- Plan ground rules, phase map and verify commands: [`../tasks/0.7.0/README.md`](../tasks/0.7.0/README.md).
- Shared design docs: [`../design/`](../design/).
- Progress notebook: [`PROGRESS.md`](PROGRESS.md).
- Run logs: [`logs/`](logs/).

Task ids are per-set, so `T01` here is unrelated to `T01` in the MVP or security sets. Every
task carries a `verify:` front matter command, so the harness runs that exact scoped check
instead of the inferred root `npm test`.
