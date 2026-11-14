#ifndef MULTIWIFI_LOAD_BALANCER_H
#define MULTIWIFI_LOAD_BALANCER_H

#include <glib.h>
#include <stdint.h>
#include <stdbool.h>
#include "multiwifi/core/config.h"

G_BEGIN_DECLS

typedef struct {
    gchar *device_id;
    gchar *ifname;
    uint32_t fwmark;
    uint32_t weight;            /* Base configured weight (1 - 1000) */
    uint32_t effective_weight;   /* Weight penalized by health/loss */
    gboolean is_eligible;        /* Upstream reachability eligibility */
    gdouble rtt_ms;
    guint loss_percent;
    uint64_t assigned_flows;
} MultiWifiSchedulerEntry;

typedef struct _MultiWifiScheduler MultiWifiScheduler;

/**
 * multiwifi_scheduler_new:
 * @strategy: Load balancing strategy (e.g. MULTIWIFI_STRATEGY_WEIGHTED_ROUND_ROBIN)
 *
 * Creates a new flow affinity scheduler.
 *
 * Returns: (transfer full): Newly created MultiWifiScheduler.
 */
MultiWifiScheduler *multiwifi_scheduler_new(MultiWifiStrategy strategy);

/**
 * multiwifi_scheduler_free:
 * @self: (transfer full)(nullable): Scheduler to free
 */
void multiwifi_scheduler_free(MultiWifiScheduler *self);

/**
 * multiwifi_scheduler_add_or_update:
 * @self: Scheduler instance
 * @device_id: Unique device identifier or interface name
 * @ifname: Network interface name
 * @fwmark: Routing firewall mark
 * @weight: Configured weight (1 - 1000)
 * @is_eligible: Whether path is currently eligible from PathMonitor
 * @rtt_ms: Measured RTT in milliseconds
 * @loss_percent: Packet loss percentage (0 - 100)
 */
void multiwifi_scheduler_add_or_update(MultiWifiScheduler *self,
                                       const gchar *device_id,
                                       const gchar *ifname,
                                       uint32_t fwmark,
                                       uint32_t weight,
                                       gboolean is_eligible,
                                       gdouble rtt_ms,
                                       guint loss_percent);

/**
 * multiwifi_scheduler_set_weight:
 * @self: Scheduler instance
 * @device_id: Device identifier
 * @weight: New weight (1 - 1000)
 *
 * Returns: %TRUE if device found and weight updated, %FALSE otherwise.
 */
gboolean multiwifi_scheduler_set_weight(MultiWifiScheduler *self,
                                        const gchar *device_id,
                                        uint32_t weight);

/**
 * multiwifi_scheduler_select_mark:
 * @self: Scheduler instance
 *
 * Selects an uplink for a new flow according to weighted round-robin.
 *
 * Returns: Selected fwmark, or 0 if no eligible uplinks available.
 */
uint32_t multiwifi_scheduler_select_mark(MultiWifiScheduler *self);

/**
 * multiwifi_scheduler_get_distribution:
 * @self: Scheduler instance
 * @marks_out: (out)(transfer full): GArray of uint32_t fwmarks for mapping
 *
 * Builds a discrete distribution vector of fwmarks proportional to effective weights.
 *
 * Returns: Number of slots in the distribution.
 */
guint multiwifi_scheduler_get_distribution(MultiWifiScheduler *self,
                                           GArray **marks_out);

/**
 * multiwifi_scheduler_get_eligible_count:
 * @self: Scheduler instance
 *
 * Returns: Number of currently eligible uplinks.
 */
guint multiwifi_scheduler_get_eligible_count(MultiWifiScheduler *self);

/**
 * multiwifi_scheduler_reset:
 * @self: Scheduler instance
 *
 * Clears all registered uplinks from the scheduler.
 */
void multiwifi_scheduler_reset(MultiWifiScheduler *self);

G_END_DECLS

#endif /* MULTIWIFI_LOAD_BALANCER_H */
