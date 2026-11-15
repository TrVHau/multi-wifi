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
pkg_check_modules(NM REQUIRED IMPORTED_TARGET libnm>=1.40)
pkg_check_modules(POLKIT REQUIRED IMPORTED_TARGET polkit-gobject-1)
pkg_check_modules(NFTABLES REQUIRED IMPORTED_TARGET libnftables)

include_directories(
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

# Core library (pure logic, logging, capabilities, config parsing, Wi-Fi inventory, path monitor, D-Bus service, routing manager)
add_library(multiwifi-core STATIC
    src/core/logging.c
    src/core/capabilities.c
    src/core/config.c
    src/core/wifi_device.c
    src/core/wifi_inventory.c
    src/core/path_monitor.c
    src/core/operation.c
    src/core/journal.c
    src/core/netlink_route.c
    src/core/routing_manager.c
    src/core/load_balancer.c
    src/core/firewall.c
    src/core/bonding_protocol.c
    src/core/tun_device.c
    src/core/loop_prevention.c
    src/core/bonding_client.c
    src/daemon/service.c
)
target_link_libraries(multiwifi-core PUBLIC
    PkgConfig::GLIB
    PkgConfig::GIO
    PkgConfig::GIO_UNIX
    PkgConfig::NM
    PkgConfig::POLKIT
    PkgConfig::NFTABLES
    m
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
