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

option(MULTIWIFI_BUILD_DAEMON "Build privileged multiwifi-daemon from Rust workspace (crates/multiwifi-daemon)" ON)

find_package(PkgConfig REQUIRED)
pkg_check_modules(GLIB REQUIRED IMPORTED_TARGET glib-2.0>=2.74)
pkg_check_modules(GIO REQUIRED IMPORTED_TARGET gio-2.0>=2.74)
pkg_check_modules(GIO_UNIX REQUIRED IMPORTED_TARGET gio-unix-2.0>=2.74)
pkg_check_modules(GTK4 REQUIRED IMPORTED_TARGET gtk4>=4.10)
pkg_check_modules(NM REQUIRED IMPORTED_TARGET libnm>=1.40)

include(CTest)

if(BUILD_TESTING)
    pkg_check_modules(NFTABLES REQUIRED IMPORTED_TARGET libnftables)
endif()

include_directories(
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

# Unprivileged GUI support library (no daemon/polkit/nftables code linked into GUI)
add_library(multiwifi-gui-support STATIC
    src/core/logging.c
    src/core/capabilities.c
    src/core/wifi_device.c
    src/core/wifi_inventory.c
    src/core/path_monitor.c
)
target_link_libraries(multiwifi-gui-support PUBLIC
    PkgConfig::GLIB
    PkgConfig::GIO
    PkgConfig::GIO_UNIX
    PkgConfig::NM
    m
)
target_include_directories(multiwifi-gui-support PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

# Unprivileged GTK4 GUI executable (C11)
add_executable(multiwifi-gui
    src/gui/main.c
    src/gui/window.c
)
target_link_libraries(multiwifi-gui PRIVATE
    multiwifi-gui-support
    PkgConfig::GTK4
)

# Privileged System Daemon executable (Rust 2021)
if(MULTIWIFI_BUILD_DAEMON)
    find_program(CARGO_EXECUTABLE cargo)
    if(NOT CARGO_EXECUTABLE)
        message(FATAL_ERROR "cargo executable not found. Install Rust toolchain or configure with -DMULTIWIFI_BUILD_DAEMON=OFF.")
    endif()

    set(RUST_TARGET_DIR "${CMAKE_BINARY_DIR}/rust-target")
    set(RUST_DAEMON_BIN "${RUST_TARGET_DIR}/release/multiwifi-daemon")
    set(STAGED_DAEMON_BIN "${CMAKE_BINARY_DIR}/multiwifi-daemon")

    file(GLOB_RECURSE MULTIWIFI_RUST_SOURCES CONFIGURE_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/crates/*.rs"
        "${CMAKE_CURRENT_SOURCE_DIR}/crates/*/Cargo.toml"
        "${CMAKE_CURRENT_SOURCE_DIR}/crates/multiwifi-core/build.rs"
    )

    add_custom_command(
        OUTPUT "${STAGED_DAEMON_BIN}"
        COMMAND "${CARGO_EXECUTABLE}" build
            --manifest-path "${CMAKE_CURRENT_SOURCE_DIR}/Cargo.toml"
            --package multiwifi-daemon
            --release
            --target-dir "${RUST_TARGET_DIR}"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${RUST_DAEMON_BIN}" "${STAGED_DAEMON_BIN}"
        BYPRODUCTS "${RUST_DAEMON_BIN}"
        DEPENDS
            "${CMAKE_CURRENT_SOURCE_DIR}/Cargo.toml"
            "${CMAKE_CURRENT_SOURCE_DIR}/Cargo.lock"
            ${MULTIWIFI_RUST_SOURCES}
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
        COMMENT "Building Rust multiwifi-daemon (release)"
        VERBATIM
    )
    add_custom_target(multiwifi-daemon-rust ALL DEPENDS "${STAGED_DAEMON_BIN}")
endif()

# Tests
if(BUILD_TESTING)
    add_subdirectory(tests)
endif()

# Installation rules
include(GNUInstallDirs)
install(TARGETS multiwifi-gui
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)
if(MULTIWIFI_BUILD_DAEMON)
    install(PROGRAMS "${STAGED_DAEMON_BIN}"
        DESTINATION ${CMAKE_INSTALL_BINDIR}
    )
endif()
install(FILES data/io.github.multiwifi.MultiWiFi.desktop
    DESTINATION ${CMAKE_INSTALL_DATADIR}/applications
)
install(DIRECTORY data/icons/
    DESTINATION ${CMAKE_INSTALL_DATADIR}/icons
)
install(FILES data/style.css
    DESTINATION ${CMAKE_INSTALL_DATADIR}/multiwifi
)
install(FILES data/io.github.multiwifi.Daemon1.xml
    DESTINATION ${CMAKE_INSTALL_DATADIR}/dbus-1/interfaces
)
install(FILES data/io.github.multiwifi.Daemon.conf
    DESTINATION ${CMAKE_INSTALL_DATADIR}/dbus-1/system.d
)
install(FILES data/io.github.multiwifi.policy
    DESTINATION ${CMAKE_INSTALL_DATADIR}/polkit-1/actions
)
install(FILES data/io.github.multiwifi.rules
    DESTINATION ${CMAKE_INSTALL_DATADIR}/polkit-1/rules.d
)
install(FILES data/multiwifi-daemon.service
    DESTINATION lib/systemd/system
)

# Packaging with CPack
set(CPACK_PACKAGE_NAME "multiwifi")
set(CPACK_PACKAGE_VENDOR "MultiWiFi Team")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Native Linux multi-path Wi-Fi load balancing and MPTCP bonding")
set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
set(CPACK_PACKAGE_VERSION_MAJOR "${PROJECT_VERSION_MAJOR}")
set(CPACK_PACKAGE_VERSION_MINOR "${PROJECT_VERSION_MINOR}")
set(CPACK_PACKAGE_VERSION_PATCH "${PROJECT_VERSION_PATCH}")
set(CPACK_PACKAGE_CONTACT "support@multiwifi.local")
set(CPACK_RESOURCE_FILE_LICENSE "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE")

# Debian (.deb)
set(CPACK_DEBIAN_PACKAGE_HOMEPAGE "https://github.com/TrVHau/multi-wifi")
set(CPACK_DEBIAN_PACKAGE_SECTION "net")
set(CPACK_DEBIAN_PACKAGE_PRIORITY "optional")
set(CPACK_DEBIAN_PACKAGE_DEPENDS "libgtk-4-1 (>= 4.10), libglib2.0-0 (>= 2.74), libnm0 (>= 1.40), libpolkit-gobject-1-0, libnftables1")
set(CPACK_DEBIAN_PACKAGE_MAINTAINER "MultiWiFi Team <support@multiwifi.local>")
set(CPACK_DEBIAN_PACKAGE_CONTROL_EXTRA
    "${CMAKE_CURRENT_SOURCE_DIR}/packaging/debian/postinst;${CMAKE_CURRENT_SOURCE_DIR}/packaging/debian/prerm;${CMAKE_CURRENT_SOURCE_DIR}/packaging/debian/postrm"
)

# RPM (.rpm)
set(CPACK_RPM_PACKAGE_LICENSE "MIT")
set(CPACK_RPM_PACKAGE_GROUP "Applications/Internet")
set(CPACK_RPM_PACKAGE_URL "https://github.com/TrVHau/multi-wifi")
set(CPACK_RPM_PACKAGE_REQUIRES "gtk4 >= 4.10, glib2 >= 2.74, NetworkManager-libnm >= 1.40, polkit, libnftables")

# Archive (.tar.zst and .tar.gz)
set(CPACK_ARCHIVE_THREADS 0)
set(CPACK_GENERATOR "DEB;TZST")

include(CPack)
