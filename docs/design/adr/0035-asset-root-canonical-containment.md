# 0035 — Canonicalize user-chosen asset roots before containment

## Status
accepted

## Context
`ResolveSidecarPath`'s last-resort fallback searches user-chosen asset roots
(`additionalSearchRoots`, supplied by the trusted UI). `AcceptCandidate` proves containment by
opening the candidate and comparing its `GetFinalPathNameByHandleW(FILE_NAME_NORMALIZED)` path against
a lowercased, trailing-separator directory prefix. For the primary file that prefix is already derived
from a canonical path, so both sides are in the `\\?\C:\...` extended-length form. The user-root
branch instead built the prefix from the *raw* root text (`C:\Assets`), which can never prefix the
candidate's `\\?\C:\Assets\...` canonical path. The fallback therefore always failed (audit F-11):
`resolved.file` was false for every normal-path root.

## Decision
Canonicalize each user-chosen root the same handle-based way as the candidate: open the directory with
`CreateFileW(..., FILE_FLAG_BACKUP_SEMANTICS)`, read its `GetFinalPathNameByHandleW(FILE_NAME_NORMALIZED)`
path, and lower-case it with a trailing separator to form the containment prefix. A root that cannot be
opened/canonicalized, or that canonicalizes to a non-directory, is skipped — never compared as raw text
(`CanonicalDirectoryPrefix` returns `std::nullopt`). The existing trailing-separator check, and the
rule that only the reference's leaf name is searched under the root and its `texture`/`textures`
subfolders, are unchanged.

Rejected: lower-casing/normalizing the raw root as a string. A string normalization must reimplement
`GetFinalPathNameByHandleW`'s reparse resolution, short-name expansion, and volume normalization, and
would disagree with the candidate side on exactly the cases that matter.

## Consequences
- Normal-path, `\\?\`-prefixed, trailing-separator, and case-different roots now resolve; the fallback
  works as designed and the `[sidecar-resolver][asset-root]` cases gate the broker/worker tasks.
- A root that cannot be canonicalized fails closed (skipped), not open.
- A reparse point *inside* a root that resolves outside it is still rejected, because the candidate's
  canonical path is compared against the canonical root prefix.
- The worker never sees a path; this stays entirely inside the trusted broker.