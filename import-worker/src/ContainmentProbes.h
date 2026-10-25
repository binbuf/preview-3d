#pragma once

// Spike-only containment probes run by Preview3DImportWorker.exe under
// --probes / --hang / --overallocate to prove, from inside the sandbox, that
// the AppContainer token and Job Object launched by
// shared/import-broker/SandboxLauncher actually deny what they're supposed
// to deny. Each probe writes one newline-terminated line to the process's
// inherited stdout handle:
//   "<NAME>=<PASS|DENIED|FAIL|UNEXPECTED_SUCCESS> code=<n>\n"
// This is an ad hoc, spike-only report format -- not the real versioned wire
// format from .docs/design/03-file-formats-and-ingestion.md, which is
// separate, deferred work.

namespace import_worker {

// Attempts to open canaryPath, a file the parent created outside anything
// this AppContainer should have access to. Expects ERROR_ACCESS_DENIED.
void RunFilesystemEscapeProbe(const wchar_t* canaryPath);

// Attempts to connect() to a loopback listener the parent opened on `port`.
// Expects failure: a zero-capability AppContainer token lacks
// internetClient/internetClientServer, and loopback is blocked by default.
void RunNetworkEscapeProbe(unsigned short port);

// Attempts to CreateProcessW a copy of this same worker with --child-noop.
// Expects failure due to the Job Object's ActiveProcessLimit=1 (no
// breakaway).
void RunProcessSpawnEscapeProbe();

// Loops VirtualAlloc(MEM_COMMIT) until it fails, reporting whether the
// failure looks like the Job Object's ProcessMemoryLimit tripping (expected
// ERROR_COMMITMENT_LIMIT).
void RunOverallocateProbe();

// Reports readiness, then blocks forever so the parent can prove Job Object
// kill-on-close against a hung worker.
void RunHangProbe();

// Reports readiness, then spins in user mode forever so the parent can prove
// the Job Object's JOB_OBJECT_LIMIT_PROCESS_TIME terminates a process that
// burns CPU without ever blocking.
void RunCpuSpinProbe();

// Writes the terminating "DONE" line.
void ReportDone();

} // namespace import_worker
