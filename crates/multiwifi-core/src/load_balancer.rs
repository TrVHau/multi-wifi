use crate::config::LoadBalanceStrategy;
use std::sync::Mutex;

const MAX_WEIGHT: u32 = 1000;
const DEFAULT_WEIGHT: u32 = 100;
const MAX_DISTRIBUTION_SLOTS: u32 = 128;

#[derive(Debug, Clone)]
pub struct SchedulerEntry {
    pub device_id: String,
    pub ifname: String,
    pub fwmark: u32,
    pub weight: u32,
    pub effective_weight: u32,
    pub is_eligible: bool,
    pub rtt_ms: f64,
    pub loss_percent: u32,
    pub assigned_flows: u64,
}

#[derive(Debug)]
struct SchedulerInner {
    strategy: LoadBalanceStrategy,
    entries: Vec<SchedulerEntry>,
    current_idx: Option<usize>,
    current_weight: u32,
    max_weight: u32,
}

/// Thread-safe Weighted Round-Robin uplink scheduler matching `MultiWifiScheduler`.
#[derive(Debug)]
pub struct Scheduler {
    inner: Mutex<SchedulerInner>,
}

fn calculate_effective_weight(weight: u32, is_eligible: bool, loss_percent: u32) -> u32 {
    if !is_eligible || weight == 0 {
        return 0;
    }
    if loss_percent >= 50 {
        (weight / 4).max(1)
    } else if loss_percent >= 20 {
        (weight / 2).max(1)
    } else {
        weight
    }
}

const fn gcd(mut a: u32, mut b: u32) -> u32 {
    while b != 0 {
        let t = b;
        b = a % b;
        a = t;
    }
    a
}

impl SchedulerInner {
    fn update_max_weight(&mut self) {
        self.max_weight = self
            .entries
            .iter()
            .map(|e| e.effective_weight)
            .max()
            .unwrap_or(0);
    }

    fn effective_gcd(&self) -> u32 {
        let mut g = 0u32;
        for e in &self.entries {
            if e.effective_weight > 0 {
                g = if g == 0 {
                    e.effective_weight
                } else {
                    gcd(g, e.effective_weight)
                };
            }
        }
        if g == 0 {
            1
        } else {
            g
        }
    }
}

/// Input metrics for registering or updating an uplink in `Scheduler`.
#[derive(Debug, Clone, Copy)]
pub struct UplinkState<'a> {
    pub device_id: &'a str,
    pub ifname: &'a str,
    pub fwmark: u32,
    pub weight: u32,
    pub is_eligible: bool,
    pub rtt_ms: f64,
    pub loss_percent: u32,
}

impl Scheduler {
    #[must_use]
    pub fn new(strategy: LoadBalanceStrategy) -> Self {
        Self {
            inner: Mutex::new(SchedulerInner {
                strategy,
                entries: Vec::new(),
                current_idx: None,
                current_weight: 0,
                max_weight: 0,
            }),
        }
    }

    #[must_use]
    pub fn strategy(&self) -> LoadBalanceStrategy {
        let guard = self
            .inner
            .lock()
            .unwrap_or_else(std::sync::PoisonError::into_inner);
        guard.strategy
    }

    pub fn add_or_update(&self, spec: &UplinkState<'_>) {
        if spec.device_id.is_empty() {
            return;
        }
        let mut inner = self
            .inner
            .lock()
            .unwrap_or_else(std::sync::PoisonError::into_inner);

        let clamped_loss = spec.loss_percent.min(100);
        if let Some(existing) = inner
            .entries
            .iter_mut()
            .find(|e| e.device_id == spec.device_id || e.ifname == spec.device_id)
        {
            if spec.weight > 0 {
                existing.weight = spec.weight.min(MAX_WEIGHT);
            } else if existing.weight == 0 {
                existing.weight = DEFAULT_WEIGHT;
            }
            if spec.fwmark > 0 {
                existing.fwmark = spec.fwmark;
            }
            existing.is_eligible = spec.is_eligible;
            existing.rtt_ms = spec.rtt_ms;
            existing.loss_percent = clamped_loss;
            existing.effective_weight =
                calculate_effective_weight(existing.weight, spec.is_eligible, clamped_loss);
        } else {
            let base_weight = if spec.weight > 0 {
                spec.weight.min(MAX_WEIGHT)
            } else {
                DEFAULT_WEIGHT
            };
            let effective = calculate_effective_weight(base_weight, spec.is_eligible, clamped_loss);
            let iface = if spec.ifname.is_empty() {
                spec.device_id.to_owned()
            } else {
                spec.ifname.to_owned()
            };
            inner.entries.push(SchedulerEntry {
                device_id: spec.device_id.to_owned(),
                ifname: iface,
                fwmark: spec.fwmark,
                weight: base_weight,
                effective_weight: effective,
                is_eligible: spec.is_eligible,
                rtt_ms: spec.rtt_ms,
                loss_percent: clamped_loss,
                assigned_flows: 0,
            });
        }

        inner.update_max_weight();
    }

