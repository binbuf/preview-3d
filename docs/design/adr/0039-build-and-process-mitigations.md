# 0039 — Shared build/process mitigations, broker mitigation policy, and the application manifest

## Status
accepted

## Context
`docs/design/09-quality-performance-and-security.md` promised DEP/NX, ASLR, CFG, CET, SDL, and stack
protection for all binaries, but only the thumbnail provider set CFG/CET; `Directory.Build.props`
had `/sdl`/`/W4`/`/WX` and segment heap only. `SandboxLauncher` passed no
`PROC_THREAD_ATTRIBUTE_MITIGATION_POLICY`, no process called `SetProcessMitigationPolicy`, and the
only manifest mitigation was the segment heap. Signature enforcement, EH continuation, and Spectre
mitigations were absent.

## Decision
- **CFG and CET are shared defaults.** `Directory.Build.props` sets `ControlFlowGuard=Guard` and
  `CETCompat=true`; Microsoft.CppCommon.targets derives linker `/GUARD:CF` from the compiler
  metadata. The provider keeps its local settings only as documentation. Debug states
  `DebugInformationFormat=ProgramDatabase` so `/guard:cf` can never collide with `/ZI`.
- **`/Qspectre` is conditional.** It is enabled only when the optional MSVC Spectre-mitigated
  libraries component is installed (`Directory.Build.targets`, because the VC_LibraryPath_* value is
  not known until Microsoft.Cpp.props). Setting it without the component fails MSB8040. The measured
  outcome is exposed as `Preview3DSpectreMitigationEnabled` (false on this build image).
- **`/guard:ehcont` is opt-in, off by default.** The pinned third-party static libraries (TinyUSDZ,
  ufbx, zip, zstd, draco, ktx, lib3mf, libwebp, meshoptimizer, simdjson, tbb, OpenUSD, OCCT) were
  built without EH continuation metadata; linking with `GuardEHContMetadata` fails LNK1386/LNK2047
  on the worker, viewer, and provider (measured). `Preview3DEnableGuardEhCont=true` turns it on once
  the ports are rebuilt with the flag.
- **The broker forces a mitigation policy on every import child at creation.**
  `LaunchSuspendedSandboxedWithSid` adds `PROC_THREAD_ATTRIBUTE_MITIGATION_POLICY` with CFG
  always-on, extension-point disable, and ACG; a failed `UpdateProcThreadAttribute` fails the launch
  closed. Microsoft-signed-image enforcement is withheld until the payload is Authenticode-signed
  (SEC-14): the images and app-local DLLs are unsigned today and the policy would make them
  unloadable.
- **Each product executable applies its startup mitigations.** `platform::ApplyProcessMitigations`
  (header-only, `shared/platform`) enables heap-termination-on-corruption and extension-point
  disable, and ACG for the untrusted-data parsers (`Preview3DImportWorker.exe`,
  `Preview3DImportHost.exe`, `Preview3DStepHost.exe`). The viewer withholds ACG until a GPU run
  validates the user-mode D3D runtime. Failure is fatal. `SetProcessMitigationPolicy` is deliberately
  not used for CFG: it returns ERROR_ACCESS_DENIED (measured, err=5) on an already-CFG image, whose
  flag is set by the PE header, link, and the broker attribute.
- **One shared application manifest.** `shared/platform/app.manifest` adds `longPathAware`,
  `activeCodePage UTF-8`, and an explicit Windows 10/11 `supportedOS` entry; `mt.exe` merges it with
  the linker manifest so the segment heap and UAC trustInfo survive.

## Consequences
- Product images are consistent: `dumpbin /headers` and `/loadconfig` show Guard CF, CET, DYNAMICBASE,
  NXCOMPAT, and high-entropy VA for the viewer, worker, both hosts, `Preview3DOpenUsdCore.dll`, and
  the provider; the embedded manifest carries longPathAware/activeCodePage/supportedOS/segment heap.
- `SandboxLaunchTests` asserts the creation mitigation policy on a still-suspended child (extension
  points, CFG, ACG) so the broker attribute is tested independently of child startup code.
- The OpenUSD spike's fixed 150 ms "started" sleep became a bounded poll: CFG-always-on, ACG, and the
  manifest measurably lengthen host startup.
- Known gaps, all deliberate and recorded: Spectre libs not installed; EHCont blocked on
  un-instrumented libraries; signature policy deferred to SEC-14; viewer ACG pending GPU validation.
- `Tests.ImportIsolation` Release 399 cases/0 failed/5 skipped, Debug 399/0/1; `npm test` 357 green;
  all product projects build Debug and Release.
- Rejected: unconditional `/Qspectre` (MSB8040 on a machine without the component); global
  `/guard:ehcont` (LNK1386/LNK2047); MicrosoftSignedOnly now (unsigned payload); the runtime CFG
  policy call (denied); `/force:guardehcont` (not supported by this linker, measured).