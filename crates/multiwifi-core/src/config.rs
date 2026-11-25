use serde::{Deserialize, Serialize};
use std::collections::HashSet;
use std::fs::{self, OpenOptions};
use std::io::Read;
use std::path::Path;
use thiserror::Error;

const MAX_CONFIG_BYTES: u64 = 1024 * 1024;
const FORBIDDEN_SECRET_KEYWORDS: &[&str] = &[
    "password",
    "secret",
    "private_key",
    "token",
    "psk",
    "passphrase",
];

/// Errors produced when parsing, validating, or loading `MultiWiFi` config.
#[derive(Debug, Error)]
pub enum ConfigError {
    #[error("configuration syntax error: {0}")]
    Syntax(String),

    #[error("unsupported schema_version {0} (supported version is 1)")]
    SchemaVersion(u32),

    #[error("configuration validation failed: {0}")]
    Validation(String),

    #[error("line {line} contains forbidden secret field; MultiWiFi config never stores secrets")]
    SecretForbidden { line: usize },

    #[error("configuration I/O error: {0}")]
    Io(#[from] std::io::Error),
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize, Default)]
#[serde(rename_all = "snake_case")]
pub enum Mode {
    None,
    #[default]
    LoadBalance,
    Bonding,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize, Default)]
#[serde(rename_all = "snake_case")]
pub enum LoadBalanceStrategy {
    #[default]
    WeightedRoundRobin,
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
#[serde(default, deny_unknown_fields)]
pub struct UplinkConfig {
    pub device_id: String,
    pub enabled: bool,
    pub weight: u32,
}

impl Default for UplinkConfig {
    fn default() -> Self {
        Self {
            device_id: String::new(),
            enabled: true,
            weight: 1,
        }
    }
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
#[serde(default, deny_unknown_fields)]
pub struct LoadBalanceConfig {
    pub strategy: LoadBalanceStrategy,
    pub health_check_interval_ms: u32,
    pub failure_threshold: u32,
    pub recovery_threshold: u32,
    pub cooldown_ms: u32,
}

impl Default for LoadBalanceConfig {
    fn default() -> Self {
        Self {
            strategy: LoadBalanceStrategy::WeightedRoundRobin,
            health_check_interval_ms: 2000,
            failure_threshold: 3,
            recovery_threshold: 3,
            cooldown_ms: 5000,
        }
    }
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize, Default)]
#[serde(default, deny_unknown_fields)]
pub struct BondingConfig {
    pub enabled: bool,
    pub profile_id: String,
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
#[serde(default, deny_unknown_fields)]
pub struct MultiWifiConfig {
    pub schema_version: u32,
    pub mode: Mode,
    pub load_balance: LoadBalanceConfig,
    pub uplinks: Vec<UplinkConfig>,
    pub bonding: BondingConfig,
}

impl Default for MultiWifiConfig {
    fn default() -> Self {
        Self {
            schema_version: 1,
            mode: Mode::LoadBalance,
            load_balance: LoadBalanceConfig::default(),
            uplinks: Vec::new(),
            bonding: BondingConfig::default(),
        }
    }
}

fn is_valid_identifier(value: &str) -> bool {
    !value.is_empty()
        && value
            .bytes()
            .all(|b| b.is_ascii_alphanumeric() || b == b'-' || b == b'_')
}

fn check_forbidden_secrets(text: &str) -> Result<(), ConfigError> {
    for (idx, raw_line) in text.lines().enumerate() {
        let trimmed = raw_line.trim();
        if trimmed.is_empty() || trimmed.starts_with('#') {
            continue;
        }
        let lower = trimmed.to_ascii_lowercase();
        if FORBIDDEN_SECRET_KEYWORDS
            .iter()
            .any(|kw| lower.contains(kw))
        {
            return Err(ConfigError::SecretForbidden { line: idx + 1 });
        }
    }
    Ok(())
}

impl MultiWifiConfig {
    /// Parses and validates TOML configuration text.
    pub fn parse_str(text: &str) -> Result<Self, ConfigError> {
        check_forbidden_secrets(text)?;

        let cfg: Self = toml::from_str(text).map_err(|err| {
            let msg = err.to_string();
            if msg.contains("out of range")
                || msg.contains("invalid number")
                || msg.contains("invalid value: integer")
            {
                ConfigError::Validation(msg)
            } else {
                ConfigError::Syntax(msg)
            }
        })?;

        cfg.validate()?;
        Ok(cfg)
    }

