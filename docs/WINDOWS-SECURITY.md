# Windows download and protection guidance

Preview 3D is distributed through this repository’s [GitHub Releases](https://github.com/binbuf/preview-3d/releases). Official release artifacts are Authenticode-signed and carry a GitHub build-provenance attestation, so Windows should recognize a genuine download. An older release, or an unsigned engineering build you made from source, may still trigger a warning. A warning does not establish that a file is unsafe, but it does mean you should verify it before running it.

## Verify the download first

Only download from the project’s Releases page. Each release includes a matching `.sha256` file. In PowerShell, calculate the download’s SHA-256 and compare it with that file’s value:

```powershell
Get-FileHash .\Preview3D-<version>-x64-setup.exe -Algorithm SHA256
```

Do not run a build whose source or checksum you cannot verify. On a work or school device, follow your organization’s policy or contact its administrator.

## Choose the warning that matches what you see

### “This file came from another computer…”

This is the Windows Attachment Manager mark applied to downloaded files. After verifying the source and checksum:

1. In File Explorer, right-click the downloaded ZIP or installer and select **Properties**.
2. On the **General** tab, select **Unblock** in the Security section.
3. Select **Apply**, then **OK**.

For a ZIP, unblock the ZIP *before* extracting it so its contents do not inherit the mark. Microsoft’s [Attachment Manager guidance](https://support.microsoft.com/en-us/windows/security/information-about-the-attachment-manager-in-microsoft-windows) describes this checkbox and its security implications.

### “Windows protected your PC”

This is commonly a Microsoft Defender SmartScreen reputation warning. Confirm that the file came from the Releases page and that its hash matches. If Windows presents **More info** followed by **Run anyway**, use it only after those checks. Do not disable antivirus or edit security policies merely to run the app.

### “Smart App Control blocked this app”

Smart App Control (SAC) is different: it does **not** offer a per-app or per-file exception, so the Properties **Unblock** checkbox and a SmartScreen override will not bypass SAC. Microsoft explains that SAC allows apps it recognizes as safe or that have a valid signature; otherwise it can block them.

A blocked bundled DLL can surface as a Bad Image dialog naming that file — for example `worker\zstd.dll` with `Error status 0xC0E90002`. SAC evaluates each image on its own, so an unsigned dependency is refused even when the main executable starts.

If you have verified this release and still choose to run it, the available user-level option is to turn off SAC:

1. Open **Windows Security**.
2. Select **App & browser control**.
3. Open **Smart App Control settings**.
4. Set **Smart App Control** to **Off**.

Turning off SAC lowers protection for all apps, not just Preview 3D. Do this only for a release you trust, and do not change it on a managed device without approval. On current Windows 11 releases, Microsoft says SAC can be re-enabled from Windows Security when it is available on the device.

## What we are doing

The project signs its release artifacts. A release is built only from a `v*` tag on `main`, by a workflow that (a) runs the full test gate first, (b) requires a reviewer-held deployment approval, (c) fails closed if the signing certificate is not configured, (d) Authenticode-signs every executable image in the payload with a timestamp, (e) signs the checksum files with a detached PKCS#7 signature, and (f) publishes a GitHub build-provenance attestation for the archives and their SBOM/manifest metadata. The release is uploaded to a draft and only then published; a published tag is never overwritten.

### Verifying a release

1. Download only from the Releases page.
2. Compare the download’s SHA-256 with the matching `.sha256` asset: `Get-FileHash .\Preview3D-<version>-x64-setup.exe -Algorithm SHA256`. The combined `Preview3D-<version>-SHA256SUMS` asset lists every published file.
3. If you use the GitHub CLI, confirm provenance: `gh attestation verify .\Preview3D-<version>-x64-setup.exe --repo binbuf/preview-3d`. This checks that the file was produced by this repository’s release workflow.
4. Inspect the Authenticode signature with `Get-AuthenticodeSignature` or the file’s **Digital Signatures** tab; the installed payload’s images are signed with a timestamp.

The unsigned path is an engineering-only local build: the packaging scripts (`packaging/portable/Create-PortableRelease.ps1`, `packaging/installer/Create-Installer.ps1`) warn and continue when no certificate thumbprint is passed. Those artifacts are never published by the release workflow. If a signature is missing or does not validate, treat the file as untrusted.

The release tag itself must be protected. Maintainers configure a tag protection rule (or ruleset) for `v*` on GitHub so only authorized users can create release tags, and a `release` environment with required reviewers and a tags-only deployment rule. Repository workflow code cannot enforce those settings; they are repository configuration.

Code signing is applied per executable image, and Smart App Control evaluates every image separately. A signed `Preview3D.exe` alone is not enough: the DLLs bundled beside it (the worker/OpenUSD/OCCT dependency closure) are signed in the same `Create-PortableRelease.ps1` pass, or SAC can still refuse one of them. If Windows nonetheless blocks an image, use the steps above rather than disabling protection globally.

For the technical details, see Microsoft’s [Smart App Control FAQ](https://support.microsoft.com/en-us/windows/security/threat-malware-protection/smart-app-control-frequently-asked-questions) and the [related Microsoft Q&A discussion](https://learn.microsoft.com/en-us/answers/questions/5637638/smart-app-blocked-my-app).
