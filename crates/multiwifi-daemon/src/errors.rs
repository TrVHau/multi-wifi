#![deny(unsafe_code)]

use zbus::DBusError;

pub const MULTIWIFI_ERROR_NOT_AUTHORIZED: &str = "io.github.multiwifi.Error.NotAuthorized";
pub const MULTIWIFI_ERROR_INVALID_ARGUMENT: &str = "io.github.multiwifi.Error.InvalidArgument";
pub const MULTIWIFI_ERROR_CONFLICT: &str = "io.github.multiwifi.Error.Conflict";
pub const MULTIWIFI_ERROR_NETWORK_FAILURE: &str = "io.github.multiwifi.Error.NetworkFailure";
pub const MULTIWIFI_ERROR_ROLLBACK_FAILED: &str = "io.github.multiwifi.Error.RollbackFailed";
pub const MULTIWIFI_ERROR_BUSY: &str = "io.github.multiwifi.Error.Busy";

/// Custom D-Bus errors matching `io.github.multiwifi.Error.*` from `dbus_defs.h`.
#[derive(Debug, DBusError)]
#[zbus(prefix = "io.github.multiwifi.Error")]
pub enum DaemonError {
    NotAuthorized(String),
    InvalidArgument(String),
    Unsupported(String),
    Conflict(String),
    CapabilityMissing(String),
    NetworkFailure(String),
    RollbackFailed(String),
    Busy(String),

    #[zbus(error)]
    Zbus(zbus::Error),
}
