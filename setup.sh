#!/usr/bin/env bash
# ==============================================================================
# MultiWiFi - Unified Interactive Setup & Management Script
# ==============================================================================

set -e

RED='\033[0;31m'
GREEN='\033[0;32m'
BLUE='\033[0;34m'
YELLOW='\033[1;33m'
CYAN='\033[0;36m'
BOLD='\033[1m'
NC='\033[0m' # No Color

PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$PROJECT_DIR"

show_help() {
    echo -e "${BLUE}${BOLD}MultiWiFi - Trình quản lý cài đặt & gỡ bỏ${NC}"
    echo ""
    echo -e "Cách sử dụng:"
    echo -e "  ${BOLD}./setup.sh${NC}                    : Mở Menu tương tác để xem và chọn thao tác"
    echo -e "  ${BOLD}sudo ./setup.sh install${NC}      : Cài đặt MultiWiFi vào hệ thống"
    echo -e "  ${BOLD}sudo ./setup.sh uninstall${NC}    : Gỡ bỏ MultiWiFi sạch sẽ khỏi hệ thống"
    echo -e "  ${BOLD}./setup.sh status${NC}            : Xem trạng thái hoạt động của daemon"
    echo -e "  ${BOLD}sudo ./setup.sh restart${NC}      : Khởi động lại daemon hệ thống"
    echo -e "  ${BOLD}./setup.sh help${NC}               : Hiển thị bảng hướng dẫn này"
    echo ""
}

check_root() {
    local cmd="${1:-}"
    if [ "$(id -u)" -ne 0 ]; then
        echo -e "${YELLOW}Yêu cầu quyền quản trị (root/sudo) để thực hiện thao tác này.${NC}"
        echo -e "Đang chuyển sang thực thi với sudo..."
        exec sudo "$0" "$cmd"
    fi
}

do_install() {
    check_root "install"
    echo -e "${BLUE}${BOLD}==> Bắt đầu cài đặt MultiWiFi vào hệ thống...${NC}"

    echo -e "${BLUE}1/4. Kiểm tra và biên dịch mã nguồn (/usr)...${NC}"
    cmake -B build -DCMAKE_INSTALL_PREFIX=/usr -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON
    cmake --build build -j"$(nproc)"

    echo -e "${BLUE}2/4. Cài đặt các tệp tin vào hệ thống...${NC}"
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
    echo -e "${GREEN}${BOLD}✔ CÀI ĐẶT MULTIWIFI HOÀN TẤT THÀNH CÔNG!${NC}"
    echo -e "${GREEN}${BOLD}=================================================================${NC}"
    echo -e "• Daemon chạy ngầm:   ${GREEN}Active (systemd)${NC}"
    echo -e "• Khởi chạy giao diện: Gõ lệnh ${BOLD}multiwifi-gui${NC} hoặc mở từ menu ứng dụng"
    echo -e "• Để gỡ cài đặt:        Chạy lệnh ${BOLD}sudo ./setup.sh uninstall${NC}"
    echo ""
}

