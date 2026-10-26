#pragma once

// The version compiled into the viewer. It is also the version compared
// against the latest published release by platform/UpdateCheck.cpp.
//
// Keep this in sync with /version.txt (the file published with each GitHub
// release) and the other release-version sources; packaging/
// ReleaseMetadata.ps1 fails the build when they drift.
#define PREVIEW3D_VERSION L"0.6.3"
