#!/usr/bin/env bash
# ==============================================================================
# MultiWiFi - Automated Linux Installation Script
# ==============================================================================

set -e

RED='\033[0;31m'
GREEN='\033[0;32m'
BLUE='\033[0;34m'
YELLOW='\033[1;33m'
BOLD='\033[1m'
NC='\033[0m' # No Color

echo -e "${BLUE}${BOLD}==> MultiWiFi Installer${NC}"

# Check for root / sudo privileges
if [ "$(id -u)" -ne 0 ]; then
    echo -e "${YELLOW}Yêu cầu quyền quản trị (root/sudo) để cài đặt dịch vụ hệ thống.${NC}"
    echo -e "Đang chuyển sang chạy với sudo..."
    exec sudo "$0" "$@"
fi

PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$PROJECT_DIR"

echo -e "${BLUE}1/4. Kiểm tra và biên dịch mã nguồn...${NC}"
cmake -B build -DCMAKE_INSTALL_PREFIX=/usr -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON
cmake --build build -j"$(nproc)"

echo -e "${BLUE}2/4. Cài đặt các tệp tin vào hệ thống (/usr)...${NC}"
cmake --install build

echo -e "${BLUE}3/4. Kích hoạt dịch vụ hệ thống multiwifi-daemon...${NC}"
if command -v systemctl >/dev/null 2>&1; then
    systemctl daemon-reload
    systemctl enable --now multiwifi-daemon
    echo -e "  ${GREEN}✔ multiwifi-daemon.service đã được khởi chạy thành công.${NC}"
fi

echo -e "${BLUE}4/4. Cập nhật biểu tượng và menu ứng dụng...${NC}"
if command -v update-desktop-database >/dev/null 2>&1; then
    update-desktop-database -q /usr/share/applications || true
fi
if command -v gtk-update-icon-cache >/dev/null 2>&1; then
    gtk-update-icon-cache -q -t -f /usr/share/icons/hicolor || true
fi

echo ""
echo -e "${GREEN}${BOLD}=================================================================${NC}"
echo -e "${GREEN}${BOLD}✔ CÀI ĐẶT MULTIWIFI THÀNH CÔNG!${NC}"
echo -e "${GREEN}${BOLD}=================================================================${NC}"
echo -e "• Daemon chạy ngầm:  ${GREEN}Active (systemd)${NC}"
echo -e "• Mở ứng dụng GUI:    Gõ lệnh ${BOLD}multiwifi-gui${NC} hoặc tìm 'MultiWiFi' trong menu"
echo -e "• Để gỡ cài đặt sau này: Chạy lệnh ${BOLD}sudo ./uninstall.sh${NC}"
echo ""
