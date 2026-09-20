# Installation and registration

Current implementation note (2026-09-18): the shipping engineering package
is NSIS-based, not the proposed MSI/WiX package below. It currently registers
interactive Open With/Default Apps for all direct formats, including `.3mf`,
and stages the exact worker-only `lib3mf`/libzip/zlib/bzip2 runtime closure.
It does not register any Explorer thumbnail handler. The MSI/thumbnail,
transactional repair, and signed clean-machine lifecycle sections below remain
design targets, not claims about the current NSIS installer.

## Package contract

Preview 3D ships as a signed, per-machine x64 MSI for Windows 11. The initial package identity is:

| Field | Value |
| --- | --- |
| Product name | Preview 3D |
| Manufacturer | Binbuf |
| UpgradeCode | {DEA1E9AA-95AA-4722-ADD1-3D2ABCE22617} |
| Install scope | perMachine, elevated |
| Default directory | ProgramFiles64Folder\Binbuf\Preview 3D |
| Platform | x64 only |
| Reboot | Not required in normal install/upgrade/uninstall |

ProductCode and PackageCode change for each major-upgrade package. MSI ProductVersion uses the first three numeric semantic-version components; the full build/commit identity is also embedded in file version resources and the release manifest.

WiX source lives under installer and is built in the same pinned toolchain as the binaries. All permanent component GUIDs, ProgIDs, CLSIDs, UpgradeCode, and registry paths are checked into source and never regenerated during a build.

## Payload

Release payload:

- Preview3D.exe;
- Preview3DImportWorker.exe plus fastgltf, the product STL/PLY parsers, ufbx, lib3mf, TinyUSDZ, the pinned Draco decoder, KTX/Basis transcoder, libwebp, DirectXTex/WIC, and meshoptimizer — every general-format parser/decoder the product ships, none of which is present in Preview3D.exe's own binary;
- Preview3DImportHost.exe plus the exact signed app-local OpenUSD libraries and release-manifest-hashed resources required by its minimal host build;
- Preview3DStepHost.exe plus the exact signed app-local constrained OCCT closure (modeling, data-exchange, and foundation toolkits only) required by its minimal STEP host build;
- Preview3DThumbnailProvider.dll;
- license/third-party notices;
- optional local documentation and uninstaller metadata.

Dependencies are statically linked where their licenses and support model permit. Draco, KTX/Basis, libwebp, DirectXTex, and the product PLY parser are part of the signed product dependency graph, but they and every other general-format parser link only into `Preview3DImportWorker.exe`, never into `Preview3D.exe`. OpenUSD and its required resources are private to `Preview3DImportHost.exe`; the OCCT closure is private to `Preview3DStepHost.exe`; neither is ever registered system-wide, placed on PATH, or loaded into the viewer/import-worker/thumbnail process. The MSI grants each import process's AppContainer identity read/execute access only to its own private payload; model and cache directories receive no such ACE for any process. The MSVC runtime is app-local or statically linked according to the release security servicing decision; the package must not assume a developer machine's runtime. The MVP uses the Windows 11 system D3D12/DXGI/WIC/DirectWrite components and does not install the Agility SDK, graphics drivers, codecs, or a system-wide parser runtime.

PDBs and private diagnostics are archived with the release but are not in the consumer MSI. Every product PE file carries product/file versions, company/product strings, high-DPI/long-path manifests as appropriate, CFG/CET/NX/ASLR flags, and the same Authenticode publisher.

## Import-process identities

Three distinct AppContainer profiles exist, one per import executable, so a policy or ACL change to one can never accidentally widen another:

- `Binbuf.Preview3D.ImportWorker` for `Preview3DImportWorker.exe`, created at or before the first Open in a session;
- `Binbuf.Preview3D.ImportHost` for `Preview3DImportHost.exe`, created only on first USD fallback for a user;
- `Binbuf.Preview3D.StepHost` for `Preview3DStepHost.exe`, created only on first STEP/STP open for a user.

Each profile's deterministic package SID is part of the installer manifest. The installer grants each SID read/execute access only to its own private payload — the worker's general-format parser/decoder binaries, the host's OpenUSD libraries, or the STEP host's OCCT closure — never to another's payload, and never to model or cache directories. Profile creation for any identity is not on ordinary startup beyond what that process's first use requires, and grants no model, cache, registry, device, or network access by itself.

Per-generation pipe, event, and shared-section ACLs admit only the current viewer identity, LocalSystem where required for diagnostics, and the relevant import process's AppContainer SID — never two import SIDs on the same objects. Handles are non-inheritable except for an explicit minimal launch list passed through `PROC_THREAD_ATTRIBUTE_HANDLE_LIST`. Uninstall removes the machine payloads/ACLs for all three profiles and attempts to remove the initiating user's unused profiles; profiles created by other users may remain as inert security identities with no payload or resource grant and are documented for administrator cleanup.

