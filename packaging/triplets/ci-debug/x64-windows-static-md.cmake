# CI-only overlay of the built-in x64-windows-static-md triplet, Debug half.
#
# The CI test lane builds Debug as well as Release. The sibling
# packaging/triplets/x64-windows-static-md.cmake is release-only
# (VCPKG_BUILD_TYPE release), so a Debug build that restores through it has no
# <triplet>\debug\lib to link and fails with LNK1104/LNK2019 on the OpenUSD,
# OCCT, lib3mf and ktx closures. This overlay installs only the debug half so
# the Debug job links the matching _ITERATOR_DEBUG_LEVEL and the Release job
# still pays for release only; a local `vcpkg install` keeps the stock
# debug+release triplet.
#
# The folder is named ci-debug rather than debug because .gitignore ignores any
# [Dd]ebug/ directory; the path stays under packaging/triplets so the
# workflows' hashFiles('packaging/triplets/**') cache key covers it.
set(VCPKG_TARGET_ARCHITECTURE x64)
set(VCPKG_CRT_LINKAGE dynamic)
set(VCPKG_LIBRARY_LINKAGE static)
set(VCPKG_BUILD_TYPE debug)