do_uninstall() {
    check_root "uninstall"
    echo -e "${RED}${BOLD}==> Bắt đầu gỡ cài đặt MultiWiFi khỏi hệ thống...${NC}"

    echo -e "${BLUE}1/3. Dừng và vô hiệu hóa dịch vụ multiwifi-daemon...${NC}"
    if command -v systemctl >/dev/null 2>&1; then
        systemctl stop multiwifi-daemon 2>/dev/null || true
        systemctl disable multiwifi-daemon 2>/dev/null || true
        echo -e "  ${GREEN}✔ Dịch vụ systemd đã dừng và bị vô hiệu hóa.${NC}"
    fi

    echo -e "${BLUE}2/3. Xóa các tệp tin đã cài đặt...${NC}"
    if [ -f "build/install_manifest.txt" ]; then
        xargs rm -f < build/install_manifest.txt 2>/dev/null || true
    fi

    # Dọn dẹp sạch sẽ tất cả các đường dẫn đăng ký
    rm -f /usr/bin/multiwifi-gui
    rm -f /usr/bin/multiwifi-daemon
    rm -f /usr/local/bin/multiwifi-gui
    rm -f /usr/local/bin/multiwifi-daemon
    rm -f /usr/lib/systemd/system/multiwifi-daemon.service
    rm -f /etc/systemd/system/multiwifi-daemon.service
    rm -f /usr/share/dbus-1/system.d/io.github.multiwifi.Daemon.conf
    rm -f /etc/dbus-1/system.d/io.github.multiwifi.Daemon.conf
    rm -f /usr/share/polkit-1/actions/io.github.multiwifi.policy
    rm -f /usr/share/polkit-1/rules.d/io.github.multiwifi.rules
    rm -f /etc/polkit-1/rules.d/io.github.multiwifi.rules
    rm -f /usr/share/applications/io.github.multiwifi.MultiWiFi.desktop
    rm -f /usr/share/dbus-1/interfaces/io.github.multiwifi.Daemon1.xml
    rm -rf /usr/share/multiwifi
    rm -f /usr/share/icons/hicolor/*/apps/io.github.multiwifi.MultiWiFi.*
    rm -f /var/run/multiwifi/*.journal 2>/dev/null || true

    echo -e "${BLUE}3/3. Cập nhật lại hệ thống...${NC}"
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
    echo -e "${GREEN}${BOLD}✔ ĐÃ GỠ BỎ MULTIWIFI HOÀN TOÀN SẠCH SẼ KHỎI HỆ THỐNG!${NC}"
    echo -e "${GREEN}${BOLD}=================================================================${NC}"
    echo -e "Hệ thống, bảng định tuyến và firewall đã được hoàn trả nguyên vẹn."
    echo ""
}

do_status() {
    if command -v systemctl >/dev/null 2>&1; then
        systemctl status multiwifi-daemon --no-pager || true
    else
        echo -e "${YELLOW}Hệ thống không sử dụng systemd.${NC}"
    fi
}

do_restart() {
    check_root "restart"
    echo -e "${BLUE}Đang khởi động lại multiwifi-daemon...${NC}"
    if command -v systemctl >/dev/null 2>&1; then
        systemctl restart multiwifi-daemon
        echo -e "  ${GREEN}✔ multiwifi-daemon đã khởi động lại.${NC}"
        systemctl status multiwifi-daemon --no-pager -n 5 || true
    fi
}

show_interactive_menu() {
    echo ""
    echo -e "${CYAN}${BOLD}=================================================================${NC}"
    echo -e "${BLUE}${BOLD}            MultiWiFi - Trình Quản Lý Cài Đặt & Dịch Vụ          ${NC}"
    echo -e "${CYAN}${BOLD}=================================================================${NC}"
    echo ""
    echo -e "  ${BOLD}1)${NC} ${GREEN}Cài đặt vào hệ thống${NC}    (Build, cài file vào /usr, bật Daemon)"
    echo -e "  ${BOLD}2)${NC} ${RED}Gỡ cài đặt hoàn toàn${NC}    (Dừng service, xóa sạch mọi file)"
    echo -e "  ${BOLD}3)${NC} ${CYAN}Kiểm tra trạng thái${NC}     (Xem trạng thái multiwifi-daemon)"
    echo -e "  ${BOLD}4)${NC} ${YELLOW}Khởi động lại Daemon${NC}    (Restart systemd service)"
    echo -e "  ${BOLD}5)${NC} Hướng dẫn sử dụng      (Help)"
    echo -e "  ${BOLD}0)${NC} Thoát                  (Exit)"
    echo ""
    echo -e "${CYAN}-----------------------------------------------------------------${NC}"
    read -rp "Nhập lựa chọn của bạn [0-5]: " choice
    echo ""

    case "$choice" in
        1)
            do_install
            ;;
        2)
            do_uninstall
            ;;
        3)
            do_status
            ;;
        4)
            do_restart
            ;;
        5)
            show_help
            ;;
        0|q|Q)
            echo -e "${YELLOW}Đã thoát.${NC}"
            exit 0
            ;;
        *)
            echo -e "${RED}Lựa chọn không hợp lệ: '$choice'. Vui lòng chọn số từ 0 đến 5.${NC}"
            exit 1
            ;;
    esac
}

# Xử lý tham số đầu vào
ACTION="${1:-}"

case "$ACTION" in
    "")
        show_interactive_menu
        ;;
    install)
        do_install
        ;;
    uninstall|remove)
        do_uninstall
        ;;
    status)
        do_status
        ;;
    restart)
        do_restart
        ;;
    help|--help|-h)
        show_help
        ;;
    *)
        echo -e "${RED}Lựa chọn không hợp lệ: '$ACTION'${NC}"
        echo ""
        show_help
        exit 1
        ;;
esac
