#pragma once

#include <string>

// SemVer parsing/comparison for the update check. Kept free of WinHTTP so the
// unit suite can exercise it without opening a network connection.
namespace preview3d_update
{
// Removes surrounding whitespace, a leading 'v'/'V', and SemVer build
// metadata (+...). Returns an empty string when `text` is not a valid
// MAJOR[.MINOR[.PATCH]][-PRERELEASE] version.
std::wstring NormalizeVersion(const std::wstring& text);

// True when both inputs are valid versions and `candidate` has higher SemVer
// precedence than `current`. Invalid input is never treated as newer.
bool IsVersionNewer(const std::wstring& candidate, const std::wstring& current);

// Extracts the tag_name string from GitHub's releases/latest JSON response.
// Returns an empty string when the field is absent or malformed. This is a
// bounded, single-field extraction, not a general JSON parser: the response
// is untrusted input used only to obtain a version to display.
std::wstring ExtractGitHubReleaseTag(const std::wstring& json);
}
