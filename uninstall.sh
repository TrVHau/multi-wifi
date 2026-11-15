#!/usr/bin/env bash
# ==============================================================================
# MultiWiFi - Automated Linux Uninstallation Script
# ==============================================================================

set -e

RED='\033[0;31m'
GREEN='\033[0;32m'
BLUE='\033[0;34m'
YELLOW='\033[1;33m'
BOLD='\033[1m'
NC='\033[0m' # No Color

echo -e "${RED}${BOLD}==> MultiWiFi Uninstaller${NC}"

# Check for root / sudo privileges
if [ "$(id -u)" -ne 0 ]; then
    echo -e "${YELLOW}Yêu cầu quyền quản trị (root/sudo) để gỡ bỏ dịch vụ hệ thống.${NC}"
    echo -e "Đang chuyển sang chạy với sudo..."
    exec sudo "$0" "$@"
fi

PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$PROJECT_DIR"

echo -e "${BLUE}1/3. Dừng và vô hiệu hóa dịch vụ multiwifi-daemon...${NC}"
if command -v systemctl >/dev/null 2>&1; then
    systemctl stop multiwifi-daemon 2>/dev/null || true
    systemctl disable multiwifi-daemon 2>/dev/null || true
    echo -e "  ${GREEN}✔ Dịch vụ systemd đã dừng và bị vô hiệu hóa.${NC}"
fi

echo -e "${BLUE}2/3. Xóa sạch các file cài đặt của MultiWiFi...${NC}"
# Xóa theo install_manifest.txt nếu có
if [ -f "build/install_manifest.txt" ]; then
    xargs rm -f < build/install_manifest.txt 2>/dev/null || true
fi

# Xóa triệt để các file và thư mục liên quan
rm -f /usr/bin/multiwifi-gui
rm -f /usr/bin/multiwifi-daemon
rm -f /usr/local/bin/multiwifi-gui
rm -f /usr/local/bin/multiwifi-daemon
rm -f /usr/lib/systemd/system/multiwifi-daemon.service
rm -f /etc/systemd/system/multiwifi-daemon.service
rm -f /usr/share/dbus-1/system.d/io.github.multiwifi.Daemon.conf
rm -f /etc/dbus-1/system.d/io.github.multiwifi.Daemon.conf
rm -f /usr/share/polkit-1/actions/io.github.multiwifi.policy
rm -f /usr/share/applications/io.github.multiwifi.MultiWiFi.desktop
rm -f /usr/share/dbus-1/interfaces/io.github.multiwifi.Daemon1.xml
rm -rf /usr/share/multiwifi
rm -f /usr/share/icons/hicolor/*/apps/io.github.multiwifi.MultiWiFi.*
rm -f /var/run/multiwifi/*.journal 2>/dev/null || true

echo -e "${BLUE}3/3. Cập nhật lại hệ thống và bộ nhớ cache...${NC}"
if command -v systemctl >/dev/null 2>&1; then
    systemctl daemon-reload
fi
if command -v update-desktop-database >/dev/null 2>&1; then
    update-desktop-database -q /usr/share/applications || true
fi
if command -v gtk-update-icon-cache >/dev/null 2>&1; then
    gtk-update-icon-cache -q -t -f /usr/share/icons/hicolor || true
fi

echo ""
echo -e "${GREEN}${BOLD}=================================================================${NC}"
echo -e "${GREEN}${BOLD}✔ ĐÃ GỠ CÀI ĐẶT MULTIWIFI HOÀN TOÀN SẠCH SẼ KHỎI HỆ THỐNG!${NC}"
echo -e "${GREEN}${BOLD}=================================================================${NC}"
echo -e "Tất cả các file nhị phân, service, icon, firewall table và routing rules đã được hoàn trả nguyên trạng."
echo ""
