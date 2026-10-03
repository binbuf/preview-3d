# Third-party licenses

Preview3D is distributed under the Apache License 2.0; see [LICENSE](LICENSE)
and [NOTICE](NOTICE). It incorporates and links to third-party software that
remains subject to its own license terms.

The exact dependency set is declared in [vcpkg.json](vcpkg.json) and pinned by
[vcpkg-configuration.json](vcpkg-configuration.json) to the vcpkg baseline
[`04a9d8e5212d01ee1dd9478eadd9caade4f8b0d4`](https://github.com/microsoft/vcpkg/tree/04a9d8e5212d01ee1dd9478eadd9caade4f8b0d4).
The portable and installer release workflows generate the component list, the
CycloneDX SBOM, and the copied license texts from the **installed** vcpkg
closure (`vcpkg_installed/**/vcpkg/status` and each isolated STEP tree), and
fail the build if an installed package has no reviewed SPDX mapping in
[`packaging/portable/dependency-licenses.json`](packaging/portable/dependency-licenses.json).
Those release-package files (`SBOM.cdx.json`, `THIRD-PARTY-NOTICES.txt`, and
`licenses/`) are authoritative for a particular build.

The table below is a navigation aid generated from the same mapping; it lists
the package name, the pinned version for the current baseline, and the license
family. It is checked against the mapping (and the pinned baseline) by
`packaging/Test-ReleaseMetadata.ps1`, so it cannot silently omit a component.

| Package | Version | License family |
| --- | --- | --- |
| basisu | 2.50 | Apache-2.0 |
| bzip2 | 1.0.8#6 | bzip2-1.0.6 |
| catch2 | 3.16.0 | Boost Software License 1.0 (BSL-1.0) |
| cpp-base64 | V2.rc.08 | zlib-style (René Nyffenegger) |
| draco | 1.5.7#1 | Apache-2.0 |
| egl-registry | 2025-05-27 | Apache-2.0 and MIT |
| fast-float | 8.2.10#1 | MIT or Apache-2.0 |
| fastgltf | 0.9.0 | MIT |
| ktx | 4.4.2 | Apache-2.0, with enabled third-party notices as applicable |
| lib3mf | 2.5.0#1 | BSD-2-Clause, with bundled dependency notices as applicable |
| libwebp | 1.6.0#3 | BSD-3-Clause |
| libzip | 1.11.4 | BSD-3-Clause |
| meshoptimizer | 1.2 | MIT |
| opencascade | 7.8.1#1 | LGPL-2.1; see the release package's `licenses/opencascade.txt` |
| opengl-registry | 2026-08-03 | Apache-2.0 and MIT |
| openusd | 26.8.0 | Apache-2.0 |
| simdjson | 4.6.8 | Apache-2.0 |
| tbb | 2023.1.0 | Apache-2.0 |
| tinyusdz | 0.9.1#3 | Apache-2.0, with enabled vendored-code notices |
| ufbx | 0.23.0 | MIT (the project also offers Unlicense) |
| zlib | 1.3.2#2 | Zlib |
| zstd | 1.5.7 | BSD-3-Clause |

This table is a navigation aid, not a replacement for the complete license
texts. Before redistributing a modified build, regenerate the release package
from the pinned manifest and preserve its notices and license files.