## Supported extensions

Direct-open extensions:

    .glb .gltf .stl .ply .obj .fbx .3mf .usd .usda .usdc .usdz .step .stp

.mtl is deliberately absent. It is resolved only as an OBJ sidecar and is neither an Open With target nor a thumbnail handler.

The application registers capabilities and ProgIDs but never writes the user's selected default value for an extension. Windows Default Apps/user choice remains authoritative.

## Application ProgIDs

Stable versioned ProgIDs:

| Family | ProgID | Extensions |
| --- | --- | --- |
| glTF | Binbuf.Preview3D.glTF.1 | .glb, .gltf |
| STL | Binbuf.Preview3D.STL.1 | .stl |
| PLY | Binbuf.Preview3D.PLY.1 | .ply |
| OBJ | Binbuf.Preview3D.OBJ.1 | .obj |
| FBX | Binbuf.Preview3D.FBX.1 | .fbx |
| 3MF | Binbuf.Preview3D.ThreeMF.1 | .3mf |
| USD | Binbuf.Preview3D.USD.1 | .usd, .usda, .usdc, .usdz |
| STEP | Binbuf.Preview3D.STEP.1 | .step, .stp |

Each ProgID declares:

- friendly type name “3D model (<family>)”;
- DefaultIcon pointing to an indexed icon resource in Preview3D.exe;
- shell\open\command exactly quoting the executable and argument:

      "[INSTALLFOLDER]Preview3D.exe" --open "%1"

- FriendlyAppName “Preview 3D”.

The MSI also adds each ProgID under the extension's OpenWithProgids and lists extensions under Applications\Preview3D.exe\SupportedTypes.

Capabilities are registered under:

    HKLM\Software\Binbuf\Preview3D\Capabilities

with ApplicationName, ApplicationDescription, ApplicationIcon, and FileAssociations values. HKLM\Software\RegisteredApplications maps “Preview 3D” to that capabilities path. App Paths registers Preview3D.exe for discovery without adding the install directory to PATH.

The registration is visible in Windows Settings > Apps > Default apps. Setup may offer a final-page button that opens the Windows Default Apps UI; it must not simulate user input or claim defaults were assigned.

## Thumbnail COM registration

The CLSIDs and family mapping in [05-thumbnail-provider.md](./05-thumbnail-provider.md) are registered under the native 64-bit HKLM\Software\Classes\CLSID view:

    CLSID\{family-clsid}\
      (Default) = "Preview 3D <family> Thumbnail Provider"
      InprocServer32\
        (Default) = "[INSTALLFOLDER]Preview3DThumbnailProvider.dll"
        ThreadingModel = "Apartment"

For each direct extension:

    HKLM\Software\Classes\<extension>\shellex\
      {E357FCCD-A995-4576-B01F-234630154E96} = "{family-clsid}"

The handler key is the Windows thumbnail-handler category. Registration does not add IPreviewHandler, context-menu, property, icon-overlay, or property-handler entries.

MSI component ownership is exact: every key/value has a component/key path and uninstall removes only values installed by this product. If a pre-existing non-product thumbnail handler occupies the extension's handler value, the install records a conflict and does not overwrite it silently. The UI reports that interactive Open With support installed but the conflicting thumbnail integration was preserved. Repair follows the same non-clobber rule unless the existing value is one of this product's CLSIDs.

## Installation flow

1. Verify Windows 11 x64 and sufficient installer privileges.
2. Detect related products by UpgradeCode and block downgrade.
3. Use Restart Manager to request graceful close of Preview3D.exe, its import worker and compatibility host, and the isolated thumbnail surrogate if a product DLL is loaded. Never terminate or restart Explorer.
4. Install files to a staging/versioned component path, verify hashes through MSI, then commit registration.
5. Broadcast association/thumbnail changes from an impersonated, non-elevated notification action in the initiating interactive session using SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, null, null).
6. Complete without launching the viewer by default.

The notification action changes no machine state and is nonfatal; registration remains correct if notification is delayed until Explorer next refreshes. Silent install contains no UI and never launches the app.

The installer checks OS/platform before copying but does not use a D3D12 device capability check as a launch condition: GPU availability can change with Remote Desktop or drivers, and the app owns the actionable diagnostic.

## Upgrade

A major upgrade:

