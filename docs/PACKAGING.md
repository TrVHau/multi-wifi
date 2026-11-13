# Packaging

## Components
Ship GUI and system daemon separately: GTK GUI, privileged daemon, D-Bus service/policy, polkit actions and shared interface files.

## Arch
Provide PKGBUILD producing `.pkg.tar.zst`. Declare runtime dependencies. Install systemd, D-Bus and polkit files with correct ownership/permissions. Package installation must not activate networking modes.

## Debian/Ubuntu
Provide `.deb` packages. Maintainer scripts must install service/policy safely, preserve configuration on upgrade and avoid disruptive networking changes during installation. Do not delete user config on ordinary uninstall.

## RPM
Provide spec files and test dependencies, ownership, service lifecycle and scripts in a clean VM.

## AppImage
AppImage may package the portable GUI only. It must not silently install a privileged daemon, modify routes or request root on launch. If daemon is absent, explain installation requirements and disable unavailable controls.

## Release checks
Clean install, ordinary-user GUI launch, D-Bus/polkit behavior, upgrade, uninstall, residual-resource inspection, signatures/checksums and verification that unrelated network state remains untouched.
