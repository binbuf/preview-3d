vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO PixarAnimationStudios/OpenUSD
    REF ee47c679abde5b467a7b6a41f3b2285564a4222e
    SHA512 140c0d81b71f7b2a815dea78da37ebc85ebf7f1858f5f96ef5d497a8424a3f0722f0713b9908d659b0ad3d0e0a2d1093b2e90d38d36f60dd015f6cb662b6971e
    HEAD_REF release
)

# USD-002 intentionally carries only the native composition and schema core.
# The compatibility host does not render, execute Python, discover optional
# formats, or ship command-line tools. A monolithic DLL also gives the host a
# small, auditable app-local DLL allowlist while retaining OpenUSD's required
# generated schema and file-format plugInfo resources.
# The Preview3D release statically links its dependency closure so the shipped
# payload contains no upstream DLLs (only the app's own images, which can be
# signed). Honor the triplet so a dynamic developer build still works.
if(VCPKG_LIBRARY_LINKAGE STREQUAL "static")
    set(BUILD_SHARED_LIBS OFF)
else()
    set(BUILD_SHARED_LIBS ON)
endif()

vcpkg_cmake_configure(
    SOURCE_PATH "${SOURCE_PATH}"
    OPTIONS
        -DBUILD_SHARED_LIBS=${BUILD_SHARED_LIBS}
        -DPXR_BUILD_MONOLITHIC=ON
        -DPXR_BUILD_TESTS=OFF
        -DPXR_BUILD_EXAMPLES=OFF
        -DPXR_BUILD_TUTORIALS=OFF
        -DPXR_BUILD_USD_TOOLS=OFF
        -DPXR_BUILD_IMAGING=OFF
        -DPXR_BUILD_USD_IMAGING=OFF
        -DPXR_BUILD_USD_VALIDATION=OFF
        -DPXR_BUILD_EXEC=OFF
        -DPXR_BUILD_USDVIEW=OFF
        -DPXR_BUILD_ALEMBIC_PLUGIN=OFF
        -DPXR_BUILD_DRACO_PLUGIN=OFF
        -DPXR_BUILD_PRMAN_PLUGIN=OFF
        -DPXR_BUILD_OPENIMAGEIO_PLUGIN=OFF
        -DPXR_BUILD_OPENCOLORIO_PLUGIN=OFF
        -DPXR_ENABLE_MATERIALX_SUPPORT=OFF
        -DPXR_ENABLE_PYTHON_SUPPORT=OFF
        -DPXR_ENABLE_HDF5_SUPPORT=OFF
        -DPXR_ENABLE_OSL_SUPPORT=OFF
        -DPXR_ENABLE_PTEX_SUPPORT=OFF
        -DPXR_ENABLE_OPENVDB_SUPPORT=OFF
        -DPXR_ENABLE_GL_SUPPORT=OFF
        -DPXR_ENABLE_VULKAN_SUPPORT=OFF
        -DPXR_ENABLE_PRECOMPILED_HEADERS=ON
        -DPXR_PREFER_SAFETY_OVER_SPEED=ON
        -DPXR_OVERRIDE_PLUGINPATH_NAME=PREVIEW3D_DISABLED_PLUGIN_PATH
        -DPXR_INSTALL_LOCATION=usd
)

vcpkg_cmake_install()
vcpkg_cmake_config_fixup(CONFIG_PATH cmake)

# A dynamic OpenUSD installs its monolithic runtime beside the import library.
# Normalize that upstream layout so vcpkg app-local deployment and package
# validation can treat it like every other Windows DLL. A static build (the
# release triplet) has no runtime DLL or matching PDB to relocate.
if(EXISTS "${CURRENT_PACKAGES_DIR}/lib/usd_ms.dll")
    file(MAKE_DIRECTORY "${CURRENT_PACKAGES_DIR}/bin")
    file(RENAME "${CURRENT_PACKAGES_DIR}/lib/usd_ms.dll"
                "${CURRENT_PACKAGES_DIR}/bin/usd_ms.dll")
endif()
# The release-only CI triplet (VCPKG_BUILD_TYPE=release) installs no debug tree,
# so the debug relocation must not assume one exists.
if(EXISTS "${CURRENT_PACKAGES_DIR}/debug/lib/usd_ms.dll")
    file(MAKE_DIRECTORY "${CURRENT_PACKAGES_DIR}/debug/bin")
    file(RENAME "${CURRENT_PACKAGES_DIR}/debug/lib/usd_ms.dll"
                "${CURRENT_PACKAGES_DIR}/debug/bin/usd_ms.dll")
endif()
if(EXISTS "${CURRENT_PACKAGES_DIR}/lib/usd_ms.pdb")
    file(MAKE_DIRECTORY "${CURRENT_PACKAGES_DIR}/bin")
    file(RENAME "${CURRENT_PACKAGES_DIR}/lib/usd_ms.pdb"
                "${CURRENT_PACKAGES_DIR}/bin/usd_ms.pdb")
endif()
if(EXISTS "${CURRENT_PACKAGES_DIR}/debug/lib/usd_ms.pdb")
    file(MAKE_DIRECTORY "${CURRENT_PACKAGES_DIR}/debug/bin")
    file(RENAME "${CURRENT_PACKAGES_DIR}/debug/lib/usd_ms.pdb"
                "${CURRENT_PACKAGES_DIR}/debug/bin/usd_ms.pdb")
endif()

# The host links the single monolithic import library directly. Upstream's
# root pxrConfig embeds build-machine paths and is not relocatable; retaining
# it would contradict the private-payload contract.
file(REMOVE
    "${CURRENT_PACKAGES_DIR}/pxrConfig.cmake"
    "${CURRENT_PACKAGES_DIR}/debug/pxrConfig.cmake"
)
file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/share/openusd/pxrTargets.cmake")
file(REMOVE
    "${CURRENT_PACKAGES_DIR}/share/openusd/pxrTargets-debug.cmake"
    "${CURRENT_PACKAGES_DIR}/share/openusd/pxrTargets-release.cmake"
)

vcpkg_copy_pdbs()

file(REMOVE_RECURSE
    "${CURRENT_PACKAGES_DIR}/debug/include"
    "${CURRENT_PACKAGES_DIR}/debug/share"
)

vcpkg_install_copyright(
    FILE_LIST
        "${SOURCE_PATH}/LICENSE.txt"
        "${SOURCE_PATH}/NOTICE.txt"
)
