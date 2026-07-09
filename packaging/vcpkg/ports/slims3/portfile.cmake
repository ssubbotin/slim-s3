vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO ssubbotin/slim-s3
    REF v${VERSION}
    SHA512 cf0e729e4fd3f596d94294d620644ebfda69b8bcbbdc48b7e8c3120a2bd382e08bc1091758a37aca83693e068f8594ae7e30527c6cdba373dbb08a9d32168ad1
    HEAD_REF main
)

vcpkg_cmake_configure(
    SOURCE_PATH "${SOURCE_PATH}"
    OPTIONS
        -DSLIMS3_BUILD_TESTS=OFF
)

vcpkg_cmake_install()

vcpkg_cmake_config_fixup(PACKAGE_NAME slims3 CONFIG_PATH lib/cmake/slims3)

vcpkg_copy_pdbs()

file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/include")

vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/LICENSE")
