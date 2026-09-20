# STEP-001 constrained OCCT port. Derived from the vcpkg registry port at
# baseline 04a9d8e5212d01ee1dd9478eadd9caade4f8b0d4 with two changes:
#
#   1. no OpenGL dependency and no default features (freetype/fontconfig), so
#      the closure is the STEP/XDE/B-rep/meshing modules only; and
#   2. the DataExchange toolkit list is reduced to the STEP/XDE set, dropping
#      IGES, STL, OBJ, PLY, glTF, VRML, Cascade, and RWMesh. The XCAF
#      persistence toolkits are retained because STEPCAFControl_Provider.cxx
#      includes BinXCAFDrivers.hxx. OCCT's own dependency resolution re-adds
#      any toolkit a retained toolkit genuinely needs.
#
# Writers, samples, tests, Draw, DETools, and visualization are off. Resource
# files (STEP unit lexicons) are compiled in, so the host reads no external
# resource path.

string(REPLACE "." "_" VERSION_STR "V${VERSION}")
vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO Open-Cascade-SAS/OCCT
    REF "${VERSION_STR}"
    SHA512 807c1f8732926cfdabcfbdf8d6a0e76b8dba1a1e614afe084a467ffb4cfd80623f5e3afa7e9905b1ac96667c93e01b5f98ceaa8948a576a1093d98df98cc8f81
    HEAD_REF master
    PATCHES
        dependencies.patch
        drop-bin-letter-d.patch
        fix-pdb-find.patch
        fix-install-prefix-path.patch
        install-include-dir.patch
        fix-freetype.diff
)

file(READ "${SOURCE_PATH}/adm/MODULES" OCCT_MODULES_CONTENT)
string(REPLACE
    "DataExchange TKDE TKXSBase TKDESTEP TKDEIGES TKDESTL TKDEVRML TKDECascade TKDEOBJ TKDEGLTF TKDEPLY TKXCAF TKXmlXCAF TKBinXCAF TKRWMesh"
    "DataExchange TKDE TKXSBase TKDESTEP TKXCAF TKXmlXCAF TKBinXCAF"
    OCCT_MODULES_CONTENT "${OCCT_MODULES_CONTENT}")
file(WRITE "${SOURCE_PATH}/adm/MODULES" "${OCCT_MODULES_CONTENT}")

if (VCPKG_LIBRARY_LINKAGE STREQUAL "dynamic")
    set(BUILD_TYPE "Shared")
else()
    set(BUILD_TYPE "Static")
endif()

vcpkg_cmake_configure(
    SOURCE_PATH "${SOURCE_PATH}"
    DISABLE_PARALLEL_CONFIGURE
    OPTIONS
        -DBUILD_LIBRARY_TYPE=${BUILD_TYPE}
        -DBUILD_MODULE_FoundationClasses=ON
        -DBUILD_MODULE_ModelingData=ON
        -DBUILD_MODULE_ModelingAlgorithms=ON
        -DBUILD_MODULE_ApplicationFramework=ON
        -DBUILD_MODULE_DataExchange=ON
        -DBUILD_MODULE_Visualization=OFF
        -DBUILD_MODULE_Draw=OFF
        -DBUILD_MODULE_DETools=OFF
        -DBUILD_RESOURCES=ON
        -DBUILD_DOC_Overview=OFF
        -DINSTALL_DIR_LAYOUT=Unix
        -DINSTALL_DIR_DOC=share/trash
        -DINSTALL_DIR_SCRIPT=share/trash
        -DINSTALL_TEST_CASES=OFF
        -DINSTALL_SAMPLES=OFF
        -DUSE_TK=OFF
        -DUSE_FREETYPE=OFF
        -DUSE_TBB=OFF
        -DUSE_RAPIDJSON=OFF
        -DUSE_DRACO=OFF
        -DUSE_FREEIMAGE=OFF
        -DUSE_VTK=OFF
        -DUSE_FFMPEG=OFF
        -DUSE_OPENVR=OFF
        -DUSE_OPENCL=OFF
        -DUSE_JEMALLOC=OFF
        -DUSE_D3D=OFF
)

vcpkg_cmake_install()
vcpkg_cmake_config_fixup(CONFIG_PATH lib/cmake/opencascade)

# Keep the include tree relative, matching the registry port.
file(GLOB extra_headers
    LIST_DIRECTORIES false
    RELATIVE "${CURRENT_PACKAGES_DIR}/include/opencascade"
    "${CURRENT_PACKAGES_DIR}/include/opencascade/*.h"
)
list(JOIN extra_headers "|" extra_headers)
file(GLOB files "${CURRENT_PACKAGES_DIR}/include/opencascade/*.[hgl]xx")
foreach(file_name IN LISTS files)
    file(READ "${file_name}" filedata)
    string(REGEX REPLACE "(# *include) <([a-zA-Z0-9_]*[.][hgl]xx|${extra_headers})>" [[\1 "\2"]] filedata "${filedata}")
    file(WRITE "${file_name}" "${filedata}")
endforeach()

file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/include")
file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/share")
file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/share/trash")

vcpkg_install_copyright(
    FILE_LIST
        "${SOURCE_PATH}/LICENSE_LGPL_21.txt"
        "${SOURCE_PATH}/OCCT_LGPL_EXCEPTION.txt"
)
