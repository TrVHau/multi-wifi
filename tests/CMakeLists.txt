add_executable(test_config test_config.c)
target_link_libraries(test_config PRIVATE multiwifi-core)
add_test(NAME test_config COMMAND test_config)

add_executable(test_capabilities test_capabilities.c)
target_link_libraries(test_capabilities PRIVATE multiwifi-core)
add_test(NAME test_capabilities COMMAND test_capabilities)

add_executable(test_wifi_device test_wifi_device.c)
target_link_libraries(test_wifi_device PRIVATE multiwifi-core)
add_test(NAME test_wifi_device COMMAND test_wifi_device)

add_executable(test_path_monitor test_path_monitor.c)
target_link_libraries(test_path_monitor PRIVATE multiwifi-core)
add_test(NAME test_path_monitor COMMAND test_path_monitor)

add_executable(test_dbus_daemon test_dbus_daemon.c)
target_link_libraries(test_dbus_daemon PRIVATE multiwifi-core)
add_test(NAME test_dbus_daemon COMMAND test_dbus_daemon)

add_executable(test_routing_manager test_routing_manager.c)
target_link_libraries(test_routing_manager PRIVATE multiwifi-core)
add_test(NAME test_routing_manager COMMAND test_routing_manager)

add_executable(test_load_balancer test_load_balancer.c)
target_link_libraries(test_load_balancer PRIVATE multiwifi-core)
add_test(NAME test_load_balancer COMMAND test_load_balancer)

add_executable(test_bonding test_bonding.c)
target_link_libraries(test_bonding PRIVATE multiwifi-core)
add_test(NAME test_bonding COMMAND test_bonding)

add_executable(test_packaging test_packaging.c)
target_compile_definitions(test_packaging PRIVATE
    MULTIWIFI_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
    MULTIWIFI_BUILD_DIR="${CMAKE_BINARY_DIR}"
)
target_link_libraries(test_packaging PRIVATE multiwifi-core)
add_test(NAME test_packaging COMMAND test_packaging)

if(MULTIWIFI_RUST_DAEMON)
    add_executable(test_rust_parity test_rust_parity.c)
    add_dependencies(test_rust_parity multiwifi-daemon-rust)
    target_compile_definitions(test_rust_parity PRIVATE
        MULTIWIFI_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
        MULTIWIFI_DAEMON_BIN="${CMAKE_BINARY_DIR}/multiwifi-daemon"
    )
    target_link_libraries(test_rust_parity PRIVATE multiwifi-core)
    add_test(NAME test_rust_parity COMMAND test_rust_parity)
endif()
