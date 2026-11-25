#![deny(unsafe_code)]

use crate::errors::DaemonError;
use std::collections::HashMap;
use std::path::Path;
use zbus::message::Header;
use zbus::proxy;
use zbus::{Connection, Variant};

const POLKIT_ACTION_MODIFY: &str = "io.github.multiwifi.modify";
const POLKIT_POLICY_PATH: &str = "/usr/share/polkit-1/actions/io.github.multiwifi.policy";
const POLKIT_FLAG_ALLOW_USER_INTERACTION: u32 = 0x1;

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum BusKind {
    System,
    Session,
}

#[proxy(
    interface = "org.freedesktop.PolicyKit1.Authority",
    default_service = "org.freedesktop.PolicyKit1",
    default_path = "/org/freedesktop/PolicyKit1/Authority"
)]
trait PolkitAuthority {
    fn check_authorization(
        &self,
        subject: (&str, HashMap<&str, Variant<'_>>),
        action_id: &str,
        details: HashMap<&str, &str>,
        flags: u32,
        cancellation_id: &str,
    ) -> zbus::Result<(bool, bool, HashMap<String, String>)>;
}

#[derive(Debug, Clone)]
pub struct PolkitAuthorizer {
    enable_polkit: bool,
    bus_kind: BusKind,
}

impl PolkitAuthorizer {
    #[must_use]
    pub const fn new(enable_polkit: bool, bus_kind: BusKind) -> Self {
        Self {
            enable_polkit,
            bus_kind,
        }
    }

    #[must_use]
    pub const fn is_enforced(&self) -> bool {
        self.enable_polkit
    }

    pub async fn authorize(
        &self,
        conn: &Connection,
        header: &Header<'_>,
    ) -> Result<(), DaemonError> {
        if !self.enable_polkit {
            return Ok(());
        }

        if self.bus_kind != BusKind::System {
            return Err(DaemonError::NotAuthorized(
                "Polkit authorization requires the system bus".to_owned(),
            ));
        }

        let Some(sender) = header.sender() else {
            return Err(DaemonError::NotAuthorized(
                "Caller has no D-Bus sender identity".to_owned(),
            ));
        };

        let authority = PolkitAuthorityProxy::new(conn).await.map_err(|err| {
            DaemonError::NotAuthorized(format!("Polkit authority unavailable: {err}"))
        })?;

        let mut subject_details = HashMap::new();
        subject_details.insert("name", Variant::from(sender.as_str()));

        let res = authority
            .check_authorization(
                ("system-bus-name", subject_details),
                POLKIT_ACTION_MODIFY,
                HashMap::new(),
                POLKIT_FLAG_ALLOW_USER_INTERACTION,
                "",
            )
            .await;

        let policy_missing = !Path::new(POLKIT_POLICY_PATH).exists();
        match res {
            Ok((true, _, _)) => Ok(()),
            Ok((false, _, _)) => {
                if policy_missing {
                    Err(DaemonError::NotAuthorized(
                        "Chính sách Polkit chưa được cài đặt vào hệ thống. Vui lòng chạy 'sudo ./setup.sh' chọn 1 (Install)".to_owned(),
                    ))
                } else {
                    Err(DaemonError::NotAuthorized(format!(
                        "Not authorized by system policy for action '{POLKIT_ACTION_MODIFY}'"
                    )))
                }
            }
            Err(err) => {
                let msg = err.to_string();
                if msg.contains("not registered") || policy_missing {
                    Err(DaemonError::NotAuthorized(
                        "Chính sách Polkit chưa được cài đặt vào hệ thống. Vui lòng chạy 'sudo ./setup.sh' chọn 1 (Install)".to_owned(),
                    ))
                } else {
                    Err(DaemonError::NotAuthorized(msg))
                }
            }
        }
    }
}
