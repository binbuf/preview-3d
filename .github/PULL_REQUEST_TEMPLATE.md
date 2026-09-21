## Summary

<!-- What does this change do, and why? -->

## Related issue

<!-- Fixes #... or References #... -->

## Type of change

- [ ] Bug fix
- [ ] New feature
- [ ] Performance improvement
- [ ] Refactor or cleanup
- [ ] Documentation
- [ ] Build, packaging, or CI

## Testing

<!-- List the exact commands you ran and their results. A solution target only builds. -->

- [ ] `Tests.Unit` (Debug and Release)
- [ ] `Tests.ImportIsolation` (Debug and Release)
- [ ] App smoke or fixture lane (describe)
- [ ] Manual verification with a model (format and source)

## Checklist

- [ ] The change is focused on one logical issue.
- [ ] Untrusted parsing stays inside the AppContainer worker/hosts, and no remote assets are fetched.
- [ ] New parser paths have explicit bounds and fail closed.
- [ ] No generated output or large binaries are committed (`vcpkg_installed/`, `artifacts/`, `TestResults/`, `test-models/`).
- [ ] Documentation is updated for behavior or supported-subset changes.
