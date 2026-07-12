# Cross-compilation toolchain for ARM64 (AArch64) Linux
# Target: Xiaomi Pad 6S Pro (SM8550) running Debian forky/sid

set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)

# Cross-compiler (will be set by Qt Creator kit)
# set(CMAKE_C_COMPILER aarch64-linux-gnu-gcc)
# set(CMAKE_CXX_COMPILER aarch64-linux-gnu-g++)

# Search for libraries/header in cross-target paths
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

# Qt6 cross-compilation settings
set(CMAKE_PREFIX_PATH /opt/qt6-arm64)
