# FBX no-GPU fuzz target

`FbxFuzz` is an explicitly built sanitizer target; it is not part of the
shipping solution. It fuzzes pinned ufbx load options, progress cancellation,
a one-entry bounded virtual sidecar stream, deterministic start-pose
evaluation, a bounded normalized scene walk, triangulation, and the trusted
host's protocol/shared-section copy-and-validate decoder. It creates no window
or GPU device and never opens a model-derived path.

```powershell
python tests/fuzz/prepare_fbx_seeds.py TestResults/fbx-007/fuzz-seeds
msbuild tests/fuzz/FbxFuzz.vcxproj /p:Configuration=Release /p:Platform=x64 /m:1
x64/Release/FbxFuzz.exe TestResults/fbx-007/fuzz-seeds -max_total_time=60 -timeout=5 -rss_limit_mb=512 -max_len=1048576
```

Run the same command after adding any minimized fault to the immutable corpus.
The target's 16 MiB primary / 1 MiB virtual-sidecar caps and 64 MiB ufbx
allocator cap are intentionally lower than product limits so a smoke run has a
tight, deterministic envelope. Release qualification still relies on the real
AppContainer/Job tests for process containment and product-size caps.
