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
