# 0004 — The provider shares model-core source, not process or state

## Status
accepted

## Context
`02-system-architecture.md` requires the thumbnail DLL to link "its own copy of the bounded fast-path
parsers" and to "share no process, memory, or cache with the viewer or any import process", while
`05-thumbnail-provider.md` says it "shares validation, normalized math, material fallbacks, and
bounded parsers with model-core". In practice `shared/model-core/ModelCore.vcxproj` is a stub and the
shared code is compiled per-consumer through relative-path `<ClCompile>` includes.

## Decision
The provider compiles the same **source files** it needs from `shared/model-core` (and, where
applicable, the bounded product parsers currently in `import-worker`) directly into
`Preview3DThumbnailProvider.dll`. It links no product executable, opens no IPC channel, maps no
shared section, and reads no cache. Third-party parser types never cross a module boundary: adapters
emit product-owned values only. If sharing source becomes fragile, a small dedicated static library
target may be introduced, but the provider still embeds its own copy.

## Consequences
- T04 freezes the exact shared source subset; T07 performs the extraction and proves the provider
  compiles it with no viewer/worker-only dependency dragged in.
- Duplicated parser code in the DLL is expected and auditable; the packaging allowlist (T42) records
  exactly which binaries enter Shell isolation.
- Any future "shared library" refactor must preserve the no-shared-state invariant and is itself an
  ADR-worthy change.
- T07 realized the subset as `shared/parser-core/`: the format-agnostic STL/PLY primitives and the
  ASCII tokenizer move there and compile into both consumers, while the worker's wire-emitting
  adapter shells stay in `import-worker/` (see [ADR-0012](0012-provider-parser-core-extraction.md)).