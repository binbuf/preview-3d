# T10 — Build and process mitigation hardening

## Goal
Every shipped image, especially the ones that parse untrusted data, is built and launched with the
full documented mitigation set: CFG, CET, Spectre mitigations, EH continuation, ACG/CIG where
compatible, and heap-termination hardening.

## Context (read first)
- `Directory.Build.props:79-106` — sets `/sdl`, `/W4`, `/WX`, segment heap; no `ControlFlowGuard`,
  `CETCompat`, `/Qspectre`, or `/guard:ehcont`. Only the thumbnail provider sets CFG/CET
  (`thumbnail-provider/Preview3DThumbnailProvider.vcxproj:67,74,98,105`).
- `shared/import-broker/src/SandboxLauncher.cpp:49-62` — only `SECURITY_CAPABILITIES` and
  `HANDLE_LIST` attributes; no `PROC_THREAD_ATTRIBUTE_MITIGATION_POLICY`. No
  `SetProcessMitigationPolicy` anywhere in the repo.
- `docs/design/09-quality-performance-and-security.md:215` claims DEP/NX, ASLR, CFG, CET, SDL, and
  stack protection for all binaries.
- Installer has no application manifest (`longPathAware`, `activeCodePage`); segment heap is the
  only manifest mitigation.

## Scope
- [ ] Move CFG (`<ControlFlowGuard>Guard</ControlFlowGuard>`), CET (`<CETCompat>true</CETCompat>`),
      `/Qspectre`, and `/guard:ehcont` (where the toolset supports it) into `Directory.Build.props`
      so viewer, worker, both hosts, and `Preview3DOpenUsdCore` get them; keep provider settings
      consistent and handle the `/ZI`-Debug CFG interaction the provider already documents.
- [ ] Add a `PROC_THREAD_ATTRIBUTE_MITIGATION_POLICY` entry in `SandboxLauncher` for the import
      children: block non-Microsoft signed images and extension points where compatible, enable
      CFG-always-on if it does not break the pinned libraries, and document any exclusion with its
      measured reason.
- [ ] Add `SetProcessMitigationPolicy` (signature/dynamic-code/extension-point as applicable) and
      heap-termination-on-corruption to each EXE at startup.
- [ ] Add an application manifest for the viewer (and children as needed) with `longPathAware`,
      `activeCodePage UTF-8`, and explicit `supportedOS` entries.
- [ ] Verify by inspecting the final link logs/manifests (`GUARD:CF`, CET present, mitigation policy
      applied) and add a `SandboxLaunchTests` assertion that the mitigation policy attribute is set.

## Out of scope
- Code signing (→ SEC-14).
- Removing `/WX` or changing warning policy.

## Design notes
- ACG (dynamic code prohibited) is incompatible with some JIT/codegen paths; validate against all
  pinned parsers and record per-process exceptions rather than dropping the policy globally.
- Mitigation-policy failures must fail the sandbox launch closed.
- Keep Debug builds usable: if a mitigation conflicts with `/ZI` or sanitizer builds, scope it to
  Release via the shared props rather than removing it.

## Done when
- [ ] Debug and Release builds succeed for all product projects.
- [ ] Link logs/manifests show the flags, and a test asserts the child mitigation policy.
- [ ] Hand-off filled in, listing any per-process exclusion and its justification.

## Hand-off
_(filled in by the implementing session: what landed, what deviated and why, what the next task must know)_