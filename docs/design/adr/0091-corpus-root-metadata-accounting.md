# ADR-0091 — Corpus root metadata is not corpus content

- **Status:** Accepted
- **Date:** 2026-11-01
- **Context:** T34b — finish T34 acceptance

## Context

The render-quality external lane inventories every entry in the restricted
local corpus at `D:/repos/binbuf/preview-3d-test-models`. Windows writes
`desktop.ini` at that root, and other clients write `Thumbs.db`/`ehthumbs.db`;
a VCS checkout adds `.git`. T34's inventory excluded only leading-dot names, so
`desktop.ini` became a corpus asset. The full-corpus audit then reported
`unaccounted corpus root entries: desktop.ini` and failed the gate even though
every model was captured or had a typed disposition.

A per-profile disposition would make each new profile responsible for OS
metadata, and an extension filter would miss dotless metadata files. Both
would let the inventory and the audit drift.

## Decision

1. `tests/render-quality/corpus.py` owns one predicate,
   `is_corpus_entry(name)`. It excludes leading-dot entries (`.git`,
   `.DS_Store`) and a fixed set of OS directory metadata names: `desktop.ini`,
   `Thumbs.db`, `ehthumbs.db`, `ehthumbs_vista.db`, `Thumbs.db:encryptable`,
   `$RECYCLE.BIN` and `System Volume Information`.
2. `corpus.inventory()` and `full_corpus._external_audit()` both call the
   predicate. `rootEntryCount` is derived from the filtered set, so the audit
   cannot count metadata the inventory withheld.
3. The lane proves the rule with a focused temporary root containing `.git`,
   metadata files and one real `Cube.STL`: the real entry is counted, unaccounted
   entries are empty, and an audit profile that omits `Cube.STL` still flags it.
4. This is an accounting rule only. It adds no per-asset-name exceptions, does
   not change `maxEntryBytes`, and does not relax any parser, budget or render
   threshold. The local corpus remains restricted, uncommitted evidence.

## Consequences

- Every render-quality profile can add corpus assets without re-declaring OS
  metadata, and the inventory and audit stay in agreement.
- Adding a new OS-generated metadata name is a one-line change to the shared
  set, reviewed as a corpus-accounting change rather than an asset disposition.
- A genuine model file named `desktop.ini` would be ignored; that is acceptable
  for this test corpus because such a name has no supported model extension.
- The focused checks run even in `--fast` iteration, so the rule is exercised
  before the expensive GPU lanes.
