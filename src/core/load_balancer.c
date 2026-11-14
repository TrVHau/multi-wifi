#include "multiwifi/core/load_balancer.h"
#include <string.h>

struct _MultiWifiScheduler {
    MultiWifiStrategy strategy;
    GPtrArray *entries; /* MultiWifiSchedulerEntry* */
    GMutex mutex;
    gint current_idx;
    guint current_weight;
    guint max_weight;
};

static void scheduler_entry_free(MultiWifiSchedulerEntry *entry)
{
    if (entry == NULL) {
        return;
    }
    g_free(entry->device_id);
    g_free(entry->ifname);
    g_free(entry);
}

MultiWifiScheduler *multiwifi_scheduler_new(MultiWifiStrategy strategy)
{
    MultiWifiScheduler *self = g_new0(MultiWifiScheduler, 1);
    self->strategy = strategy;
    self->entries = g_ptr_array_new_with_free_func((GDestroyNotify)scheduler_entry_free);
    g_mutex_init(&self->mutex);
    self->current_idx = -1;
    self->current_weight = 0;
    self->max_weight = 0;
    return self;
}

void multiwifi_scheduler_free(MultiWifiScheduler *self)
{
    if (self == NULL) {
        return;
    }
    g_mutex_clear(&self->mutex);
    if (self->entries != NULL) {
        g_ptr_array_unref(self->entries);
    }
    g_free(self);
}

static MultiWifiSchedulerEntry *find_entry_locked(MultiWifiScheduler *self, const gchar *id)
{
    if (self == NULL || id == NULL) {
        return NULL;
    }
    for (guint i = 0; i < self->entries->len; i++) {
        MultiWifiSchedulerEntry *e = g_ptr_array_index(self->entries, i);
        if (g_strcmp0(e->device_id, id) == 0 || g_strcmp0(e->ifname, id) == 0) {
            return e;
        }
    }
    return NULL;
}

static guint calculate_effective_weight(guint weight, gboolean is_eligible, guint loss_percent)
{
    if (!is_eligible || weight == 0) {
        return 0;
    }

    if (loss_percent >= 50) {
        return (weight >= 4) ? (weight / 4) : 1;
    } else if (loss_percent >= 20) {
        return (weight >= 2) ? (weight / 2) : 1;
    }

    return weight;
}

static void update_max_weight_locked(MultiWifiScheduler *self)
{
    self->max_weight = 0;
    for (guint i = 0; i < self->entries->len; i++) {
        MultiWifiSchedulerEntry *e = g_ptr_array_index(self->entries, i);
        if (e->effective_weight > self->max_weight) {
            self->max_weight = e->effective_weight;
        }
    }
}

void multiwifi_scheduler_add_or_update(MultiWifiScheduler *self,
                                       const gchar *device_id,
                                       const gchar *ifname,
                                       uint32_t fwmark,
                                       uint32_t weight,
                                       gboolean is_eligible,
                                       gdouble rtt_ms,
                                       guint loss_percent)
{
    if (self == NULL || device_id == NULL) {
        return;
    }

    g_mutex_lock(&self->mutex);

    MultiWifiSchedulerEntry *e = find_entry_locked(self, device_id);
    if (e == NULL) {
        e = g_new0(MultiWifiSchedulerEntry, 1);
        e->device_id = g_strdup(device_id);
        e->ifname = g_strdup(ifname ? ifname : device_id);
        e->fwmark = fwmark;
        e->assigned_flows = 0;
        g_ptr_array_add(self->entries, e);
    }

    if (weight > 0) {
        e->weight = (weight > 1000) ? 1000 : weight;
    } else if (e->weight == 0) {
        e->weight = 100;
    }
    e->is_eligible = is_eligible;
    e->rtt_ms = rtt_ms;
    e->loss_percent = (loss_percent > 100) ? 100 : loss_percent;
    e->effective_weight = calculate_effective_weight(e->weight, is_eligible, e->loss_percent);

    if (fwmark > 0) {
        e->fwmark = fwmark;
    }

    update_max_weight_locked(self);
    g_mutex_unlock(&self->mutex);
}

gboolean multiwifi_scheduler_set_weight(MultiWifiScheduler *self,
                                        const gchar *device_id,
                                        uint32_t weight)
{
    if (self == NULL || device_id == NULL) {
        return FALSE;
    }

    g_mutex_lock(&self->mutex);
    MultiWifiSchedulerEntry *e = find_entry_locked(self, device_id);
    if (e == NULL) {
        g_mutex_unlock(&self->mutex);
        return FALSE;
    }

    e->weight = (weight == 0) ? 1 : ((weight > 1000) ? 1000 : weight);
    e->effective_weight = calculate_effective_weight(e->weight, e->is_eligible, e->loss_percent);
    update_max_weight_locked(self);

    g_mutex_unlock(&self->mutex);
    return TRUE;
}

