# CI-only overlay of the built-in x64-windows-static-md triplet.
#
# Passed to the release/dependency workflows via `--overlay-triplets`. Because
# vcpkg consults overlay triplets before the built-in ones, this shadows
# x64-windows-static-md *only for those invocations*; a developer running
# `vcpkg install` locally still gets the stock debug+release triplet.
#
# Static libraries with a dynamic CRT: the release payload statically links its
# whole dependency closure so it ships no upstream DLLs. Only the app's own
# executables and Preview3DOpenUsdCore.dll remain as executable images, which
# keeps Smart App Control and code-signing coverage tractable. The CRT stays
# dynamic (Microsoft-signed app-local DLLs), so this is the -md variant.
#
# VCPKG_BUILD_TYPE=release drops vcpkg's debug half, which the release job never
# packages and which would double the OpenUSD + OCCT compile time and disk.
set(VCPKG_TARGET_ARCHITECTURE x64)
set(VCPKG_CRT_LINKAGE dynamic)
set(VCPKG_LIBRARY_LINKAGE static)
set(VCPKG_BUILD_TYPE release)
