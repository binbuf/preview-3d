---
verify: x64\Release\Tests.ImportIsolation.exe
---

# T09 — Child-process loader/plugin hardening

## Goal
Every sandboxed child establishes a safe DLL search path and a scrubbed environment before parsing,
production binaries no longer accept developer/test fault modes, and STEP lexical admission rejects
control bytes in strings/comments regardless of how input is fed.

## Context (read first)
- `import-worker/src/main.cpp:48-56` — no `SetDefaultDllDirectories`, `AddDllDirectory`, CWD reset,
  or environment scrub; the launcher passes null env/CWD (`shared/import-broker/src/SandboxLauncher.cpp:76-79`),
  so the viewer's CWD/environment leak in. Both hosts do this correctly:
  `compatibility-host/src/main.cpp:54-70`, `compatibility-host-step/src/main.cpp:71-85`.
  Directly contradicts `docs/design/09-quality-performance-and-security.md:216`.
- Harness flags accepted by production binaries: `import-worker/src/main.cpp:61-167` (`--hang`,
  `--test-*`, `--overallocate`, `--probes`, `--*-spike`); `compatibility-host/src/main.cpp:230-267`
  and `compatibility-host-step/src/main.cpp:231-280` (`--pool-*`, `--*-spike`). The broker never
  passes them, but they ship; `docs/design/06-application-lifecycle-and-ipc.md:27` says developer
  switches are compiled out or rejected by production registration.
- `compatibility-host-step/src/StepPart21Preflight.cpp:89-121` — BOM/UTF-16/XML sniffing runs only
  on the first non-empty `Feed` chunk; `:136-168,181-184` — control-byte checks apply only outside
  strings/comments, so an embedded NUL inside a quoted string or comment is admitted to OCCT.
- Tests: `tests/import-isolation/SandboxLaunchTests.cpp` (restriction suite),
  `StepQualificationTests.cpp`, `tests/fuzz/StepFuzz.cpp`.

## Scope
- [x] In the general worker's `main`, before any parsing: `SetDefaultDllDirectories` with the same
      flags the hosts use, `AddDllDirectory` for the payload directory only, reset CWD to the
      payload directory, and clear inherited plug-in/loader environment variables.
- [x] Prefer passing an explicit `lpEnvironment`/`lpCurrentDirectory` from `SandboxLauncher` so all
      three children start clean; keep the in-process hardening as defense in depth.
- [x] Compile harness/fault flags out of Release binaries (or gate them behind a compile-time
      developer define), keeping the test binaries able to reach them. Enumerate every flag so
      none is forgotten.
- [x] STEP preflight: apply the control-byte rule inside strings/comments too, and buffer up to the
      BOM length across `Feed` calls so a split signature is still detected.
- [x] Tests: restriction suite still passes for all three children; a STEP probe with NUL in a
      string/comment and a BOM split across feeds is rejected by the preflight.

## Out of scope
- Adding new capabilities to children; the sandbox policy is unchanged.
- Process mitigation attributes (→ SEC-10).

## Design notes
- Removing a flag is a behavior change for the test harnesses: update the harness/test projects in
  the same session and record the replacement invocation.
- Environment scrubbing should be explicit (`SetEnvironmentVariableW(name, nullptr)` for a known
  list) and documented; do not call `ClearEnvironment` blind without testing OCCT/USD startup.
- These paths run before sandboxed parsing today only because the broker always uses `--pool`; make
  that an invariant, not a convention.

## Done when
- [x] All three children pass the restriction suite (`Tests.ImportIsolation`), Debug and Release.
- [x] A Release binary rejects every removed flag with usage/exit, verified by a test or scripted
      check recorded in the Hand-off.
- [x] Hand-off filled in.

## Hand-off

Landed
- `import-worker/src/main.cpp`: `ExecutableDirectory()` + `HardenProcessDiscovery()` now run before
  any request is parsed — `SetDefaultDllDirectories(SYSTEM32|USER_DIRS)`, one
  `AddDllDirectory(payload)`, `SetCurrentDirectoryW(payload)`, and an explicit scrub of the union
  of OpenUSD/plugin and OCCT `CSF_*` variables (including `PATH`). Failure exits before parsing.
- `shared/import-broker/src/SandboxLauncher.cpp`: `LaunchSuspendedSandboxedWithSid` now builds a
  scrubbed Unicode environment block from the caller's environment (parent base minus the same
  plug-in/loader names) and passes the child's own payload directory as `lpCurrentDirectory`.
  **Gotcha:** the non-obvious part is `CREATE_UNICODE_ENVIRONMENT` — without it `CreateProcessW`
  reads the wide block as ANSI and every launch fails (`nullopt`) with no diagnostic.
