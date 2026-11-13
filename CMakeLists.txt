cmake_minimum_required(VERSION 3.20)
project(multiwifi
    VERSION 0.1.0
    DESCRIPTION "Native Linux desktop application for managing multiple Wi-Fi uplinks"
    LANGUAGES C
)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

# Enforce strict compiler warnings as mandated by AGENTS.md
add_compile_options(
    -Wall
    -Wextra
    -Wpedantic
    -Wformat=2
    -Wmissing-prototypes
    -Wstrict-prototypes
    -Wshadow
)

find_package(PkgConfig REQUIRED)
pkg_check_modules(GLIB REQUIRED IMPORTED_TARGET glib-2.0>=2.74)
pkg_check_modules(GIO REQUIRED IMPORTED_TARGET gio-2.0>=2.74)
pkg_check_modules(GIO_UNIX REQUIRED IMPORTED_TARGET gio-unix-2.0>=2.74)
pkg_check_modules(GTK4 REQUIRED IMPORTED_TARGET gtk4>=4.10)

include_directories(
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

# Core library (pure logic, logging, capabilities, config parsing)
add_library(multiwifi-core STATIC
    src/core/logging.c
    src/core/capabilities.c
    src/core/config.c
)
target_link_libraries(multiwifi-core PUBLIC
    PkgConfig::GLIB
    PkgConfig::GIO
    PkgConfig::GIO_UNIX
)
target_include_directories(multiwifi-core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

# Unprivileged GTK4 GUI executable
add_executable(multiwifi-gui
    src/gui/main.c
    src/gui/window.c
)
target_link_libraries(multiwifi-gui PRIVATE
    multiwifi-core
    PkgConfig::GTK4
)

# System Daemon stub executable
add_executable(multiwifi-daemon
    src/daemon/main.c
)
target_link_libraries(multiwifi-daemon PRIVATE
    multiwifi-core
)

# Tests
include(CTest)
if(BUILD_TESTING)
    add_subdirectory(tests)
endif()

# Installation rules
include(GNUInstallDirs)
install(TARGETS multiwifi-gui multiwifi-daemon
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)
install(FILES data/io.github.multiwifi.MultiWiFi.desktop
    DESTINATION ${CMAKE_INSTALL_DATADIR}/applications
)
