# MultiWiFi Packaging & Distribution Guide

This document describes the package architectures, build manifests, filesystem hierarchies, and installation procedures across supported Linux distributions.

---

## 1. Supported Packaging Formats

All packages distribute the unprivileged **C11/GTK4 desktop GUI (`multiwifi-gui`)** together with the privileged **Rust system daemon (`multiwifi-daemon`)** (except the portable AppImage, which bundles the unprivileged GUI only).

| Distribution / Target | Package Type | Manifest / Tool | Output Artifact |
| :--- | :--- | :--- | :--- |
| **Arch Linux / Manjaro** | Pacman package | `packaging/arch/PKGBUILD` | `multiwifi-0.1.0-1-x86_64.pkg.tar.zst` |
| **Debian / Ubuntu / Mint** | Debian / CPack | `packaging/debian/`, `CMakeLists.txt` | `multiwifi_0.1.0-1_amd64.deb` / `cpack` `.deb` |
| **Fedora / RHEL / CentOS** | RPM package | `packaging/rpm/multiwifi.spec` | `multiwifi-0.1.0-1.fc*.x86_64.rpm` |
| **Universal Linux** | Portable AppDir / AppImage | `packaging/appimage/build-appimage.sh` | `build/AppDir/` (GUI only) |
| **Source Quick Install** | Unified script | `setup.sh` | Direct system installation under `/usr` |

---

## 2. Installed Filesystem Hierarchy (FHS)

```text
/usr/
├── bin/
│   ├── multiwifi-daemon                       # Privileged Rust D-Bus system service
│   └── multiwifi-gui                          # Unprivileged C11/GTK4 desktop app
├── lib/systemd/system/
│   └── multiwifi-daemon.service               # Hardened systemd unit
└── share/
    ├── applications/
    │   └── io.github.multiwifi.MultiWiFi.desktop
    ├── dbus-1/
    │   ├── interfaces/
    │   │   └── io.github.multiwifi.Daemon1.xml
    │   └── system.d/
    │       └── io.github.multiwifi.Daemon.conf
    ├── icons/hicolor/
    │   └── */apps/io.github.multiwifi.MultiWiFi.*
    ├── multiwifi/
    │   └── style.css
    └── polkit-1/
        ├── actions/
        │   └── io.github.multiwifi.policy
        └── rules.d/
            └── io.github.multiwifi.rules

/run/
└── multiwifi/
    ├── journals/                              # Transactional netlink recovery journals
    └── resolv.conf.bak                        # Ephemeral DNS backup when active
```

---

## 3. Building Packages

CMake compiles `multiwifi-gui` (C11/GTK4) together with the Rust system daemon (`cargo build --release -p multiwifi-daemon`) whenever `MULTIWIFI_BUILD_DAEMON=ON` (default). When `BUILD_TESTING=OFF`, legacy C test-only core modules are skipped completely.

### Arch Linux (`PKGBUILD`)
```bash
cd packaging/arch
makepkg -si
```

### CPack (`.deb` & `.tar.zst`)
```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF
cmake --build build -j"$(nproc)"
cd build && cpack
```

### Debian / Ubuntu (`dpkg-buildpackage`)
```bash
dpkg-buildpackage -us -uc -b
sudo dpkg -i ../multiwifi_*.deb
```

### Fedora / RHEL (`.spec`)
```bash
rpmbuild -ba packaging/rpm/multiwifi.spec
sudo dnf install ~/rpmbuild/RPMS/x86_64/multiwifi-*.rpm
```

### Portable Standalone GUI AppDir
```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j"$(nproc)"
./packaging/appimage/build-appimage.sh build
./build/AppDir/AppRun
```

---

## 4. Unified System Script (`setup.sh`)

```bash
# 1. Automated build (unprivileged when invoked without sudo) and system installation
./setup.sh install      # or: sudo ./setup.sh install

# 2. Inspect daemon status
./setup.sh status

# 3. Restart daemon after updates
sudo ./setup.sh restart

# 4. Complete uninstallation
sudo ./setup.sh uninstall
```

---

## 5. Security & Isolation Guarantees During Packaging
- **No Network Mutation on Install:** Package post-install scripts reload `systemd`, without enabling load balancing or altering host routes automatically.
- **Clean Uninstallation:** Removing or purging cleans installed unit/policy/D-Bus/icon files and removes orphaned runtime journals.
- **Unprivileged AppImage:** `AppDir` explicitly excludes `multiwifi-daemon`, `sudo`, and `pkexec`; the GUI connects to the host system daemon over System D-Bus.