    pub fn set_weight(&self, device_id: &str, weight: u32) -> bool {
        if device_id.is_empty() {
            return false;
        }
        let mut inner = self
            .inner
            .lock()
            .unwrap_or_else(std::sync::PoisonError::into_inner);

        let clamped = weight.clamp(1, MAX_WEIGHT);
        let Some(entry) = inner
            .entries
            .iter_mut()
            .find(|e| e.device_id == device_id || e.ifname == device_id)
        else {
            return false;
        };

        entry.weight = clamped;
        entry.effective_weight =
            calculate_effective_weight(entry.weight, entry.is_eligible, entry.loss_percent);
        inner.update_max_weight();
        true
    }

    /// Selects the next `fwmark` using GCD-normalized Weighted Round-Robin.
    /// Returns `0` if no eligible uplink exists.
    pub fn select_mark(&self) -> u32 {
        let mut inner = self
            .inner
            .lock()
            .unwrap_or_else(std::sync::PoisonError::into_inner);

        if inner.entries.is_empty() || inner.max_weight == 0 {
            return 0;
        }

        let weight_gcd = inner.effective_gcd();
        let len = inner.entries.len();
        let attempts = len * (inner.max_weight as usize + 1);

        for _ in 0..attempts {
            let next_idx = match inner.current_idx {
                None => 0,
                Some(idx) => (idx + 1) % len,
            };
            inner.current_idx = Some(next_idx);

            if next_idx == 0 {
                if inner.current_weight <= weight_gcd {
                    inner.current_weight = inner.max_weight;
                    if inner.current_weight == 0 {
                        return 0;
                    }
                } else {
                    inner.current_weight -= weight_gcd;
                }
            }

            let cur_weight = inner.current_weight;
            let cand = &mut inner.entries[next_idx];
            if cand.effective_weight >= cur_weight {
                cand.assigned_flows += 1;
                return cand.fwmark;
            }
        }

        0
    }

    /// Builds a discrete distribution vector of `fwmark`s proportional to effective weights,
    /// capped at `<= 128` slots for netfilter map efficiency.
    #[must_use]
    pub fn distribution(&self) -> Vec<u32> {
        let inner = self
            .inner
            .lock()
            .unwrap_or_else(std::sync::PoisonError::into_inner);

        if inner.entries.is_empty() || inner.max_weight == 0 {
            return Vec::new();
        }

        let weight_gcd = inner.effective_gcd();
        let total_slots: u32 = inner
            .entries
            .iter()
            .map(|e| e.effective_weight / weight_gcd)
            .sum();

        let divisor = if total_slots > MAX_DISTRIBUTION_SLOTS {
            weight_gcd * ((total_slots / MAX_DISTRIBUTION_SLOTS) + 1)
        } else {
            weight_gcd
        };

        let mut marks = Vec::with_capacity((total_slots.min(MAX_DISTRIBUTION_SLOTS)) as usize);
        for entry in &inner.entries {
            let mut slots = entry.effective_weight / divisor;
            if entry.effective_weight > 0 && slots == 0 {
                slots = 1;
            }
            for _ in 0..slots {
                marks.push(entry.fwmark);
            }
        }
        marks
    }

    #[must_use]
    pub fn eligible_count(&self) -> usize {
        let inner = self
            .inner
            .lock()
            .unwrap_or_else(std::sync::PoisonError::into_inner);
        inner
            .entries
            .iter()
            .filter(|e| e.effective_weight > 0)
            .count()
    }

