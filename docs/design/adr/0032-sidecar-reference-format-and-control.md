# 0032 — Sidecar reference control-character rejection and per-format allowlist

## Status
accepted

## Context
The trusted process is the only component with path authority: the sandboxed worker asks for a
dependency by relative reference and `import_broker::ResolveSidecarPath` opens it. Two gaps let a
hostile reference do more than intended:

- `ResolveSidecarPath` decoded the worker's bytes with `MultiByteToWideChar(CP_UTF8, 0, ...)`, which
  never fails (invalid sequences become U+FFFD) and passes embedded NULs through. It validated
  `path.extension()` on the untruncated text, but `CreateFileW(candidate.c_str())` stops at the
  first NUL. A reference like `secret.pdf\0.png` therefore looked like a `.png`, passed the global
  allowlist, and opened `secret.pdf`.
- One global extension list (`.bin`, `.mtl`, images, USD layers) applied to every requesting format.
  A glTF could ask for an `.mtl` or a USD layer, even though it needs neither.

## Decision
- **Reject control characters, do not trim.** Any byte `< 0x20` or `== 0x7F` in the reference is
  rejected, checked on the raw UTF-8 bytes and again on the decoded wide text. Trimming would
  silently change which file is opened.
- **Make the UTF-8 decode failure explicit.** The decode passes `MB_ERR_INVALID_CHARS` and returns
  `std::optional<std::wstring>`; invalid UTF-8 is rejected with its own diagnostic rather than
  substituted or conflated with an empty reference.
- **Two layers.** `ServiceSidecarRequest` rejects control bytes before any path work; the resolver
  repeats both the control and UTF-8 checks so it remains the authority if a future caller bypasses
  the servicer.
- **Per-format allowlist.** `ResolveSidecarPath` and `ServiceSidecarRequest` take the trusted host's
  `ImportFormat` (already in `ImportSessionRequest`) and allow exactly what each format needs:
  glTF `.bin` + images; OBJ `.mtl` + images; FBX images; USD `.usd`/`.usda`/`.usdc` layers + images;
  STL/PLY/3MF/STEP nothing. Direct image assets are dependencies, not composition. The worker never
  selects the format, so **no control-protocol version change is needed**; overloading `reserved`
  was rejected for that reason.
- Corpus seeds `sidecar-nul-extension.gltf` and `sidecar-nul-trailing.gltf` pin the NUL cases
  end-to-end.

## Consequences
- A `.pdf`, or any other disallowed extension, cannot be opened through an embedded NUL regardless
  of what follows the NUL.
- A format that references nothing (STL/PLY/3MF/STEP) now fails sidecar requests that the old global
  list would have accepted; 3MF and USDZ package entries remain in-archive and never reach the
  broker.
- Later format work that brokers a new dependency type must extend `IsAllowedSidecarExtension` for
  that format; the extension list is the single policy point and is exercised by cross-format
  resolver tests.