    /// Reads, parses, and validates a configuration file securely (refusing symlinks and oversized files).
    pub fn parse_file(path: &Path) -> Result<Self, ConfigError> {
        let meta = fs::symlink_metadata(path)?;
        if meta.file_type().is_symlink() {
            return Err(ConfigError::Io(std::io::Error::new(
                std::io::ErrorKind::PermissionDenied,
                format!("refusing to follow symlink at '{}'", path.display()),
            )));
        }
        if meta.len() > MAX_CONFIG_BYTES {
            return Err(ConfigError::Validation(format!(
                "config file exceeds maximum allowed size ({MAX_CONFIG_BYTES} bytes)"
            )));
        }

        let mut opts = OpenOptions::new();
        opts.read(true);
        #[cfg(unix)]
        {
            use std::os::unix::fs::OpenOptionsExt;
            opts.custom_flags(0x2_0000); // O_NOFOLLOW
        }

        let file = opts.open(path)?;
        let mut content = String::new();
        file.take(MAX_CONFIG_BYTES + 1)
            .read_to_string(&mut content)?;
        if content.len() as u64 > MAX_CONFIG_BYTES {
            return Err(ConfigError::Validation(format!(
                "config file exceeds maximum allowed size ({MAX_CONFIG_BYTES} bytes)"
            )));
        }
        Self::parse_str(&content)
    }

    /// Validates invariants on a parsed or constructed configuration.
    pub fn validate(&self) -> Result<(), ConfigError> {
        if self.schema_version != 1 {
            return Err(ConfigError::SchemaVersion(self.schema_version));
        }

        let interval = self.load_balance.health_check_interval_ms;
        if !(100..=60_000).contains(&interval) {
            return Err(ConfigError::Validation(format!(
                "health_check_interval_ms ({interval}) must be between 100 and 60000 ms"
            )));
        }

        let fail_th = self.load_balance.failure_threshold;
        if !(1..=50).contains(&fail_th) {
            return Err(ConfigError::Validation(format!(
                "failure_threshold ({fail_th}) must be between 1 and 50"
            )));
        }

        let rec_th = self.load_balance.recovery_threshold;
        if !(1..=50).contains(&rec_th) {
            return Err(ConfigError::Validation(format!(
                "recovery_threshold ({rec_th}) must be between 1 and 50"
            )));
        }

        let cooldown = self.load_balance.cooldown_ms;
        if cooldown > 600_000 {
            return Err(ConfigError::Validation(format!(
                "cooldown_ms ({cooldown}) exceeds maximum limit of 600000 ms"
            )));
        }

        let mut seen = HashSet::with_capacity(self.uplinks.len());
        for up in &self.uplinks {
            if !is_valid_identifier(&up.device_id) {
                return Err(ConfigError::Validation(format!(
                    "invalid or empty uplink device_id '{}'",
                    up.device_id
                )));
            }
            if !seen.insert(up.device_id.as_str()) {
                return Err(ConfigError::Validation(format!(
                    "duplicate uplink device_id '{}' found",
                    up.device_id
                )));
            }
            if !(1..=1000).contains(&up.weight) {
                return Err(ConfigError::Validation(format!(
                    "uplink '{}' weight ({}) must be between 1 and 1000",
                    up.device_id, up.weight
                )));
            }
        }

        if self.bonding.enabled && !is_valid_identifier(&self.bonding.profile_id) {
            return Err(ConfigError::Validation(format!(
                "bonding enabled but profile_id '{}' is missing or invalid",
                self.bonding.profile_id
            )));
        }

        Ok(())
    }

