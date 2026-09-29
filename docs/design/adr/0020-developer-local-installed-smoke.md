# 0020 — A developer/QA-local installed smoke precedes the installer

## Status
accepted

## Context
T22 has to prove the provider the way a user will use it — Release build, registered CLSID, a real
`.stl` thumbnail in Explorer's Shell path — before the eight-CLSID installer (T41), signing/SBOM
(T42) and clean-machine acceptance (T44) exist. The T03 spike proved surrogate hosting and the
extension `ShellEx` mapping with a throwaway probe; T17's `Tests.ProviderHost.exe` activates the real
DLL through COM but in-process and without registration. Neither is a registered, Shell-resolved
thumbnail of a real file. The run session is non-interactive and not elevated (T03), so a machine-wide
install and manual Explorer browsing are unavailable.

## Decision
The local smoke is a small, explicitly non-release procedure under `packaging/smoke/`:

- `Stage-ProviderSmoke.ps1` copies the built `Preview3DThumbnailProvider.dll` and its non-system
  runtime closure (the app-local MSVC CRT) into a scratch directory; there is no machine-wide install.
- `Register-ProviderSmoke.ps1` / `Unregister-ProviderSmoke.ps1` write and remove only the STL
  `CLSID`/`InprocServer32`/`ThreadingModel=Apartment`, the AppID with an empty `DllSurrogate`, and the
  T03-validated extension-level `ShellEx` mapping. Default scope is `HKCU` (per-user, no elevation);
  every touched key is backed up and restored exactly. No `DisableProcessIsolation` is ever written.
- `ProviderSmokeHost.exe` renders the same `.stl` twice: in-process through the DLL's PRIVATE
  `DllGetClassObject` (a reference image, because an `HBITMAP` cannot be marshaled across a surrogate),
  and through the real Shell path `IThumbnailCache::GetThumbnail`. It passes only when the two images
  match, so the Shell thumbnail is provably this provider's model-derived output. It also activates
  the CLSID with `CLSCTX_LOCAL_SERVER` and uses the T03 module-identity scan to observe the handler in
  `DllHost.exe` (never the caller), and checks the opt-out value is absent.
- `Invoke-ProviderSmoke.ps1` orchestrates build → stage → clear local thumbnail cache → register →
  verify → unregister, and always cleans up.

The committed `fixtures/smoke-cube.stl` is a small, recognizable binary cube so the thumbnail is
visually meaningful; the corpus `.stl` files are pathological fixtures and are not used here.

## Consequences
- Every later family task (T23–T34) re-runs the same procedure for its family by adding one CLSID and
  extension mapping; the verifier and cleanup are family-agnostic.
- This is developer/QA evidence, not the release artifact: it writes per-user keys and stages a
  scratch CRT, so it does not satisfy T41 (machine-level NSIS registration) or T44 (clean-machine,
  signed install, DPI). Those tasks keep their own acceptance.
- The reference-vs-Shell image comparison is the durable "model-derived, produced by this provider"
  check T44 can reuse; the module-identity scan is the same `IThumbnailCache` method T03/T44 use.