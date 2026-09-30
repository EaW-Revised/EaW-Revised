set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)

if(NOT DEFINED ENV{EAWR_ARM_GNU_ROOT} OR "$ENV{EAWR_ARM_GNU_ROOT}" STREQUAL "")
    message(FATAL_ERROR "EAWR_ARM_GNU_ROOT must name the extracted Arm GNU Toolchain 14.2.Rel1 root")
endif()

file(TO_CMAKE_PATH "$ENV{EAWR_ARM_GNU_ROOT}" EAWR_ARM_GNU_ROOT)
set(EAWR_ARM_TRIPLE aarch64-none-linux-gnu)
set(EAWR_ARM_BIN "${EAWR_ARM_GNU_ROOT}/bin")

foreach(tool gcc g++ ar ranlib strip objcopy readelf)
    if(NOT EXISTS "${EAWR_ARM_BIN}/${EAWR_ARM_TRIPLE}-${tool}")
        message(FATAL_ERROR "Missing target tool: ${EAWR_ARM_BIN}/${EAWR_ARM_TRIPLE}-${tool}")
    endif()
endforeach()

set(CMAKE_C_COMPILER "${EAWR_ARM_BIN}/${EAWR_ARM_TRIPLE}-gcc")
set(CMAKE_CXX_COMPILER "${EAWR_ARM_BIN}/${EAWR_ARM_TRIPLE}-g++")
set(CMAKE_AR "${EAWR_ARM_BIN}/${EAWR_ARM_TRIPLE}-ar")
set(CMAKE_RANLIB "${EAWR_ARM_BIN}/${EAWR_ARM_TRIPLE}-ranlib")
set(CMAKE_STRIP "${EAWR_ARM_BIN}/${EAWR_ARM_TRIPLE}-strip")
set(CMAKE_OBJCOPY "${EAWR_ARM_BIN}/${EAWR_ARM_TRIPLE}-objcopy")

# The Arm compiler carries its matching glibc sysroot and libstdc++ search paths.
# Constrain discovery so host headers and libraries cannot enter target links.
set(CMAKE_SYSROOT "${EAWR_ARM_GNU_ROOT}/${EAWR_ARM_TRIPLE}/libc")
set(CMAKE_FIND_ROOT_PATH "${CMAKE_SYSROOT}" "${EAWR_ARM_GNU_ROOT}/${EAWR_ARM_TRIPLE}")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)
