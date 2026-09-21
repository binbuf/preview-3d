# Security policy

## Supported versions

Security fixes are provided for the latest [release](https://github.com/binbuf/preview-3d/releases/latest)
only. Builds from `main` are development snapshots and are not supported.

## Reporting a vulnerability

Please do not report security issues in public issues, discussions, or pull
requests.

Use GitHub's private vulnerability reporting for this repository:
**[Report a vulnerability](https://github.com/binbuf/preview-3d/security/advisories/new)**
(the **Security** tab, then **Report a vulnerability**). If that form is not
available to you, open an issue that asks a maintainer to contact you privately,
and include no technical details in the issue itself.

A useful report includes:

- The affected version or commit.
- A description of the issue and the impact you believe it has.
- Steps to reproduce, including a model or file if one is required. Do not send
  confidential files; minimize or synthesize an input if possible.
- Whether exploitation requires the user to open a file, use a specific format,
  or perform another action.

We aim to acknowledge reports within a few days and to coordinate a fix and
disclosure timeline with you. Credit is given to reporters who want it.

## In scope

Preview 3D treats every model file as untrusted. Reports that are in scope
include:

- Escapes from the AppContainer/Job containment of `Preview3DImportWorker.exe`,
  `Preview3DImportHost.exe` (OpenUSD), or `Preview3DStepHost.exe`, or privilege
  escalation through those processes.
- Memory-safety bugs reachable from parsing or decoding an untrusted model,
  including the compressed glTF (Draco, meshopt, KTX2/Basis, WebP) paths.
- Broker or path-handling bugs that read files outside the approved local
  sidecar set, follow traversal/UNC/ADS references, or fetch remote assets.
- Activation or command-line handling that bypasses format admission or loads
  the wrong executable.

## Out of scope

- Windows SmartScreen, Smart App Control, or attachment warnings for unsigned
  builds. See [Windows download and protection guidance](.docs/WINDOWS-SECURITY.md).
- Vulnerabilities in upstream dependencies that are not reachable through
  Preview 3D; report those to the upstream project.
- Resource exhaustion or denial of service within the documented Tier B limits.
- Missing features or format gaps that are already listed in
  [Format support and limits](.docs/FORMAT-SUPPORT.md).
- Model, texture, or screenshot licensing questions.
