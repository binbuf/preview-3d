# Screenshots

This directory holds the images used in the project [README](../../README.md).
Add the files here, then uncomment the screenshot block near the top of the
README (it is wrapped in an HTML comment with a `Screenshots:` marker).

## Suggested shot list

| File | What to show |
| --- | --- |
| `hero-studio.png` | A textured PBR model, Studio lighting, grid on. The main README image. |
| `hero-directional.png` | A second model with the Directional light and the navigation gizmo visible. |
| `info-panel.png` | A model with the Stats & Shading information panel open. |
| `shading-modes.png` | A 2x2 montage: Studio, Clay, Directional, and Wireframe on the same model. |
| `point-cloud.png` | A `.ply` point cloud. |
| `formats/obj.png`, `formats/fbx.png`, `formats/stl.png`, `formats/3mf.png`, `formats/usd.png`, `formats/step.png` | One representative model per format, for a format gallery. |

Keep the README images at roughly 1600x1000 or larger and under about 1 MB each;
they are displayed at half-width. Prefer PNG for UI text and lossless detail.
Use `F11` fullscreen or a clean window, and hide anything private (user names,
file paths, unrelated windows) before capturing.

## Model sources for screenshots

Every model used in a screenshot must have a license that allows redistribution
and commercial use. Prefer CC0 or public domain; CC BY is fine with credit.
Avoid models with NoDerivatives, NonCommercial, or unclear terms, and never
commit the model files themselves (`test-models/` is gitignored for this
reason) — only the screenshots.

| Source | License | Formats | Good for |
| --- | --- | --- | --- |
| [Poly Haven models](https://polyhaven.com/models) | [CC0](https://polyhaven.com/license) | glTF, FBX, USD, Blend | Realistic PBR props with 4K textures: `Drill 01`, `Camera 01`, `Chandelier 01`, `Marble Bust 01`, `Ship Pinnace`, `Portable Generator` |
| [Khronos glTF Sample Assets](https://github.com/KhronosGroup/glTF-Sample-Assets) | Per model; showcase is mostly [CC BY 4.0](https://creativecommons.org/licenses/by/4.0/), some CC0 | glTF, GLB | Feature-rich hero shots; browse the [showcase list](https://github.com/KhronosGroup/glTF-Sample-Assets/blob/main/Models/Models-showcase.md) or the [asset browser](https://github.khronos.org/glTF-Assets/) |
| [Smithsonian Open Access 3D](https://3d.si.edu/cc0) | CC0 | OBJ, glTF and other per-record formats | Museum scans with real-world detail; good for OBJ and point-cloud shots |
| [NASA 3D Resources](https://science.nasa.gov/3d-resources/) | Free to use under the [NASA media guidelines](https://www.nasa.gov/nasa-brand-center/images-and-media/) (no endorsement) | GLB, STL, and more | Spacecraft, rovers, and mission hardware; good for STL and GLB |
| [USD Working Group assets](https://github.com/usd-wg/assets) | Per asset (typically CC0 or permissive); repo Apache-2.0 | USD, USDZ | Small, well-structured USD scenes such as `StandardShaderBall`; preview them in the [web catalog](https://usd-assets.needle.tools/) |
| [DPEL](https://dpel.aswf.io/) | [ASWF Digital Assets License v1.1](https://aswf.io/licenses/aswf_digital_assets_license_v1.1.txt) | USD and more | Large production scenes such as `4004 Moore Lane` and `ALab` for USD hero shots |
| [Blender demo files](https://www.blender.org/download/demo-files/) | CC0 or CC BY (a few CC BY-SA); check each entry | Blend (export to OBJ/FBX/glTF/STL) | Human Base Meshes and Classroom are CC0; good for OBJ and FBX shots |
| [Quaternius](https://quaternius.com/) | [Quaternius Asset License](https://quaternius.com/license.html): free, no attribution, no resale of assets | glTF, FBX, OBJ, Blend | Stylized characters and props; good for FBX |
| [Kenney 3D assets](https://kenney.nl/assets?q=3d) | [CC0](https://kenney.nl/support) | Common interchange formats | Low-poly kits for clean, readable format-gallery images |
| [3MF Consortium samples](https://github.com/3MFConsortium/3mf-samples) | BSD-2-Clause | 3MF | Functional `.3mf` files for a format screenshot |
| [NIST MBE PMI models](https://www.nist.gov/ctl/smart-connected-systems-division/smart-connected-manufacturing-systems-group/mbe-pmi-0) | U.S. government work (public domain) | STEP (AP242) | CAD parts with real B-rep geometry for a STEP screenshot; PMI annotations themselves are not rendered |
| [Sketchfab CC0 filter](https://sketchfab.com/3d-models?features=downloadable&licenses=322a749bcfa841b29dff1e8a1bb74b0b) | CC0 only when filtered | Many; downloads expose glTF/GLB, OBJ, FBX, STL | One-off subjects and photogrammetry scans |

For 3D-printing models (STL/3MF), filter Printables or Thingiverse by
CC0/CC BY and verify the license on each model page; most listing sites mix
licenses per model.

## Attribution

CC0 and public-domain models need no credit. For CC BY and similar licenses,
add a line here and keep it in sync with the README image captions:

```markdown
- `hero-studio.png` — "<model name>" by <author>, licensed under <license>, via <source URL>.
```

Screenshots are content, not code: the project's Apache-2.0 license does not
replace the model's license. Do not include a screenshot unless its model's
license permits the use.
