# MultiWiFi Packaging & Distribution Guide

This document describes the package architectures, build manifests, filesystem hierarchies, and installation procedures across supported Linux distributions.

---

## 1. Supported Packaging Formats

MultiWiFi provides official packaging targets for all major Linux ecosystems:

| Distribution / Target | Package Type | Manifest / Tool | Output Artifact |
| :--- | :--- | :--- | :--- |
| **Arch Linux / Manjaro** | Pacman package | `packaging/arch/PKGBUILD` | `multiwifi-0.1.0-1-x86_64.pkg.tar.zst` |
| **Debian / Ubuntu / Mint** | Debian package | `packaging/debian/` | `multiwifi_0.1.0-1_amd64.deb` |
| **Fedora / RHEL / CentOS** | RPM package | `packaging/rpm/multiwifi.spec`| `multiwifi-0.1.0-1.fc*.x86_64.rpm` |
| **Universal Linux** | Portable AppImage | `packaging/appimage/` | `MultiWiFi-x86_64.AppImage` |
| **Source Quick Install** | Unified script | `setup.sh` | Direct system installation |

---

## 2. Installed Filesystem Hierarchy (FHS)

When installed on a Linux system, MultiWiFi places files according to the Filesystem Hierarchy Standard:

```text
/usr/
├── bin/
│   ├── multiwifi-daemon       # Privileged background system service
│   └── multiwifi-gui          # Unprivileged GTK4 desktop application
├── lib/systemd/system/
│   └── multiwifi-daemon.service # Systemd unit configuration
└── share/
    ├── applications/
    │   └── io.github.multiwifi.MultiWiFi.desktop # Desktop entry launcher
    ├── dbus-1/system.d/
    │   └── io.github.multiwifi.conf              # System D-Bus security policy
    ├── icons/hicolor/
    │   └── */apps/io.github.multiwifi.MultiWiFi.png # Application icons (16-512px)
    ├── polkit-1/
    │   ├── actions/
    │   │   └── io.github.multiwifi.policy        # Polkit action declaration
    │   └── rules.d/
    │       └── io.github.multiwifi.rules         # Wheel/sudo session rules
    └── metainfo/
        └── io.github.multiwifi.MultiWiFi.metainfo.xml # AppStream metadata

/etc/
└── multiwifi/
    └── multiwifi.conf         # System configuration file (TOML)

/run/
└── multiwifi/
    └── journals/              # Ephemeral transactional recovery journals
```

---

## 3. Building Packages

### Arch Linux (`PKGBUILD`)
```bash
cd packaging/arch
makepkg -si
```

### Debian / Ubuntu (`.deb`)
```bash
dpkg-buildpackage -us -uc -b
sudo dpkg -i ../multiwifi_*.deb
```

### Fedora / RHEL (`.spec`)
```bash
rpmbuild -ba packaging/rpm/multiwifi.spec
sudo dnf install ~/rpmbuild/RPMS/x86_64/multiwifi-*.rpm
```

### Portable Standalone AppImage
```bash
./packaging/appimage/build_appimage.sh
# Run anywhere:
./build-appimage/MultiWiFi-x86_64.AppImage
```

---

## 4. Unified System Script (`setup.sh`)

The root `setup.sh` script automates installation, systemd enablement, status checking, and clean removal:

```bash
# 1. Automated installation and system service startup
sudo ./setup.sh install

# 2. Inspect daemon status
sudo ./setup.sh status

# 3. Restart daemon after updating configuration
sudo ./setup.sh restart

# 4. Clean and complete uninstallation
sudo ./setup.sh uninstall
```

---

## 5. Security & Isolation Guarantees During Packaging
- **No Network Mutation on Install:** Installing packages must never modify host routing or start load balancing automatically.
- **Clean Uninstallation:** Running `setup.sh uninstall` or package purge commands guarantees that all nftables tables, policy routing rules, and `/run/multiwifi` journals are cleanly flushed without leaving orphaned artifacts.
- **Config Preservation:** Upgrading packages preserves existing `/etc/multiwifi/multiwifi.conf` configurations.
