# 0038 — Child loader/discovery hardening, explicit launch environment, and fault-harness gating

## Status
accepted

## Context
`import-worker/src/main.cpp` did not establish a safe loader state before parsing:
`SetDefaultDllDirectories`/`AddDllDirectory`/CWD reset/plug-in environment scrub were missing, and
`import-broker/src/SandboxLauncher.cpp` passed a null environment and working directory to
`CreateProcessW`, so the viewer's CWD/environment leaked into every child. Both compatibility hosts
already did the in-process hardening, so the general worker contradicted design/09's "safe DLL
search established before optional loads". Separately, every production child shipped fault/test
switches (`--hang`, `--overallocate`, `--probes`-adjacent fault modes, `--pool-crash`, ...), and
`StepPart21Preflight.cpp` only applied its control-byte rule outside strings/comments and only
sniffed the signature on the first non-empty `Feed` chunk.

## Decision
- **In-process hardening for every child.** `Preview3DImportWorker.exe` now mirrors the hosts:
  `SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_SYSTEM32 | LOAD_LIBRARY_SEARCH_USER_DIRS)`, one
  `AddDllDirectory(payload dir)`, `SetCurrentDirectoryW(payload dir)`, and an explicit scrub of the
  union of OpenUSD/plugin and OCCT `CSF_*` loader variables before any request is parsed. Failure
  is fail-closed (exit before parsing).
- **Explicit launch environment and working directory.** `LaunchSuspendedSandboxedWithSid` builds a
  `CREATE_UNICODE_ENVIRONMENT` block from the caller's environment minus the same scrub list (so
  `SystemRoot`/`TEMP` remain for OCCT/USD) and passes the child's own payload directory as
  `lpCurrentDirectory`. In-process hardening stays as defense in depth. The Unicode flag is
  required: without it `CreateProcessW` reads the wide block as ANSI and the launch fails.
- **Fault modes are Debug-only.** A `PREVIEW3D_ENABLE_FAULT_HARNESS` define (Directory.Build.props,
  Debug only) guards every fault/attack switch in the three children. Release production binaries
  reject them with their normal usage exit. Benign verification entry points (`--probes`,
  `--pool`, `--generate`, the `--*-spike`/`--*-spike-pool` qualification routes and the parse
  routes) stay in Release because design/09 requires the AppContainer/Job restriction suite against
  every child; only fault injection is compiled out.
- **STEP lexical admission is strict everywhere.** The control-byte rule is applied in `Consume`
  before the string/comment state machine, so an embedded NUL/control byte is rejected inside a
  quoted literal or `/* */` comment. The leading signature probe buffers up to four bytes across
  `Feed` calls, so a UTF-8/UTF-16 BOM, ZIP, gzip or XML signature split across reads is still
  detected.

## Consequences
- Restriction/fault-recovery cases that need a fault mode are gated on the same macro and are
  skipped (not failed) under Release; a Release-only case launches each child with every removed
  flag and asserts a nonzero exit. Debug keeps full fault coverage.
- `Tests.ImportIsolation` Release: 398 cases, 0 failures, 5 skips. Debug: the same two pre-existing
  Job-commit-pressure failures (ThreeMfSpike, UsdSpike) and one skip. `Tests.Unit` Release green.
- The viewer's `--benchmark-worker-budget-failure`/WM fault injection (T11 viewer attack surface)
  now hands a Release child flags it rejects; T11 must compile that developer switch out or reject
  it before it is relied on in a Release build.
- Rejected: scrubbing the environment from inside the child only (loader runs before `main`, so the
  broker must also pass a clean environment/CWD); an empty environment block (breaks OCCT/USD
  startup without their base variables); removing `--probes`/`--pool` (would remove the design/09
  restriction-suite proof against the production children).