    /// Serializes the configuration into canonical TOML.
    pub fn serialize(&self) -> Result<String, ConfigError> {
        toml::to_string_pretty(self).map_err(|e| ConfigError::Syntax(e.to_string()))
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::io::Write;
    use tempfile::NamedTempFile;

    #[test]
    fn default_config_is_valid() {
        let cfg = MultiWifiConfig::default();
        assert_eq!(cfg.schema_version, 1);
        assert_eq!(cfg.mode, Mode::LoadBalance);
        assert_eq!(cfg.load_balance.health_check_interval_ms, 2000);
        assert!(cfg.validate().is_ok());
    }

    #[test]
    fn parse_valid_and_roundtrip() {
        let valid_toml = r#"
schema_version = 1
mode = "load_balance"

[load_balance]
strategy = "weighted_round_robin"
health_check_interval_ms = 2500
failure_threshold = 4
recovery_threshold = 2
cooldown_ms = 3000

[[uplinks]]
device_id = "wlp0s20f3"
enabled = true
weight = 2

[[uplinks]]
device_id = "wlp0s20f0u1"
enabled = true
weight = 1

[bonding]
enabled = false
profile_id = ""
"#;
        let cfg = MultiWifiConfig::parse_str(valid_toml).expect("valid config");
        assert_eq!(cfg.uplinks.len(), 2);
        assert_eq!(cfg.uplinks[0].device_id, "wlp0s20f3");
        assert_eq!(cfg.uplinks[0].weight, 2);

        let serialized = cfg.serialize().expect("serialize");
        let reparsed = MultiWifiConfig::parse_str(&serialized).expect("reparsed");
        assert_eq!(cfg, reparsed);
    }

    #[test]
    fn reject_secrets() {
        let malicious = "schema_version = 1\nmode = \"load_balance\"\nwifi_password = \"secret\"\n";
        let err = MultiWifiConfig::parse_str(malicious).unwrap_err();
        assert!(matches!(err, ConfigError::SecretForbidden { line: 3 }));
    }

    #[test]
    fn reject_bad_schema_version() {
        let bad = "schema_version = 99\nmode = \"load_balance\"\n";
        let err = MultiWifiConfig::parse_str(bad).unwrap_err();
        assert!(matches!(err, ConfigError::SchemaVersion(99)));
    }

    #[test]
    fn reject_duplicate_uplinks() {
        let dup = r#"
schema_version = 1
mode = "load_balance"

[[uplinks]]
device_id = "wlan0"
weight = 1

[[uplinks]]
device_id = "wlan0"
weight = 2
"#;
        let err = MultiWifiConfig::parse_str(dup).unwrap_err();
        assert!(matches!(err, ConfigError::Validation(_)));
    }

    #[test]
    fn reject_invalid_range_overflow_and_syntax() {
        let low = "schema_version = 1\n[load_balance]\nhealth_check_interval_ms = 10\n";
        assert!(matches!(
            MultiWifiConfig::parse_str(low).unwrap_err(),
            ConfigError::Validation(_)
        ));

        let overflow =
            "schema_version = 1\n[load_balance]\nhealth_check_interval_ms = 4294968296\n";
        assert!(matches!(
            MultiWifiConfig::parse_str(overflow).unwrap_err(),
            ConfigError::Validation(_)
        ));

        let garbage = "schema_version = 1\n[load_balance]\ncooldown_ms = 500abc\n";
        assert!(matches!(
            MultiWifiConfig::parse_str(garbage).unwrap_err(),
            ConfigError::Syntax(_)
        ));
    }

    #[cfg(unix)]
    #[test]
    fn parse_file_rejects_symlink() {
        let mut target = NamedTempFile::new().expect("temp target");
        writeln!(target, "schema_version = 1").expect("write");

        let dir = tempfile::tempdir().expect("tempdir");
        let link_path = dir.path().join("cfg.toml");
        std::os::unix::fs::symlink(target.path(), &link_path).expect("symlink");

        let err = MultiWifiConfig::parse_file(&link_path).unwrap_err();
        assert!(matches!(err, ConfigError::Io(_)));
    }
}
