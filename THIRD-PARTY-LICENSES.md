# Third-party licenses

Preview3D is distributed under the Apache License 2.0; see [LICENSE](LICENSE)
and [NOTICE](NOTICE). It incorporates and links to third-party software that
remains subject to its own license terms.

The exact dependency set is declared in [vcpkg.json](vcpkg.json) and pinned by
[vcpkg-configuration.json](vcpkg-configuration.json). The portable and
installer release workflows copy the complete, version-specific upstream
license texts into each release package's `licenses` directory and include a
`THIRD-PARTY-NOTICES.txt` index. Those release-package files are authoritative
for a particular build.

The current package may include components from these projects:

| Component | License family |
| --- | --- |
| Catch2 | Boost Software License 1.0 |
| fastgltf | MIT |
| simdjson | Apache-2.0 / MIT |
| Draco | Apache-2.0 |
| Basis Universal | Apache-2.0, with enabled third-party notices as applicable |
| KTX-Software | Apache-2.0, with enabled third-party notices as applicable |
| meshoptimizer | MIT |
| libwebp | BSD-style |
| ufbx | MIT (the project also offers Unlicense) |
| lib3mf | BSD-2-Clause, with bundled dependency notices as applicable |
| libzip | BSD-3-Clause |
| bzip2 | BSD-style |
| TinyUSDZ | Apache-2.0, with enabled vendored-code notices |
| OpenUSD and oneTBB | Apache-2.0 |
| Open CASCADE Technology (OCCT) | LGPL-2.1; see the release package's `licenses/opencascade.txt` |
| zlib and Zstandard | zlib and BSD-3-Clause, respectively |

This table is a navigation aid, not a replacement for the complete license
texts. Before redistributing a modified build, regenerate the release package
from the pinned manifest and preserve its notices and license files.
