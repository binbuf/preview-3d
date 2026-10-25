# Overlay of the registry ufbx port.
#
# `ufbx` formats a missing external MTL warning with the resolved path. A
# `mtllib` value made only of directory separators (for example `mtllib \`)
# resolves to an empty blob with a null data pointer, so the `%s` formatting
# dereferences null and faults inside `ufbxi_obj_load_mtl`. The pinned 0.23.0
# source has the defect (and upstream master still does), so keep the fix as a
# patch until a release that contains it can be pinned. ObjFuzz found this.
vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO ufbx/ufbx
    REF "v${VERSION}"
    SHA512 0825f24df9782cd5522748dbea0fa567cfc91346e1cd5540a115dc9b4b93717f9ea189b1052a3f17090fb3de942bd824eecfcd21e2fae1739f2c48cbb8808a97
    HEAD_REF master
    PATCHES
        fix-empty-mtllib-path.patch
)

file(COPY "${CMAKE_CURRENT_LIST_DIR}/CMakeLists.txt" DESTINATION "${SOURCE_PATH}")
vcpkg_cmake_configure(SOURCE_PATH "${SOURCE_PATH}")
vcpkg_cmake_install()
vcpkg_copy_pdbs()
file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/include")
vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/LICENSE")