    #[must_use]
    pub fn entries(&self) -> Vec<SchedulerEntry> {
        let inner = self
            .inner
            .lock()
            .unwrap_or_else(std::sync::PoisonError::into_inner);
        inner.entries.clone()
    }

    pub fn reset(&self) {
        let mut inner = self
            .inner
            .lock()
            .unwrap_or_else(std::sync::PoisonError::into_inner);
        inner.entries.clear();
        inner.current_idx = None;
        inner.current_weight = 0;
        inner.max_weight = 0;
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::sync::Arc;
    use std::thread;

    fn uplink(
        id: &str,
        fwmark: u32,
        weight: u32,
        is_eligible: bool,
        loss_percent: u32,
    ) -> UplinkState<'_> {
        UplinkState {
            device_id: id,
            ifname: id,
            fwmark,
            weight,
            is_eligible,
            rtt_ms: 10.0,
            loss_percent,
        }
    }

    #[test]
    fn equal_weights_round_robin() {
        let sched = Scheduler::new(LoadBalanceStrategy::WeightedRoundRobin);
        sched.add_or_update(&uplink("wlp0s20f3", 0x1001, 100, true, 0));
        sched.add_or_update(&uplink("wlp0s20f0u1", 0x1002, 100, true, 0));

        let (mut count_a, mut count_b) = (0u32, 0u32);
        for _ in 0..100 {
            match sched.select_mark() {
                0x1001 => count_a += 1,
                0x1002 => count_b += 1,
                other => panic!("unexpected mark {other:#x}"),
            }
        }
        assert_eq!(count_a, 50);
        assert_eq!(count_b, 50);
    }

    #[test]
    fn unequal_weights_distribution() {
        let sched = Scheduler::new(LoadBalanceStrategy::WeightedRoundRobin);
        sched.add_or_update(&uplink("wlan0", 0x1000, 300, true, 0));
        sched.add_or_update(&uplink("wlan1", 0x2000, 100, true, 0));

        let marks = sched.distribution();
        assert_eq!(marks.len(), 4);
        assert_eq!(marks.iter().filter(|&&m| m == 0x1000).count(), 3);
        assert_eq!(marks.iter().filter(|&&m| m == 0x2000).count(), 1);

        let (mut count_a, mut count_b) = (0u32, 0u32);
        for _ in 0..400 {
            match sched.select_mark() {
                0x1000 => count_a += 1,
                0x2000 => count_b += 1,
                other => panic!("unexpected mark {other:#x}"),
            }
        }
        assert_eq!(count_a, 300);
        assert_eq!(count_b, 100);
    }

    #[test]
    fn health_penalty_and_failover() {
        let sched = Scheduler::new(LoadBalanceStrategy::WeightedRoundRobin);
        sched.add_or_update(&uplink("wlan0", 0x1001, 100, true, 0));
        sched.add_or_update(&uplink("wlan1", 0x1002, 100, true, 25));

        let entries = sched.entries();
        assert_eq!(entries[1].effective_weight, 50);

        sched.add_or_update(&uplink("wlan1", 0x1002, 100, false, 100));
        assert_eq!(sched.eligible_count(), 1);
        for _ in 0..20 {
            assert_eq!(sched.select_mark(), 0x1001);
        }

        sched.add_or_update(&uplink("wlan0", 0x1001, 100, false, 100));
        assert_eq!(sched.eligible_count(), 0);
        assert_eq!(sched.select_mark(), 0);
        assert_eq!(sched.distribution(), [] as [u32; 0]);
    }

    #[test]
    fn thread_concurrency_stress() {
        let sched = Arc::new(Scheduler::new(LoadBalanceStrategy::WeightedRoundRobin));
        sched.add_or_update(&uplink("wlp0s20f3", 0x1000, 100, true, 0));
        sched.add_or_update(&uplink("wlp0s20f0u1", 0x2000, 100, true, 0));

        let handles: Vec<_> = (0..4)
            .map(|_| {
                let s = Arc::clone(&sched);
                thread::spawn(move || {
                    for i in 0..1000u32 {
                        let mark = s.select_mark();
                        assert!(mark == 0x1000 || mark == 0x2000);
                        if i % 50 == 0 {
                            assert!(s.set_weight("wlp0s20f3", 100 + (i % 200)));
                        }
                    }
                })
            })
            .collect();

        for h in handles {
            h.join().expect("thread join");
        }
    }
}
