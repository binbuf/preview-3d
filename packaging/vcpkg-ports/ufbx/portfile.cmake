vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO ufbx/ufbx
    REF "v${VERSION}"
    SHA512 0825f24df9782cd5522748dbea0fa567cfc91346e1cd5540a115dc9b4b93717f9ea189b1052a3f17090fb3de942bd824eecfcd21e2fae1739f2c48cbb8808a97
    HEAD_REF master
)

file(COPY "${CMAKE_CURRENT_LIST_DIR}/CMakeLists.txt" DESTINATION "${SOURCE_PATH}")
vcpkg_cmake_configure(SOURCE_PATH "${SOURCE_PATH}")
vcpkg_cmake_install()
vcpkg_copy_pdbs()
file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/include")
vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/LICENSE")
