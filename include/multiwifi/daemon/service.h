#ifndef MULTIWIFI_DAEMON_SERVICE_H
#define MULTIWIFI_DAEMON_SERVICE_H

#include <gio/gio.h>
#include "multiwifi/core/wifi_inventory.h"
#include "multiwifi/core/path_monitor.h"
#include "multiwifi/core/operation.h"

G_BEGIN_DECLS

#define MULTIWIFI_TYPE_DAEMON_SERVICE (multiwifi_daemon_service_get_type())
G_DECLARE_FINAL_TYPE(MultiWifiDaemonService, multiwifi_daemon_service, MULTIWIFI, DAEMON_SERVICE, GObject)

/**
 * multiwifi_daemon_service_new:
 * @bus_type: G_BUS_TYPE_SYSTEM for production, G_BUS_TYPE_SESSION for testing
 * @enable_polkit: Whether to enforce Polkit authorization on mutating calls
 *
 * Creates a new D-Bus service instance for MultiWiFi.
 *
 * Returns: (transfer full): Newly allocated MultiWifiDaemonService.
 */
MultiWifiDaemonService *multiwifi_daemon_service_new(GBusType bus_type, gboolean enable_polkit);

/**
 * multiwifi_daemon_service_start:
 * @self: Daemon service
 * @error: (out)(optional): Return location for GError
 *
 * Connects to the bus, acquires bus name "io.github.multiwifi.Daemon", and registers
 * the object path "/io/github/multiwifi/Daemon".
 *
 * Returns: %TRUE if service started successfully, %FALSE on error.
 */
gboolean multiwifi_daemon_service_start(MultiWifiDaemonService *self, GError **error);

/**
 * multiwifi_daemon_service_stop:
 * @self: Daemon service
 *
 * Unregisters D-Bus object and releases bus name.
 */
void multiwifi_daemon_service_stop(MultiWifiDaemonService *self);

/**
 * multiwifi_daemon_service_get_connection:
 * @self: Daemon service
 *
 * Returns: (transfer none)(nullable): Active GDBusConnection.
 */
GDBusConnection *multiwifi_daemon_service_get_connection(MultiWifiDaemonService *self);

G_END_DECLS

#endif /* MULTIWIFI_DAEMON_SERVICE_H */