- Fault-harness gating: `PREVIEW3D_ENABLE_FAULT_HARNESS` is defined in `Directory.Build.props` for
  Debug only. It guards the fault switches in all three `main.cpp` files. Removed from Release:
  worker `--hang`, `--cpu-spin`, `--test-hang-import`, `--test-invalid-import-reply`,
  `--overallocate`, `--child-noop`, `--parse-gltf-delayed-batches`, `--test-parse-stl-ascii`,
  `--test-parse-ply-ascii`; USD host `--pool-crash`, `--pool-hang`, `--pool-overallocate`,
  `--pool-stale`, `--pool-reverse-fallback`; STEP host `--pool-crash`, `--pool-hang`,
  `--pool-overallocate`, `--pool-stale`, `--pool-wrong-format`, `--pool-unknown-error`.
- `compatibility-host-step/src/StepPart21Preflight.{h,cpp}`: the control-byte rule moved to the top
  of `Consume` (rejects NUL/control bytes inside strings and comments); the leading signature probe
  now buffers up to four bytes across `Feed` calls (`DecideLeadingSignature`, flushed by `Finish`),
  so a split UTF-8/UTF-16 BOM, ZIP, gzip, or XML signature is still classified.
- Tests: `StepQualificationTests.cpp` adds `[step-008][preflight-control][security]` (NUL in
  string/comment, 0x1F in string, legal tab still Ok) and `[step-008][preflight-split][security]`
  (BOM/PK/gzip/XML fed one/two bytes at a time). `SandboxLaunchTests.cpp` adds
  `[sandbox][security]` "Release binaries reject compiled-out fault flags" which launches each child
  directly with every removed flag and asserts a nonzero exit (Release-only; skipped in Debug).
- Docs: `docs/design/adr/0038-child-loader-hardening-and-fault-harness-gating.md`;
  `docs/design/09-quality-performance-and-security.md` (process/binary bullets);
  `docs/design/06-application-lifecycle-and-ipc.md` (developer-switch rule).

Deviations
- Benign verification entry points stay in Release: `--probes`, `--pool`, `--generate`, the
  `--*-spike`/`--*-spike-pool` qualification routes, and the normal `--parse-*` routes. The task
  Context listed `--probes`/`--*-spike` among harness flags, but the Goal says "fault modes" and
  design/09 requires the AppContainer/Job restriction suite against every child; compiling those
  out would remove the Release containment proof. Only fault injection is compiled out.
- Fault-injection test cases are `SKIP`ped (not run) under Release; Debug keeps full coverage.
  This is observable in the suite summary (Release 5 skips).
- `Tests.ImportIsolation` (compiled into the test exe, not the child) is what runs `--hang`
  equivalent hang tests now via `--pool` (production), so those sandbox cases stay in Release.

Check results (commands and actuals)
- Build Release: `& "C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe"
  tests\import-isolation\Tests.ImportIsolation.vcxproj /p:Configuration=Release /p:Platform=x64
  "/p:SolutionDir=D:\repos\binbuf\preview-3d\\" /m` → success.
- `x64\Release\Tests.ImportIsolation.exe` → **398 cases, 393 passed, 5 skipped, 0 failed**
  (293022 assertions). The 5 skips are the fault-injection cases (`[sandbox]` commit-limit,
  `[sandbox]` cpu-time, `[recovery]` hang/crash, `[step-002]` host faults, `[usd-006]` host faults).
- Build Debug (same command, `Configuration=Debug`) → success.
- `x64\Debug\Tests.ImportIsolation.exe` → **398 cases, 395 passed, 2 failed, 1 skipped**, 293017/293019
  assertions. The 2 failures are the pre-existing Job-commit-pressure cases documented under T03–T06a
  (`ThreeMfSpikeTests.cpp:630` memory-pressure recovery, `UsdSpikeTests.cpp:296` low cap); not caused
  by this task. The 1 skip is the Release-only rejection test.
- Build/run `Tests.Unit` Release (`npm test`): `135005 assertions in 357 test cases` all passed.
- Targeted: `x64\Release\Tests.ImportIsolation.exe "[sandbox]"` → 16 cases, 749/749 assertions;
  `"[step-008]"` → 5 cases, 52 assertions.

Exact remaining work / next task must know
- T11 (viewer attack surface): `interactive-viewer/src/app/Preview3D.cpp` still exposes
  `--benchmark-worker-budget-failure` (sets `faultForTesting=6`) and a WM fault-injection message
  that make the viewer pass now-compiled-out flags to a Release child; compile that switch out or
  reject it in Release before relying on the benchmark fault path.
- The ROADMAP/task file `## Out of scope` items (process mitigation attributes) are untouched; SEC-10
  owns them.