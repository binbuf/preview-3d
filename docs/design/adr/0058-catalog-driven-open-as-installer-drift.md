# 0058 — Catalog-driven Open As, filters and installer drift check

## Status

Accepted (T03, 0.7.0).

## Context

T02 introduced `model_core::FormatCatalog` and a bounded content sniffer, but the
viewer still decided an open from a hand-written extension switch. The open
dialog, single-instance activation, Shell Open With discovery and the NSIS
installer each carried a separate copy of the same extension list. A valid model
with a missing or wrong suffix was rejected before any parser ran, and adding a
format meant editing four lists that could silently drift.

## Decision

- `model_core::DecideOpenFormat` combines the extension and content candidates.
  A known suffix keeps the existing extension-only open path. Content-only
  evidence and cross-family disagreements return `AskUser`; they never open
  silently. The failure card's **Open As** action lists the detected and suffix
  candidates first, then every importable catalog format with its stable name.
- `--format <stable-name>` and Open As force a catalog importer. The forced
  choice only replaces candidate selection; the worker/host still performs full
  validation, budgets, path policy and wire validation. `--skip-content-check`
  produces an extension-only classification. `--list-formats` emits one
  `stable-name<TAB>extensions<TAB>display-name` line per registered catalog
  entry.
- The open-dialog filter, `ActiveInstance` acceptance, `ShellIntegration`'s
  extension enumeration and the registration ProgID map are derived from the
  catalog. The NSIS calls stay explicit (no new build tool), but the
  `[format-installer]` unit test reads `packaging/installer/Preview3D.nsi` and
  asserts every catalog extension has the matching install and uninstall macro
  and that no extra extension is registered.
- Reserved catalog identities without a shipped importer are rejected by
  `--format`/Open As until their adapter task lands.

## Consequences

Adding a format now requires its descriptor plus an explicit NSIS entry; the
test fails if either side is missing, or if the family ProgID disagrees.
Open As is a user override, not a sniffing fallback: a weak or conflicting
candidate produces a typed choice instead of a silent import. The single-instance
forwarding path still accepts only catalog extensions (unchanged for existing
formats), so a forced format forwarded to an already-running instance is a
follow-up, not a claim of this task.
