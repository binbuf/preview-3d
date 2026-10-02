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
- [ ] In the general worker's `main`, before any parsing: `SetDefaultDllDirectories` with the same
      flags the hosts use, `AddDllDirectory` for the payload directory only, reset CWD to the
      payload directory, and clear inherited plug-in/loader environment variables.
- [ ] Prefer passing an explicit `lpEnvironment`/`lpCurrentDirectory` from `SandboxLauncher` so all
      three children start clean; keep the in-process hardening as defense in depth.
- [ ] Compile harness/fault flags out of Release binaries (or gate them behind a compile-time
      developer define), keeping the test binaries able to reach them. Enumerate every flag so
      none is forgotten.
- [ ] STEP preflight: apply the control-byte rule inside strings/comments too, and buffer up to the
      BOM length across `Feed` calls so a split signature is still detected.
- [ ] Tests: restriction suite still passes for all three children; a STEP probe with NUL in a
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
- [ ] All three children pass the restriction suite (`Tests.ImportIsolation`), Debug and Release.
- [ ] A Release binary rejects every removed flag with usage/exit, verified by a test or scripted
      check recorded in the Hand-off.
- [ ] Hand-off filled in.

## Hand-off
_(filled in by the implementing session: what landed, what deviated and why, what the next task must know)_