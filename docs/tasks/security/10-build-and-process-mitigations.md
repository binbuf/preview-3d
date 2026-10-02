---
verify: x64\Release\Tests.ImportIsolation.exe
---

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
- [x] Move CFG (`<ControlFlowGuard>Guard</ControlFlowGuard>`), CET (`<CETCompat>true</CETCompat>`),
      `/Qspectre`, and `/guard:ehcont` (where the toolset supports it) into `Directory.Build.props`
      so viewer, worker, both hosts, and `Preview3DOpenUsdCore` get them; keep provider settings
      consistent and handle the `/ZI`-Debug CFG interaction the provider already documents.
- [x] Add a `PROC_THREAD_ATTRIBUTE_MITIGATION_POLICY` entry in `SandboxLauncher` for the import
      children: block non-Microsoft signed images and extension points where compatible, enable
      CFG-always-on if it does not break the pinned libraries, and document any exclusion with its
      measured reason.
- [x] Add `SetProcessMitigationPolicy` (signature/dynamic-code/extension-point as applicable) and
      heap-termination-on-corruption to each EXE at startup.
- [x] Add an application manifest for the viewer (and children as needed) with `longPathAware`,
      `activeCodePage UTF-8`, and explicit `supportedOS` entries.
- [x] Verify by inspecting the final link logs/manifests (`GUARD:CF`, CET present, mitigation policy
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
- [x] Debug and Release builds succeed for all product projects.
- [x] Link logs/manifests show the flags, and a test asserts the child mitigation policy.
- [x] Hand-off filled in, listing any per-process exclusion and its justification.

## Hand-off
Landed:
- `Directory.Build.props`: `ControlFlowGuard=Guard` and `CETCompat=true` for every project (the
  provider's local settings are now redundant but unchanged); Debug pins
  `DebugInformationFormat=ProgramDatabase` so `/guard:cf` cannot collide with `/ZI`.
  `GuardEHContMetadata` is wired behind opt-in `Preview3DEnableGuardEhCont` (default false).
- `Directory.Build.targets`: `/Qspectre` is enabled only when the optional MSVC Spectre-mitigated
  libraries component exists; `Preview3DSpectreMitigationEnabled` records the outcome.
- `shared/import-broker/src/SandboxLauncher.cpp`: `PROC_THREAD_ATTRIBUTE_MITIGATION_POLICY` (CFG
  always-on, extension-point disable, ACG) on every child; failure to set it fails the launch closed.
- `shared/platform/ProcessMitigations.h`: heap-termination-on-corruption + extension-point disable
  for every EXE, plus ACG for the worker and both import hosts. Called from all four `main`s.
- `shared/platform/app.manifest`: `longPathAware`, `activeCodePage UTF-8`, Win10/11 `supportedOS`,
  merged with the linker manifest (verified segment heap + trustInfo survive).
- `SandboxLaunchTests`: new `[sandbox][security]` case reads the creation policy off a still-suspended
  worker (extension points / CFG / ACG).
- `docs/design/09-quality-performance-and-security.md` and `docs/design/adr/0039-*.md` updated.

Deviations / measured exclusions:
- **`/guard:ehcont` off.** Every product image (viewer included) statically imports vcpkg libraries
  built without EH continuation metadata; linking fails `LNK1386`/`LNK2047` (measured on the worker,
  viewer, and provider). Left as an opt-in switch that will work once the ports are rebuilt with it.
- **`/Qspectre` not active here.** The Spectre-mitigated libraries component is not installed on this
  image, so enabling it would fail MSB8040. The detection is in place and will turn it on when the
  component is present.
- **Signature policy (MicrosoftSignedOnly) deferred.** The images and app-local DLLs are unsigned
  until SEC-14; enforcing it now would make them unloadable. The extension-point, ACG, and CFG
  policies are applied.
- **Viewer ACG withheld.** Viewer mitigations run with `prohibitDynamicCode=false` until a GPU run
  validates the user-mode D3D runtime; the untrusted-data children enable ACG.
- **Runtime CFG policy call removed.** `SetProcessMitigationPolicy(ProcessControlFlowGuardPolicy)`
  returns ERROR_ACCESS_DENIED (measured, err=5) on an already-CFG image, so CFG stays enforced by the
  PE header, the linker, and the broker attribute.
- `OpenUsdHostSpikeTests.cpp`: replaced the fixed `Sleep(150)` "started" check with a bounded
  `WaitForState` poll, because the added creation mitigations/manifest measurably lengthen host
  startup and made the sleep flaky.

Checks (MSBuild: `C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe`):
- All product projects (`ModelCore`, worker, `OpenUsdCore`, import host, STEP host, provider, viewer)
  build Debug and Release with `/m` and `"/p:SolutionDir=D:\repos\binbuf\preview-3d\\"`.
- `dumpbin /headers` + `/loadconfig`: Guard CF, CET compatible, DYNAMICBASE, NXCOMPAT, high-entropy VA
  on all five product images; `mt.exe` extraction shows longPathAware/activeCodePage/supportedOS/
  SegmentHeap on the viewer and worker manifests.
- `Tests.ImportIsolation` Release 399 cases / 0 failed / 5 skipped; Debug 399 / 0 / 1 (was 2
  pre-existing failures in the T09 baseline). `npm test` (Tests.Unit Release) 357 cases green.

Next task must know:
- SEC-14 can enable MicrosoftSignedOnly in `SandboxLauncher` and `ApplyProcessMitigations` once the
  payload is signed.
- Rebuilding the vcpkg ports with `/guard:ehcont` (and installing the Spectre libs) is what unblocks
  those two flags; no code change is needed beyond flipping `Preview3DEnableGuardEhCont`.
- A GPU validation run should confirm whether the viewer can also take ACG.