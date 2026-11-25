use std::fmt::Write as _;
use std::fs::{self, DirBuilder};
use std::io::Write as _;
use std::path::Path;
use std::time::{SystemTime, UNIX_EPOCH};
use tempfile::NamedTempFile;
use thiserror::Error;

#[derive(Debug, Error)]
pub enum JournalError {
    #[error("invalid journal argument: {0}")]
    InvalidArgument(&'static str),

    #[error("journal format error: {0}")]
    Format(String),

    #[error("journal I/O error: {0}")]
    Io(#[from] std::io::Error),
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
#[repr(u32)]
pub enum JournalOpType {
    AddRule = 1,
    AddRoute = 2,
    AllocTable = 3,
}

impl TryFrom<i32> for JournalOpType {
    type Error = JournalError;

    fn try_from(val: i32) -> Result<Self, Self::Error> {
        match val {
            1 => Ok(Self::AddRule),
            2 => Ok(Self::AddRoute),
            3 => Ok(Self::AllocTable),
            other => Err(JournalError::Format(format!(
                "unknown journal op_type {other}"
            ))),
        }
    }
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct JournalEntry {
    pub op_type: JournalOpType,
    pub table_id: u32,
    pub rule_pref: u32,
    pub fwmark: u32,
    pub fwmask: u32,
    pub src_ip: Option<String>,
    pub gateway: Option<String>,
    pub ifname: Option<String>,
    pub metric: u32,
    pub timestamp_us: i64,
}

impl JournalEntry {
    #[must_use]
    pub fn add_rule(
        table_id: u32,
        rule_pref: u32,
        fwmark: u32,
        mark_mask: u32,
        src_ip: Option<&str>,
        ifname: Option<&str>,
    ) -> Self {
        Self::with_type(
            JournalOpType::AddRule,
            table_id,
            rule_pref,
            fwmark,
            mark_mask,
            src_ip,
            None,
            ifname,
            0,
        )
    }

    #[must_use]
    pub fn add_route(table_id: u32, gateway: &str, ifname: Option<&str>, metric: u32) -> Self {
        Self::with_type(
            JournalOpType::AddRoute,
            table_id,
            0,
            0,
            0,
            None,
            Some(gateway),
            ifname,
            metric,
        )
    }

    #[allow(clippy::too_many_arguments)]
    fn with_type(
        op_type: JournalOpType,
        table_id: u32,
        rule_pref: u32,
        fwmark: u32,
        mark_mask: u32,
        src_ip: Option<&str>,
        gateway: Option<&str>,
        ifname: Option<&str>,
        metric: u32,
    ) -> Self {
        let now_us = SystemTime::now()
            .duration_since(UNIX_EPOCH)
            .map_or(0, |d| i64::try_from(d.as_micros()).unwrap_or(i64::MAX));
        Self {
            op_type,
            table_id,
            rule_pref,
            fwmark,
            fwmask: mark_mask,
            src_ip: src_ip.filter(|s| !s.is_empty()).map(str::to_owned),
            gateway: gateway.filter(|s| !s.is_empty()).map(str::to_owned),
            ifname: ifname.filter(|s| !s.is_empty()).map(str::to_owned),
            metric,
            timestamp_us: now_us,
        }
    }
}

/// Atomic route rollback journal compatible with the C `GKeyFile` journal format.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct RouteJournal {
    pub op_id: String,
    pub entries: Vec<JournalEntry>,
}

fn reject_symlink_if_exists(path: &Path) -> Result<(), JournalError> {
    match fs::symlink_metadata(path) {
        Ok(meta) if meta.file_type().is_symlink() => Err(JournalError::Io(std::io::Error::new(
            std::io::ErrorKind::PermissionDenied,
            format!("refusing to follow symlink at '{}'", path.display()),
        ))),
        Ok(_) => Ok(()),
        Err(e) if e.kind() == std::io::ErrorKind::NotFound => Ok(()),
        Err(e) => Err(JournalError::Io(e)),
    }
}

impl RouteJournal {
    #[must_use]
    pub fn new(op_id: &str) -> Self {
        Self {
            op_id: if op_id.is_empty() {
                "unknown".to_owned()
            } else {
                op_id.to_owned()
            },
            entries: Vec::new(),
        }
    }

    pub fn append(&mut self, entry: JournalEntry) {
        self.entries.push(entry);
    }

    /// Atomically persists the journal to `file_path` in `GKeyFile`-compatible INI format.
    pub fn save_to_file(&self, file_path: &Path) -> Result<(), JournalError> {
        let Some(parent) = file_path.parent() else {
            return Err(JournalError::InvalidArgument(
                "file path has no parent directory",
            ));
        };
        reject_symlink_if_exists(file_path)?;

        let mut dir_builder = DirBuilder::new();
        dir_builder.recursive(true);
        #[cfg(unix)]
        {
            use std::os::unix::fs::DirBuilderExt;
            dir_builder.mode(0o700);
        }
        dir_builder.create(parent)?;

        let mut out = String::new();
        writeln!(
            out,
            "[journal]\nop_id={}\ncount={}",
            self.op_id,
            self.entries.len()
        )
        .map_err(|e| JournalError::Format(e.to_string()))?;

        for (idx, e) in self.entries.iter().enumerate() {
            writeln!(
                out,
                "\n[entry_{idx}]\nop_type={}\ntable_id={}\nrule_pref={}\nfwmark={}\nfwmask={}\nsrc_ip={}\ngateway={}\nifname={}\nmetric={}\ntimestamp_us={}",
                e.op_type as u32,
                e.table_id,
                e.rule_pref,
                e.fwmark,
                e.fwmask,
                e.src_ip.as_deref().unwrap_or(""),
                e.gateway.as_deref().unwrap_or(""),
                e.ifname.as_deref().unwrap_or(""),
                e.metric,
                e.timestamp_us,
            )
            .map_err(|err| JournalError::Format(err.to_string()))?;
        }

        let mut tmp = NamedTempFile::new_in(parent)?;
        tmp.write_all(out.as_bytes())?;
        tmp.as_file().sync_all()?;
        tmp.persist(file_path)
            .map_err(|e| JournalError::Io(e.error))?;
        Ok(())
    }

    /// Loads an existing journal from disk, rejecting symlinks (`CWE-59`).
    pub fn load_from_file(file_path: &Path) -> Result<Self, JournalError> {
        let meta = fs::symlink_metadata(file_path)?;
        if meta.file_type().is_symlink() {
            return Err(JournalError::Io(std::io::Error::new(
                std::io::ErrorKind::PermissionDenied,
                format!(
                    "refusing to load journal symlink at '{}'",
                    file_path.display()
                ),
            )));
        }

        let content = fs::read_to_string(file_path)?;
        let mut op_id = String::from("unknown");
        let mut expected_count: Option<usize> = None;
        let mut current_group = String::new();
        let mut raw_entries: Vec<(usize, RawEntry)> = Vec::new();

        for raw_line in content.lines() {
            let line = raw_line.trim();
            if line.is_empty() || line.starts_with('#') {
                continue;
            }
            if line.starts_with('[') && line.ends_with(']') {
                line[1..line.len() - 1]
                    .trim()
                    .clone_into(&mut current_group);
                if let Some(idx_str) = current_group.strip_prefix("entry_") {
                    let idx: usize = idx_str.parse().map_err(|_| {
                        JournalError::Format(format!("invalid group '{current_group}'"))
                    })?;
                    raw_entries.push((idx, RawEntry::default()));
                }
                continue;
            }
            let Some((key, val)) = line.split_once('=') else {
                continue;
            };
            let key = key.trim();
            let val = val.trim();

            if current_group == "journal" {
                if key == "op_id" && !val.is_empty() {
                    val.clone_into(&mut op_id);
                } else if key == "count" {
                    expected_count = Some(
                        val.parse()
                            .map_err(|_| JournalError::Format(format!("invalid count '{val}'")))?,
                    );
                }
            } else if current_group.starts_with("entry_") {
                if let Some((_, entry)) = raw_entries.last_mut() {
                    entry.apply(key, val)?;
                }
            }
        }

        raw_entries.sort_by_key(|(idx, _)| *idx);
        let limit = expected_count.unwrap_or(raw_entries.len());
        let mut journal = Self::new(&op_id);
        for (_, raw) in raw_entries.into_iter().take(limit) {
            journal.append(raw.build()?);
        }
        Ok(journal)
    }

    pub fn remove_file(file_path: &Path) -> Result<(), JournalError> {
        match fs::symlink_metadata(file_path) {
            Ok(meta) if meta.file_type().is_symlink() => {
                Err(JournalError::Io(std::io::Error::new(
                    std::io::ErrorKind::PermissionDenied,
                    format!("refusing to unlink symlink at '{}'", file_path.display()),
                )))
            }
            Ok(_) => {
                fs::remove_file(file_path)?;
                Ok(())
            }
            Err(e) if e.kind() == std::io::ErrorKind::NotFound => Ok(()),
            Err(e) => Err(JournalError::Io(e)),
        }
    }
}

#[derive(Default)]
struct RawEntry {
    op_type: Option<JournalOpType>,
    table_id: u32,
    rule_pref: u32,
    fwmark: u32,
    fwmask: u32,
    src_ip: Option<String>,
    gateway: Option<String>,
    ifname: Option<String>,
    metric: u32,
    timestamp_us: i64,
}

impl RawEntry {
    fn apply(&mut self, key: &str, val: &str) -> Result<(), JournalError> {
        match key {
            "op_type" => {
                let code: i32 = val
                    .parse()
                    .map_err(|_| JournalError::Format(format!("invalid op_type '{val}'")))?;
                self.op_type = Some(JournalOpType::try_from(code)?);
            }
            "table_id" => {
                self.table_id = val
                    .parse()
                    .map_err(|_| JournalError::Format(format!("invalid table_id '{val}'")))?;
            }
            "rule_pref" => {
                self.rule_pref = val
                    .parse()
                    .map_err(|_| JournalError::Format(format!("invalid rule_pref '{val}'")))?;
            }
            "fwmark" => {
                self.fwmark = val
                    .parse()
                    .map_err(|_| JournalError::Format(format!("invalid fwmark '{val}'")))?;
            }
            "fwmask" => {
                self.fwmask = val
                    .parse()
                    .map_err(|_| JournalError::Format(format!("invalid fwmask '{val}'")))?;
            }
            "src_ip" => {
                self.src_ip = (!val.is_empty()).then(|| val.to_owned());
            }
            "gateway" => {
                self.gateway = (!val.is_empty()).then(|| val.to_owned());
            }
            "ifname" => {
                self.ifname = (!val.is_empty()).then(|| val.to_owned());
            }
            "metric" => {
                self.metric = val
                    .parse()
                    .map_err(|_| JournalError::Format(format!("invalid metric '{val}'")))?;
            }
            "timestamp_us" => {
                self.timestamp_us = val
                    .parse()
                    .map_err(|_| JournalError::Format(format!("invalid timestamp_us '{val}'")))?;
            }
            _ => {}
        }
        Ok(())
    }

    fn build(self) -> Result<JournalEntry, JournalError> {
        let op_type = self
            .op_type
            .ok_or_else(|| JournalError::Format("journal entry missing op_type".to_owned()))?;
        Ok(JournalEntry {
            op_type,
            table_id: self.table_id,
            rule_pref: self.rule_pref,
            fwmark: self.fwmark,
            fwmask: self.fwmask,
            src_ip: self.src_ip,
            gateway: self.gateway,
            ifname: self.ifname,
            metric: self.metric,
            timestamp_us: self.timestamp_us,
        })
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn journal_save_load_and_remove_roundtrip() {
        let dir = tempfile::tempdir().expect("tempdir");
        let path = dir.path().join("journals").join("op-123.journal");

        let mut j = RouteJournal::new("op-123");
        j.append(JournalEntry::add_rule(
            10001,
            20001,
            0x1001,
            0xff00,
            Some("192.168.1.5"),
            Some("wlan0"),
        ));
        j.append(JournalEntry::add_route(
            10001,
            "192.168.1.1",
            Some("wlan0"),
            100,
        ));

        j.save_to_file(&path).expect("save journal");
        assert!(path.exists());

        let loaded = RouteJournal::load_from_file(&path).expect("load journal");
        assert_eq!(loaded.op_id, "op-123");
        assert_eq!(loaded.entries.len(), 2);
        assert_eq!(loaded.entries[0].op_type, JournalOpType::AddRule);
        assert_eq!(loaded.entries[0].src_ip.as_deref(), Some("192.168.1.5"));
        assert_eq!(loaded.entries[1].op_type, JournalOpType::AddRoute);
        assert_eq!(loaded.entries[1].gateway.as_deref(), Some("192.168.1.1"));

        RouteJournal::remove_file(&path).expect("remove journal");
        assert!(!path.exists());
        // Removing already-absent file succeeds cleanly
        RouteJournal::remove_file(&path).expect("idempotent remove");
    }

    #[cfg(unix)]
    #[test]
    fn journal_refuses_symlinks() {
        let dir = tempfile::tempdir().expect("tempdir");
        let target = dir.path().join("real.txt");
        let link = dir.path().join("evil.journal");
        std::os::unix::fs::symlink(&target, &link).expect("symlink");

        let j = RouteJournal::new("op-sym");
        assert!(j.save_to_file(&link).is_err());
        assert!(!target.exists());
        assert!(RouteJournal::load_from_file(&link).is_err());
        assert!(RouteJournal::remove_file(&link).is_err());
    }
}
