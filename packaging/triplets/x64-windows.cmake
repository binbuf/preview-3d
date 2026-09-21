# CI-only overlay of the built-in x64-windows triplet.
#
# Passed to the release/dependency workflows via `--overlay-triplets`. Because
# vcpkg consults overlay triplets before the built-in ones, this shadows
# x64-windows *only for those invocations*; a developer running `vcpkg install`
# locally still gets the stock debug+release triplet.
#
# The release job packages Release only, so building vcpkg's debug half doubles
# the OpenUSD + OCCT compile time and roughly doubles the on-disk footprint for
# artifacts nothing consumes. VCPKG_BUILD_TYPE=release drops it. The body below
# is otherwise the stock x64-windows triplet plus that one line; see vcpkg's
# community triplet `x64-windows-release` for the same shape.
set(VCPKG_TARGET_ARCHITECTURE x64)
set(VCPKG_CRT_LINKAGE dynamic)
set(VCPKG_LIBRARY_LINKAGE dynamic)
set(VCPKG_BUILD_TYPE release)