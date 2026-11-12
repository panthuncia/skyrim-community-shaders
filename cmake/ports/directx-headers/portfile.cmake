# Upstream vcpkg stops at 1.619.4. Drop this overlay once a port carries these headers.
#
# Header-only: the headers are copied, with a CMake config of our own. Upstream's CMake build also compiles the
# DirectX-Guids library, and in vcpkg's deep build tree that compile failed on path length (C1083). Nothing here needs the
# library: BasicRHI links the Windows SDK's dxguid.
vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO Microsoft/DirectX-Headers
    REF v${VERSION}
    SHA512 12fef7c1980846fcef8d26f64620ec7435be22551fb3705e8f0b8996ff7373327b7b6cf4b2198d97b7e9622111f96ebf43a278602dd6b544334e263e88cb87dd
    HEAD_REF main
)

set(VCPKG_BUILD_TYPE release)
set(VCPKG_POLICY_SKIP_CRT_LINKAGE_CHECK enabled)

file(INSTALL "${SOURCE_PATH}/include/" DESTINATION "${CURRENT_PACKAGES_DIR}/include")
file(INSTALL "${CMAKE_CURRENT_LIST_DIR}/directx-headers-config.cmake" DESTINATION "${CURRENT_PACKAGES_DIR}/share/${PORT}")
file(INSTALL "${CMAKE_CURRENT_LIST_DIR}/usage" DESTINATION "${CURRENT_PACKAGES_DIR}/share/${PORT}")
vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/LICENSE")