- detects the prior product with the stable UpgradeCode;
- prevents a lower ProductVersion unless an explicit enterprise downgrade policy is used;
- schedules RemoveExistingProducts inside the transactional, tested WiX major-upgrade sequence so component rules cannot remove the newly installed files or leave a CLSID pointing at a missing DLL;
- uses Restart Manager so loaded binaries are released gracefully;
- preserves the per-user cache-enabled preference and compatible derived entries; incompatible cache schemas become misses and are trimmed by the next viewer launch;
- installs new registration atomically enough that an old CLSID never points to a missing DLL.

The thumbnail provider is unloadable and keeps no persistent background threads, allowing its surrogate to close normally. If Windows or third-party software refuses to release an in-use binary, MSI reports FilesInUse and gives the user a cancel/retry choice; an unattended deployment may return 3010 for an exceptional deferred replacement. “No reboot normally” is tested, not achieved by killing Explorer.

Binary/parser changes that can affect output increment the file version. The installer invalidates notification state so Explorer can regenerate thumbnails; it does not recursively delete the user's system thumbnail cache.

## Repair

Repair restores missing/corrupt product files including the import-worker and compatibility-host payloads, owned ProgIDs, capabilities, App Paths, and product-owned COM mappings. It:

- re-verifies Authenticode/file hashes through MSI;
- does not replace another vendor's handler written after installation;
- re-sends the association-change notification;
- does not change user-selected defaults.

## Uninstall

Uninstall cancels if the user declines a Restart Manager close request for a running viewer. Otherwise it removes:

- product files and empty product directories;
- product-owned CLSIDs and InprocServer32 values;
- only extension handler values still equal to this product's assigned CLSID;
- ProgIDs, OpenWithProgids values, capabilities, RegisteredApplications, App Paths, and ARP entry owned by the package.

It leaves source models, Windows thumbnail cache data, per-user derived-cache entries/preferences, user default choices pointing to other products, and unrelated keys intact. MSI does not traverse every user profile. The app's Clear cached previews command removes the current user's entries before uninstall, and support documentation identifies `%LOCALAPPDATA%\Binbuf\Preview 3D` for manual post-uninstall removal. After removal MSI sends the same association notification. There is no service, task, tray startup item, firewall rule, or protocol handler to remove.

If a user had selected Preview 3D as default, Windows may show no current default after uninstall; setup must not select a replacement on the user's behalf.

## Signing and supply chain

- MSI, both EXEs, every app-local DLL, the thumbnail DLL, and any bootstrapper/custom-action binary are SHA-256 Authenticode signed with timestamping.
- CI verifies signatures after packaging and again on an installed image.
- Third-party versions/commits and licenses are pinned in a lock manifest; source/archive checksums are verified before build.
- Release artifacts include SBOM, notices, reproducible build inputs, checksums, symbols, and format-limit documentation.
- No install-time download or dynamic dependency fetching is allowed.

## Installer logging and rollback

All mutation uses MSI tables or rollback-aware custom actions. Custom actions do not parse model files and do not embed shell command strings. Failures return standard MSI codes and leave verbose logs when requested. Rollback restores previous product-owned registration and payload; association notification is sent after rollback completion.

No secrets, source-model paths, user document inventory, or telemetry identifier enter installer logs.

## Acceptance matrix

Clean install, repair, same-version repair, major upgrade, blocked downgrade, uninstall, rollback-injected failure, silent install, non-admin elevation, handler conflict, loaded viewer/import host, loaded thumbnail provider, and enterprise deployment are tested on clean Windows 11 images.

For each extension, tests verify:

- Preview 3D appears in Open With and Default Apps;
- existing user default remains unchanged;
- command quoting preserves adversarial valid paths;
- correct thumbnail CLSID is selected;
- Explorer shows a thumbnail or safe fallback without crashing;
- uninstall removes product ownership and restores usable Explorer behavior.

Import-process package tests, run identically for `Preview3DImportWorker.exe`, `Preview3DImportHost.exe`, and `Preview3DStepHost.exe`, additionally verify that each AppContainer SID can read/execute only its own private installed payload (never another process's), cannot read a model/cache directory without a brokered handle, has no network capability, and loses all executable payload access after uninstall. Current-user profile cleanup and documented inert profiles for other users are verified separately for all three profiles.

Primary references: [Default Programs registration](https://learn.microsoft.com/windows/win32/shell/default-programs), [file associations](https://learn.microsoft.com/windows/win32/shell/fa-file-types), [thumbnail handlers](https://learn.microsoft.com/windows/win32/shell/thumbnail-providers), [AppContainer isolation](https://learn.microsoft.com/windows/win32/secauthz/appcontainer-isolation), and [Restart Manager](https://learn.microsoft.com/windows/win32/rstmgr/restart-manager-portal).
