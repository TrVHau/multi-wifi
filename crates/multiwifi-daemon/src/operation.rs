#![deny(unsafe_code)]

use std::collections::{HashMap, VecDeque};
use std::sync::{Mutex, PoisonError};
use std::time::Instant;
use uuid::Uuid;
use zbus::{OwnedVariant, Variant};

const MAX_OPERATION_HISTORY: usize = 50;

#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
pub enum OpState {
    #[default]
    Queued,
    Validating,
    Applying,
    Verifying,
    Succeeded,
    RollingBack,
    Failed,
}

impl OpState {
    #[must_use]
    pub const fn as_str(self) -> &'static str {
        match self {
            Self::Queued => "QUEUED",
            Self::Validating => "VALIDATING",
            Self::Applying => "APPLYING",
            Self::Verifying => "VERIFYING",
            Self::Succeeded => "SUCCEEDED",
            Self::RollingBack => "ROLLING_BACK",
            Self::Failed => "FAILED",
        }
    }
}

#[derive(Debug, Clone)]
pub struct OperationRecord {
    pub operation_id: String,
    pub action: String,
    pub state: OpState,
    pub percent: u32,
    pub message: String,
    pub success: bool,
    pub result: HashMap<String, OwnedVariant>,
    pub created_at: Instant,
    pub finished_at: Option<Instant>,
}

/// Thread-safe tracker for asynchronous and synchronous daemon operation lifecycles.
#[derive(Debug, Default)]
pub struct OpManager {
    operations: Mutex<VecDeque<OperationRecord>>,
}

impl OpManager {
    #[must_use]
    pub fn new() -> Self {
        Self::default()
    }

    #[must_use]
    pub fn create_op(&self, action: &str) -> String {
        let op_id = Uuid::new_v4().to_string();
        let record = OperationRecord {
            operation_id: op_id.clone(),
            action: if action.is_empty() {
                "unknown".to_owned()
            } else {
                action.to_owned()
            },
            state: OpState::Queued,
            percent: 0,
            message: "Operation queued".to_owned(),
            success: false,
            result: HashMap::new(),
            created_at: Instant::now(),
            finished_at: None,
        };

        let mut ops = self
            .operations
            .lock()
            .unwrap_or_else(PoisonError::into_inner);
        while ops.len() >= MAX_OPERATION_HISTORY {
            ops.pop_front();
        }
        ops.push_back(record);
        op_id
    }

    pub fn update(&self, op_id: &str, state: OpState, percent: u32, message: &str) -> bool {
        let mut ops = self
            .operations
            .lock()
            .unwrap_or_else(PoisonError::into_inner);
        let Some(op) = ops.iter_mut().find(|op| op.operation_id == op_id) else {
            return false;
        };
        op.state = state;
        op.percent = percent.min(100);
        message.clone_into(&mut op.message);
        true
    }

    pub fn finish(
        &self,
        op_id: &str,
        success: bool,
        result: Option<HashMap<String, OwnedVariant>>,
        message: Option<&str>,
    ) -> bool {
        let mut ops = self
            .operations
            .lock()
            .unwrap_or_else(PoisonError::into_inner);
        let Some(op) = ops.iter_mut().find(|op| op.operation_id == op_id) else {
            return false;
        };
        op.state = if success {
            OpState::Succeeded
        } else {
            OpState::Failed
        };
        op.percent = 100;
        op.success = success;
        op.finished_at = Some(Instant::now());
        message
            .unwrap_or(if success { "Succeeded" } else { "Failed" })
            .clone_into(&mut op.message);
        op.result = result.unwrap_or_default();
        true
    }

    #[must_use]
    pub fn find(&self, op_id: &str) -> Option<OperationRecord> {
        let ops = self
            .operations
            .lock()
            .unwrap_or_else(PoisonError::into_inner);
        ops.iter().find(|op| op.operation_id == op_id).cloned()
    }

    #[must_use]
    pub fn to_dbus_dict(&self, op_id: &str) -> Option<HashMap<String, OwnedVariant>> {
        let op = self.find(op_id)?;
        let mut map = HashMap::new();
        insert_val(&mut map, "operation_id", op.operation_id);
        insert_val(&mut map, "action", op.action);
        insert_val(&mut map, "state", op.state.as_str().to_owned());
        insert_val(&mut map, "percent", op.percent);
        insert_val(&mut map, "message", op.message);
        insert_val(&mut map, "success", op.success);
        insert_val(&mut map, "result", op.result);
        Some(map)
    }
}

pub(crate) fn insert_val<'a, T>(
    map: &mut HashMap<String, OwnedVariant>,
    key: &str,
    val: T,
) where
    T: Into<Variant<'a>>,
{
    if let Ok(owned) = val.into().try_to_owned() {
        map.insert(key.to_owned(), owned);
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn operation_lifecycle_and_pruning() {
        let mgr = OpManager::new();
        let op_id = mgr.create_op("TestOperation");
        assert!(op_id.len() > 10);

        let op = mgr.find(&op_id).expect("created");
        assert_eq!(op.action, "TestOperation");
        assert_eq!(op.state, OpState::Queued);
        assert_eq!(op.percent, 0);
        assert!(!op.success);

        assert!(mgr.update(&op_id, OpState::Validating, 25, "Validating"));
        let op2 = mgr.find(&op_id).expect("updated");
        assert_eq!(op2.state, OpState::Validating);
        assert_eq!(op2.percent, 25);

        let mut res = HashMap::new();
        insert_val(&mut res, "detail", "ok");
        assert!(mgr.finish(&op_id, true, Some(res), Some("Completed")));

        let dict = mgr.to_dbus_dict(&op_id).expect("dict");
        assert!(dict.contains_key("operation_id"));
        assert!(dict.contains_key("result"));

        // Exceeding MAX_OPERATION_HISTORY prunes oldest entries without panic.
        for i in 0..60 {
            let _ = mgr.create_op(&format!("Bulk-{i}"));
        }
        assert!(mgr.find(&op_id).is_none());
    }
}
