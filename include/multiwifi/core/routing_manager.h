#ifndef MULTIWIFI_ROUTING_MANAGER_H
#define MULTIWIFI_ROUTING_MANAGER_H

#include <glib-object.h>
#include "multiwifi/core/operation.h"
#include "multiwifi/core/journal.h"
#include "multiwifi/core/wifi_device.h"

G_BEGIN_DECLS

#define MULTIWIFI_BASE_TABLE_ID   10000
#define MULTIWIFI_BASE_RULE_PREF  20000
#define MULTIWIFI_BASE_FWMARK     0x1000
#define MULTIWIFI_DEFAULT_FWMASK  0xff00

typedef struct {
    gchar *ifname;
    uint32_t ifindex;
    gchar *ipv4_address;
    gchar *gateway;
    uint32_t table_id;
    uint32_t src_rule_pref;
    uint32_t fwmark_rule_pref;
    uint32_t fwmark;
    uint32_t fwmask;
    uint32_t metric;
} MultiWifiUplinkRouteConfig;

typedef struct {
    GPtrArray *uplinks; /* MultiWifiUplinkRouteConfig* */
} MultiWifiRoutePlan;

/**
 * multiwifi_route_plan_new:
 *
 * Allocates a new empty routing plan.
 *
 * Returns: (transfer full): Newly created MultiWifiRoutePlan.
 */
MultiWifiRoutePlan *multiwifi_route_plan_new(void);

/**
 * multiwifi_route_plan_add_uplink:
 * @plan: Route plan
 * @ifname: Interface name
 * @ifindex: Interface index
 * @ipv4_addr: Local IPv4 address
 * @gateway: IPv4 gateway
 * @index: Zero-based uplink sequence index
 *
 * Adds and computes table, rule preference, and fwmark values for the uplink.
 */
void multiwifi_route_plan_add_uplink(MultiWifiRoutePlan *plan,
                                     const gchar *ifname,
                                     uint32_t ifindex,
                                     const gchar *ipv4_addr,
                                     const gchar *gateway,
                                     uint32_t index);

/**
 * multiwifi_route_plan_free:
 * @plan: (transfer full)(nullable): Plan to free
 */
void multiwifi_route_plan_free(MultiWifiRoutePlan *plan);

#define MULTIWIFI_TYPE_ROUTING_MANAGER (multiwifi_routing_manager_get_type())
G_DECLARE_FINAL_TYPE(MultiWifiRoutingManager, multiwifi_routing_manager, MULTIWIFI, ROUTING_MANAGER, GObject)

/**
 * multiwifi_routing_manager_new:
 * @journal_dir: (nullable): Directory to store recovery journals or %NULL for default
 *
 * Creates a new routing transaction manager.
 *
 * Returns: (transfer full): A new MultiWifiRoutingManager object.
 */
MultiWifiRoutingManager *multiwifi_routing_manager_new(const gchar *journal_dir);

/**
 * multiwifi_routing_manager_check_conflicts:
 * @self: Routing manager
 * @plan: Proposed route plan
 * @error: (out)(optional): Return location for GError
 *
 * Inspects kernel state and validates that proposed tables and rules don't conflict.
 *
 * Returns: %TRUE if safe to apply, %FALSE if conflicts or invalid inputs detected.
 */
gboolean multiwifi_routing_manager_check_conflicts(MultiWifiRoutingManager *self,
                                                   const MultiWifiRoutePlan *plan,
                                                   GError **error);

/**
 * multiwifi_routing_manager_apply_plan:
 * @self: Routing manager
 * @op_id: Operation UUID
 * @plan: Routing plan to execute
 * @op_mgr: (nullable): Operation manager for progress tracking or %NULL
 * @error: (out)(optional): Return location for GError
 *
 * Executes full transaction: JOURNAL -> APPLY -> VERIFY -> COMMIT.
 * On error, automatically rolls back all applied objects.
 *
 * Returns: %TRUE on complete success, %FALSE on failure.
 */
gboolean multiwifi_routing_manager_apply_plan(MultiWifiRoutingManager *self,
                                              const gchar *op_id,
                                              const MultiWifiRoutePlan *plan,
                                              MultiWifiOpManager *op_mgr,
                                              GError **error);

/**
 * multiwifi_routing_manager_teardown:
 * @self: Routing manager
 * @op_id: Operation UUID
 * @op_mgr: (nullable): Operation manager or %NULL
 * @error: (out)(optional): Return location for GError
 *
 * Cleanly removes all MultiWiFi-owned policy rules and dedicated routing tables.
 *
 * Returns: %TRUE on success, %FALSE on failure.
 */
gboolean multiwifi_routing_manager_teardown(MultiWifiRoutingManager *self,
                                            const gchar *op_id,
                                            MultiWifiOpManager *op_mgr,
                                            GError **error);

/**
 * multiwifi_routing_manager_recover:
 * @self: Routing manager
 * @error: (out)(optional): Return location for GError
 *
 * Scans journal directory for uncommitted transaction journals from crashes
 * and reconciles kernel state by rolling them back.
 *
 * Returns: %TRUE on successful reconciliation, %FALSE on error.
 */
gboolean multiwifi_routing_manager_recover(MultiWifiRoutingManager *self,
                                           GError **error);

/**
 * multiwifi_routing_manager_is_active:
 * @self: Routing manager
 *
 * Returns: %TRUE if managed policy routing tables are currently active.
 */
gboolean multiwifi_routing_manager_is_active(MultiWifiRoutingManager *self);

/**
 * multiwifi_routing_manager_get_fwmark:
 * @self: Routing manager
 * @ifname: Interface name to check
 *
 * Returns the fwmark assigned to @ifname in the active plan, or 0 if inactive or not found.
 */
uint32_t multiwifi_routing_manager_get_fwmark(MultiWifiRoutingManager *self, const gchar *ifname);

G_END_DECLS

#endif /* MULTIWIFI_ROUTING_MANAGER_H */
