add_executable(test_capabilities test_capabilities.c)
target_link_libraries(test_capabilities PRIVATE multiwifi-gui-support)
add_test(NAME test_capabilities COMMAND test_capabilities)

add_executable(test_wifi_device test_wifi_device.c)
target_link_libraries(test_wifi_device PRIVATE multiwifi-gui-support)
add_test(NAME test_wifi_device COMMAND test_wifi_device)

add_executable(test_path_monitor test_path_monitor.c)
target_link_libraries(test_path_monitor PRIVATE multiwifi-gui-support)
add_test(NAME test_path_monitor COMMAND test_path_monitor)

add_executable(test_packaging test_packaging.c)
target_compile_definitions(test_packaging PRIVATE
    MULTIWIFI_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
    MULTIWIFI_BUILD_DIR="${CMAKE_BINARY_DIR}"
)
target_link_libraries(test_packaging PRIVATE
    multiwifi-gui-support
    PkgConfig::NFTABLES
)
add_test(NAME test_packaging COMMAND test_packaging)

if(MULTIWIFI_BUILD_DAEMON)
    add_executable(test_rust_parity test_rust_parity.c)
    add_dependencies(test_rust_parity multiwifi-daemon-rust)
    target_compile_definitions(test_rust_parity PRIVATE
        MULTIWIFI_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
        MULTIWIFI_DAEMON_BIN="${CMAKE_BINARY_DIR}/multiwifi-daemon"
    )
    target_link_libraries(test_rust_parity PRIVATE multiwifi-gui-support)
    add_test(NAME test_rust_parity COMMAND test_rust_parity)
endif()