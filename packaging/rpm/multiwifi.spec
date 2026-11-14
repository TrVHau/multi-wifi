Name:           multiwifi
Version:        0.1.0
Release:        1%{?dist}
Summary:        Native Linux desktop application for multi-path Wi-Fi load balancing and MPTCP bonding

License:        MIT
URL:            https://github.com/TrVHau/multi-wifi
Source0:        %{name}-%{version}.tar.gz

BuildRequires:  cmake >= 3.20
BuildRequires:  gcc
BuildRequires:  pkgconfig
BuildRequires:  gtk4-devel >= 4.10
BuildRequires:  glib2-devel >= 2.74
BuildRequires:  NetworkManager-libnm-devel >= 1.40
BuildRequires:  polkit-devel
BuildRequires:  libnftables-devel
BuildRequires:  systemd-rpm-macros

Requires:       gtk4 >= 4.10
Requires:       glib2 >= 2.74
Requires:       NetworkManager-libnm >= 1.40
Requires:       polkit
Requires:       libnftables

%{?systemd_requires}

%description
MultiWiFi is a native Linux desktop application built with C11 and GTK4.
It manages multiple active Wi-Fi uplinks, providing flow-affinity load
balancing across independent NetworkManager connections and high-throughput
MPTCP bonding through remote aggregation endpoints.

%prep
%autosetup

%build
%cmake -DCMAKE_BUILD_TYPE=Release
%cmake_build

%install
%cmake_install
install -Dpm 0644 LICENSE %{buildroot}%{_datadir}/licenses/%{name}/LICENSE

%post
%systemd_post multiwifi-daemon.service

%preun
%systemd_preun multiwifi-daemon.service

%postun
%systemd_postun_with_restart multiwifi-daemon.service

%files
%license LICENSE
%{_bindir}/multiwifi-gui
%{_bindir}/multiwifi-daemon
%{_datadir}/applications/io.github.multiwifi.MultiWiFi.desktop
%{_datadir}/dbus-1/system.d/io.github.multiwifi.Daemon.conf
%{_datadir}/dbus-1/interfaces/io.github.multiwifi.Daemon1.xml
%{_datadir}/polkit-1/actions/io.github.multiwifi.policy
%{_unitdir}/multiwifi-daemon.service

%changelog
* Sat Nov 15 2026 MultiWiFi Team <support@multiwifi.local> - 0.1.0-1
- Initial release of MultiWiFi (Phases 0-7)