static guint gcd(guint a, guint b)
{
    while (b != 0) {
        guint t = b;
        b = a % b;
        a = t;
    }
    return a;
}

uint32_t multiwifi_scheduler_select_mark(MultiWifiScheduler *self)
{
    if (self == NULL) {
        return 0;
    }

    g_mutex_lock(&self->mutex);

    if (self->entries->len == 0 || self->max_weight == 0) {
        g_mutex_unlock(&self->mutex);
        return 0;
    }

    /* Compute GCD of effective weights */
    guint weight_gcd = 0;
    for (guint i = 0; i < self->entries->len; i++) {
        MultiWifiSchedulerEntry *e = g_ptr_array_index(self->entries, i);
        if (e->effective_weight > 0) {
            weight_gcd = (weight_gcd == 0) ? e->effective_weight : gcd(weight_gcd, e->effective_weight);
        }
    }
    if (weight_gcd == 0) {
        weight_gcd = 1;
    }

    /* Weighted round-robin loop */
    guint attempts = self->entries->len * (self->max_weight + 1);
    for (guint count = 0; count < attempts; count++) {
        self->current_idx = (self->current_idx + 1) % (gint)self->entries->len;
        if (self->current_idx == 0) {
            if (self->current_weight <= weight_gcd) {
                self->current_weight = self->max_weight;
                if (self->current_weight == 0) {
                    g_mutex_unlock(&self->mutex);
                    return 0;
                }
            } else {
                self->current_weight -= weight_gcd;
            }
        }

        MultiWifiSchedulerEntry *cand = g_ptr_array_index(self->entries, (guint)self->current_idx);
        if (cand->effective_weight >= self->current_weight) {
            cand->assigned_flows++;
            uint32_t mark = cand->fwmark;
            g_mutex_unlock(&self->mutex);
            return mark;
        }
    }

    g_mutex_unlock(&self->mutex);
    return 0;
}

guint multiwifi_scheduler_get_distribution(MultiWifiScheduler *self,
                                           GArray **marks_out)
{
    if (marks_out == NULL) {
        return 0;
    }
    *marks_out = g_array_new(FALSE, FALSE, sizeof(uint32_t));

    if (self == NULL) {
        return 0;
    }

    g_mutex_lock(&self->mutex);

    if (self->entries->len == 0 || self->max_weight == 0) {
        g_mutex_unlock(&self->mutex);
        return 0;
    }

    /* Find GCD of all positive effective weights */
    guint weight_gcd = 0;
    for (guint i = 0; i < self->entries->len; i++) {
        MultiWifiSchedulerEntry *e = g_ptr_array_index(self->entries, i);
        if (e->effective_weight > 0) {
            weight_gcd = (weight_gcd == 0) ? e->effective_weight : gcd(weight_gcd, e->effective_weight);
        }
    }
    if (weight_gcd == 0) {
        weight_gcd = 1;
    }

    /* Cap total slots to 128 for firewall mapping performance */
    guint total_slots = 0;
    for (guint i = 0; i < self->entries->len; i++) {
        MultiWifiSchedulerEntry *e = g_ptr_array_index(self->entries, i);
        total_slots += (e->effective_weight / weight_gcd);
    }

    guint divisor = weight_gcd;
    if (total_slots > 128) {
        divisor = weight_gcd * ((total_slots / 128) + 1);
    }

    for (guint i = 0; i < self->entries->len; i++) {
        MultiWifiSchedulerEntry *e = g_ptr_array_index(self->entries, i);
        guint slots = e->effective_weight / divisor;
        if (e->effective_weight > 0 && slots == 0) {
            slots = 1;
        }
        for (guint s = 0; s < slots; s++) {
            g_array_append_val(*marks_out, e->fwmark);
        }
    }

    guint count = (*marks_out)->len;
    g_mutex_unlock(&self->mutex);
    return count;
}

guint multiwifi_scheduler_get_eligible_count(MultiWifiScheduler *self)
{
    if (self == NULL) {
        return 0;
    }
    g_mutex_lock(&self->mutex);
    guint count = 0;
    for (guint i = 0; i < self->entries->len; i++) {
        MultiWifiSchedulerEntry *e = g_ptr_array_index(self->entries, i);
        if (e->effective_weight > 0) {
            count++;
        }
    }
    g_mutex_unlock(&self->mutex);
    return count;
}

void multiwifi_scheduler_reset(MultiWifiScheduler *self)
{
    if (self == NULL) {
        return;
    }
    g_mutex_lock(&self->mutex);
    g_ptr_array_set_size(self->entries, 0);
    self->current_idx = -1;
    self->current_weight = 0;
    self->max_weight = 0;
    g_mutex_unlock(&self->mutex);
}
