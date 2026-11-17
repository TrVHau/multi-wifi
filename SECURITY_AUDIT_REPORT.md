# BÁO CÁO ĐÁNH GIÁ AN NINH ỨNG DỤNG MULTIWIFI
*Ngày thực hiện: 17/11/2026*
*Chuyên gia đánh giá: Antigravity Security Assessment Team*

---

## 1. TỔNG QUAN HỆ THỐNG
MultiWiFi là ứng dụng native Linux desktop (C11, GTK4, GLib/GIO, libnm, nftables, netlink) kết hợp nhiều giao diện Wi-Fi để cân bằng tải (Load Balancing) hoặc ghép kênh (Bonding Tunnel qua MPTCP/TUN). 
Hệ thống được thiết kế theo mô hình tách biệt tiến trình:
- **multiwifi-gui**: Ứng dụng giao diện GTK4 chạy với quyền người dùng không đặc quyền (unprivileged).
- **multiwifi-daemon**: Dịch vụ hệ thống chạy với quyền `root` quản lý định tuyến, tường lửa và thiết bị mạng thông qua giao tiếp D-Bus và cơ chế xác thực Polkit.

---

## 2. BẢNG TỔNG HỢP CÁC LỖ HỔNG AN NINH

| STT | Tên Lỗ Hổng / Điểm Yếu | Mức Độ | Mã Định Danh (CWE) | Khả Năng Khai Thác | Tác Động |
| :-- | :--- | :---: | :---: | :---: | :--- |
| 1 | Cấu hình Polkit cho phép phiên local can thiệp root không cần mật khẩu | **CRITICAL** | CWE-250, CWE-306 | Rất cao | Leo thang đặc quyền cục bộ (Local Privilege Escalation) |
| 2 | Chiếm quyền điều khiển DNS toàn hệ thống qua D-Bus (`SetDNS`) & Phá vỡ Symlink | **CRITICAL** | CWE-15, CWE-59 | Rất cao | DNS Hijacking, Man-in-the-Middle toàn máy |
| 3 | Giao thức Ghép kênh (MPTCP Bonding) truyền tải Plaintext & Rò rỉ Token | **CRITICAL** | CWE-319, CWE-300 | Rất cao | Đánh cắp phiên kết nối, lộ toàn bộ lưu lượng người dùng |
| 4 | Tiêm gói tin IP thô vào Linux Kernel qua TUN không qua kiểm duyệt | **HIGH** | CWE-20, CWE-940 | Trung bình | Vượt rào tường lửa, tấn công dịch vụ localhost |
| 5 | Lỗ hổng Untrusted Search Path trong GUI khi thực thi `pkexec` | **HIGH** | CWE-426 | Trung bình | Leo thang đặc quyền qua tệp thực thi giả mạo |
| 6 | Lỗi phân tích địa chỉ IP/CIDR trong Netlink gây định tuyến nhầm `from all` | **HIGH** | CWE-676, CWE-20 | Cao | Sai lệch bảng định tuyến, rò rỉ dữ liệu |
| 7 | Bỏ qua hoàn toàn giao thức IPv6 gây rò rỉ lưu lượng (IPv6 Leakage) | **MEDIUM** | CWE-200 | Cao | Làm lộ địa chỉ IP thật của người dùng |
| 8 | Rò rỉ bộ nhớ (Memory Leak) trong luồng giải mã gói tin Bonding | **MEDIUM** | CWE-401 | Trung bình | Chiếm dụng tài nguyên RAM, suy giảm hiệu năng |
| 9 | Thiếu mã hóa (Escape) chuỗi giao diện GTK Markup | **LOW** | CWE-116 | Thấp | Lỗi hiển thị giao diện khi gặp ký tự đặc biệt |

---

## 3. CHI TIẾT CÁC LỖ HỔNG VÀ BIỆN PHÁP KHẮC PHỤC

---

### LỖ HỔNG 1: Cấu hình Polkit quá lỏng lẻo dẫn đến Leo thang Đặc quyền (Local Privilege Escalation)
- **Mức độ:** `CRITICAL`
- **Mã lỗi:** `CWE-250` (Execution with Unnecessary Privileges), `CWE-306` (Missing Authentication for Critical Function)
- **Vị trí tệp:**
  - `data/io.github.multiwifi.policy` (Dòng 10-18)
  - `data/io.github.multiwifi.rules` (Dòng 3-9)
- **Phân tích kỹ thuật:**
  1. Trong file policy `io.github.multiwifi.policy`:
     ```xml
     <action id="io.github.multiwifi.modify">
       <defaults>
         <allow_any>no</allow_any>
         <allow_inactive>no</allow_inactive>
         <allow_active>yes</allow_active>
       </defaults>
     </action>
     ```
  2. Trong file JavaScript rules `io.github.multiwifi.rules`:
     ```javascript
     polkit.addRule(function(action, subject) {
         if (action.id == "io.github.multiwifi.modify") {
             if (subject.local && subject.active) {
                 return polkit.Result.YES;
             }
         }
     });
     ```
- **Rủi ro an ninh:**
  Bất kỳ tiến trình nào chạy trong phiên người dùng cục bộ đang hoạt động (kể cả phần mềm độc hại, trình duyệt web bị khai thác RCE, hoặc người dùng bị hạn chế quyền) đều được Polkit tự động cấp quyền `polkit.Result.YES` đối với hành động `io.github.multiwifi.modify`. Tiến trình này có thể trực tiếp gọi các phương thức D-Bus nguy hiểm trên `multiwifi-daemon` (chạy quyền `root`) như `SetDNS`, `StartBonding`, `StopDaemon`, `StartLoadBalancing` mà không cần nhập mật khẩu quản trị.
- **Biện pháp khắc phục:**
  - Sửa `<allow_active>yes</allow_active>` thành `<allow_active>auth_admin</allow_active>` trong `data/io.github.multiwifi.policy`.
  - Trong `data/io.github.multiwifi.rules`, trả về `polkit.Result.AUTH_ADMIN` thay vì `polkit.Result.YES`.

---

### LỖ HỔNG 2: Chiếm quyền điều khiển DNS toàn hệ thống qua D-Bus & Phá vỡ Symlink
- **Mức độ:** `CRITICAL`
- **Mã lỗi:** `CWE-15` (External Control of System Configuration), `CWE-59` (Improper Link Resolution Before File Access)
- **Vị trí tệp:**
  - `src/daemon/service.c` (Dòng 377-458, các hàm `apply_dns_configuration` và `reset_dns_configuration`)
- **Phân tích kỹ thuật:**
  - Kết hợp với Lỗ hổng 1, bất kỳ tiến trình không đặc quyền nào cũng có thể gửi gọi D-Bus `SetDNS(["<attacker-dns-ip>"])`.
  - Daemon xử lý việc này bằng cách ghi đè trực tiếp lên `/etc/NetworkManager/conf.d/99-multiwifi-dns.conf` và `/etc/resolv.conf`:
    ```c
    gboolean ok = g_file_set_contents(resolv_path, resolv_content->str, -1, error);
    ```
- **Rủi ro an ninh:**
  - **DNS Hijacking:** Kẻ tấn công có thể chuyển hướng toàn bộ lưu lượng phân giải tên miền của máy tính (kể cả lưu lượng từ tài khoản `root`, các dịch vụ hệ thống chạy ngầm) về máy chủ DNS do chúng kiểm soát. Điều này cho phép thực hiện tấn công Man-in-the-Middle (MitM), giả mạo cập nhật phần mềm hoặc đánh cắp thông tin tài khoản.
  - **Hỏng cấu hình hệ thống:** `/etc/resolv.conf` trên các hệ điều hành Linux hiện đại là một symlink trỏ đến `systemd-resolved` (`/run/systemd/resolve/stub-resolv.conf`). Lệnh `g_file_set_contents` và cơ chế khôi phục trong `reset_dns_configuration` ghi đè trực tiếp thành file tĩnh, phá vỡ vĩnh viễn symlink này.
- **Biện pháp khắc phục:**
  - Bắt buộc kiểm tra quyền quản trị tối cao (`auth_admin`) trước khi thực hiện đổi DNS.
  - Tương tác với dịch vụ quản lý DNS chính thức của hệ điều hành thông qua D-Bus (`org.freedesktop.resolve1` của `systemd-resolved` hoặc NetworkManager) thay vì trực tiếp can thiệp vào file `/etc/resolv.conf`.

---

### LỖ HỔNG 3: Giao thức Ghép kênh (Bonding Tunnel) truyền tải Plaintext & Rò rỉ Token
- **Mức độ:** `CRITICAL`
- **Mã lỗi:** `CWE-319` (Cleartext Transmission of Sensitive Information), `CWE-300` (Channel Accessible by Non-Endpoint)
- **Vị trí tệp:**
  - `src/core/bonding_client.c` (Dòng 316-368)
  - `src/core/bonding_protocol.c` (Dòng 47-82, 247-252)
- **Phân tích kỹ thuật:**
  - Tài liệu thiết kế kiến trúc (`docs/SECURITY.md` và `docs/NETWORK_DESIGN.md`) quy định bắt buộc phải sử dụng TLS 1.3 hoặc một giao thức vận chuyển đã được kiểm tra an ninh độc lập.
  - Tuy nhiên, trong mã nguồn thực tế, `multiwifi_bonding_client_start` chỉ tạo một socket TCP hoặc MPTCP thuần:
    ```c
    int sock = socket(AF_INET, SOCK_STREAM, IPPROTO_MPTCP);
    ```
  - Quá trình bắt tay xác thực gửi `auth_token` hoàn toàn dưới dạng văn bản rõ (cleartext).
  - Toàn bộ gói tin IP của người dùng (`MULTIWIFI_FRAME_IP_PACKET`) được đóng gói thô và gửi trực tiếp qua mạng internet/Wi-Fi mà không có mã hóa hoặc chữ ký kiểm tra tính toàn vẹn (HMAC).
- **Rủi ro an ninh:**
  Kẻ tấn công nằm cùng mạng Wi-Fi công cộng hoặc trên đường truyền internet có thể:
  1. Đọc trộm được mã token xác thực của phiên làm việc.
  2. Bắt gói tin và đọc toàn bộ dữ liệu duyệt web/nội dung truyền qua đường hầm ghép kênh.
  3. Thực hiện tấn công Man-in-the-Middle sửa đổi dữ liệu truyền nhận.
- **Biện pháp khắc phục:**
  Tích hợp lớp mã hóa bắt buộc TLS 1.3 (thông qua thư viện OpenSSL / GnuTLS) hoặc triển khai đường hầm trên giao thức VPN chuẩn đã được kiểm chứng an ninh (như WireGuard).

---

### LỖ HỔNG 4: Tiêm gói tin IP thô vào Kernel qua TUN không qua kiểm duyệt (Raw Packet Injection)
- **Mức độ:** `HIGH`
- **Mã lỗi:** `CWE-20` (Improper Input Validation), `CWE-940` (Improper Verification of Source)
- **Vị trí tệp:**
  - `src/core/bonding_client.c` (Dòng 216-221)
  - `src/core/tun_device.c` (Dòng 268-289)
- **Phân tích kỹ thuật:**
  - Trong luồng xử lý `bonding_worker_thread`:
    ```c
    if (f->type == MULTIWIFI_FRAME_IP_PACKET && f->length > 0) {
        multiwifi_tun_device_write(self->tun, f->payload, f->length, NULL);
    }
    ```
  - Không có bất kỳ bước thẩm định tính hợp lệ nào của gói tin IP: không kiểm tra IP header, không kiểm tra dải IP nguồn (Anti-Spoofing / Source Address Validation), không kiểm tra checksum.
- **Rủi ro an ninh:**
  Máy chủ ghép kênh độc hại (hoặc kẻ tấn công mạng can thiệp vào đường truyền không mã hóa) có thể gửi các frame chứa gói tin IP có nguồn giả mạo là `127.0.0.1` hoặc các địa chỉ trong mạng LAN nội bộ. Nhân Linux khi nhận được gói tin từ thiết bị TUN sẽ xử lý như một gói tin mạng hợp lệ, cho phép kẻ tấn công vượt qua cấu hình tường lửa (firewall bypass) và gửi payload tấn công các cổng dịch vụ chỉ mở cho `localhost`.
- **Biện pháp khắc phục:**
  Thực hiện kiểm tra cấu trúc gói tin IPv4 tối thiểu:
  1. Kiểm tra phiên bản IP (`version == 4`).
  2. Kiểm tra độ dài `total_length <= f->length`.
  3. Bác bỏ tất cả các gói tin có IP nguồn thuộc dải Loopback (`127.0.0.0/8`) hoặc dải multicast/broadcast không hợp lệ.

---

### LỖ HỔNG 5: Lỗ hổng Untrusted Search Path trong GUI khi thực thi `pkexec`
- **Mức độ:** `HIGH`
- **Mã lỗi:** `CWE-426` (Untrusted Search Path)
- **Vị trí tệp:**
  - `src/gui/window.c` (Dòng 199-219, hàm `find_daemon_executable`)
  - `src/gui/window.c` (Dòng 949-972, hàm `start_daemon_via_pkexec`)
  - `src/gui/window.c` (Dòng 845-878, hàm `stop_daemon_via_pkexec`)
- **Phân tích kỹ thuật:**
  - Hàm `find_daemon_executable()` tìm đường dẫn tệp thực thi qua:
    ```c
    gchar *path = g_find_program_in_path("multiwifi-daemon");
    ```
  - Sau đó GUI gọi:
    ```c
    const gchar *argv[] = { "pkexec", daemon_path, NULL };
    ```
  - Khi dừng daemon, hàm `stop_daemon_via_pkexec()` gọi:
    ```c
    const gchar *argv[] = { "pkexec", "pkill", "-TERM", "-x", "multiwifi-daemon", NULL };
    ```
- **Rủi ro an ninh:**
  - Nếu người dùng chạy ứng dụng với biến `PATH` có chứa thư mục không an toàn (ví dụ `PATH=/tmp:$PATH` hoặc thư mục hiện tại), `pkexec` sẽ thực thi tệp tin giả mạo mang tên `multiwifi-daemon` với quyền `root`.
  - Việc cấp quyền root cho lệnh `pkill` thông qua `pkexec` là tiềm ẩn rủi ro lạm dụng lệnh hệ thống.
- **Biện pháp khắc phục:**
  - Sử dụng đường dẫn tuyệt đối cố định và an toàn: `/usr/bin/multiwifi-daemon` hoặc `/usr/libexec/multiwifi-daemon`.
  - Không sử dụng `pkexec pkill`; thay vào đó hãy sử dụng lệnh gọi phương thức D-Bus `StopDaemon` hoặc tương tác với `systemd` qua bus hệ thống.

---

### LỖ HỔNG 6: Lỗi Phân tích Địa chỉ CIDR trong Netlink gây định tuyến nhầm `from all`
- **Mức độ:** `HIGH`
- **Mã lỗi:** `CWE-676` (Use of Potentially Dangerous Function), `CWE-20` (Improper Input Validation)
- **Vị trí tệp:**
  - `src/core/netlink_route.c` (Dòng 150-156, trong hàm `multiwifi_netlink_add_rule`)
  - `src/core/wifi_inventory.c` (Dòng 186-191)
- **Phân tích kỹ thuật:**
  - `wifi_inventory.c` thu thập địa chỉ IPv4 từ NetworkManager kèm tiền tố prefix CIDR (ví dụ: `"192.168.1.50/24"`).
  - Trong `multiwifi_netlink_add_rule`:
    ```c
    if (src_ip != NULL && *src_ip != '\0') {
        struct in_addr in;
        if (inet_pton(AF_INET, src_ip, &in) == 1) {
            frh->src_len = 32;
            add_rtattr(n, sizeof(buf), FRA_SRC, &in, sizeof(in));
        }
    }
    ```
  - Hàm `inet_pton(AF_INET, "192.168.1.50/24", &in)` sẽ thất bại và trả về `0`. Do đó `FRA_SRC` không bao giờ được thêm vào thông điệp Netlink, và trường `frh->src_len` giữ nguyên giá trị `0`.
- **Rủi ro an ninh:**
  Quy tắc định tuyến được tạo trong kernel mang ý nghĩa `from all lookup 1001` (chuyển hướng tất cả các gói tin ra card mạng đầu tiên). Điều này làm hỏng toàn bộ cơ chế cân bằng tải, biến card mạng thứ nhất thành nút thắt cổ chai, và có thể dẫn đến việc rò rỉ dữ liệu ngoài ý muốn qua card mạng đó.
- **Biện pháp khắc phục:**
  Cắt bỏ phần hậu tố `/<prefix>` trước khi gọi `inet_pton`:
  ```c
  gchar *clean_ip = g_strdup(src_ip);
  gchar *slash = strchr(clean_ip, '/');
  if (slash != NULL) {
      *slash = '\0';
  }
  /* gọi inet_pton với clean_ip */
  g_free(clean_ip);
  ```

---

### LỖ HỔNG 7: Bỏ qua hoàn toàn Giao thức IPv6 (IPv6 Bypass & Leakage)
- **Mức độ:** `MEDIUM`
- **Mã lỗi:** `CWE-200` (Exposure of Sensitive Information)
- **Vị trí tệp:**
  - `src/core/firewall.c` (Dòng 52-62)
  - `src/core/netlink_route.c` (Dòng 130)
- **Phân tích kỹ thuật:**
  - Quy tắc tường lửa nftables chỉ được tạo trên bảng IPv4: `add table ip multiwifi_table`.
  - Quy tắc định tuyến Netlink chỉ tạo cho họ `AF_INET`.
- **Rủi ro an ninh:**
  Trên các hệ thống mạng hỗ trợ cả IPv4 và IPv6 (Dual-stack), toàn bộ các gói tin IPv6 sẽ hoàn toàn bỏ qua các quy tắc phân chia luồng của MultiWiFi, đi thẳng ra ngoài qua gateway IPv6 mặc định của hệ thống. Điều này làm lộ địa chỉ IP thật của người dùng và làm sai lệch kỳ vọng bảo mật của hệ thống.
- **Biện pháp khắc phục:**
  - Mở rộng bảng nftables sang họ `table inet multiwifi_table` để quản lý đồng thời cả IPv4 và IPv6.
  - Hoặc nếu ứng dụng chưa hỗ trợ định tuyến chính sách cho IPv6, phải chủ động chặn lưu lượng IPv6 hoặc thông báo rõ ràng cho người dùng.

---

### LỖ HỔNG 8: Rò rỉ Bộ nhớ (Memory Leak) trong Luồng Giải mã Gói tin Bonding
- **Mức độ:** `MEDIUM`
- **Mã lỗi:** `CWE-401` (Missing Release of Memory after Effective Lifetime)
- **Vị trí tệp:**
  - `src/core/bonding_client.c` (Dòng 231-236, 381-385)
- **Phân tích kỹ thuật:**
  - Khi nhận frame `MULTIWIFI_FRAME_DISCONNECT`:
    ```c
    } else if (f->type == MULTIWIFI_FRAME_DISCONNECT) {
        multiwifi_frame_free(f);
        g_list_free(frames);
        goto thread_exit;
    }
    multiwifi_frame_free(f);
    ```
  - Nếu trong danh sách `frames` còn các frame tiếp theo chưa được xử lý, lệnh `g_list_free(frames)` chỉ giải phóng các con trỏ node danh sách, các đối tượng `MultiWifiFrame` và bộ đệm `payload` của chúng sẽ bị rò rỉ bộ nhớ.
  - Tương tự trong quá trình bắt tay `AUTH_RESP` (dòng 381), chỉ có frame đầu tiên được lưu vào `resp_frame`, các frame còn lại trong danh sách `flist` bị bỏ quên.
- **Rủi ro an ninh:**
  Gây cạn kiệt tài nguyên bộ nhớ hệ thống (Memory Exhaustion) nếu phiên kết nối gặp sự cố liên tục hoặc bị đối phương gửi các gói tin đóng/ngắt kết nối lặp đi lặp lại.
- **Biện pháp khắc phục:**
  Sử dụng hàm giải phóng danh sách kèm hàm hủy phần tử:
  ```c
  g_list_free_full(frames, (GDestroyNotify)multiwifi_frame_free);
  ```

---

### LỖ HỔNG 9: Thiếu Mã Hóa Ký Tự Đặc Biệt trong Chuỗi GTK Markup
- **Mức độ:** `LOW`
- **Mã lỗi:** `CWE-116` (Improper Encoding or Escaping of Output)
- **Vị trí tệp:**
  - `src/gui/window.c` (Dòng 604, 689)
- **Phân tích kỹ thuật:**
  Các trường như `info->ifname` và `info->ipv4_address` được đưa trực tiếp vào `g_strdup_printf` để gán cho `gtk_label_set_markup` mà không qua hàm `g_markup_escape_text()`.
- **Rủi ro an ninh:**
  Nếu tên card mạng hoặc chuỗi IP chứa các ký tự như `<`, `>`, `&`, GTK sẽ báo lỗi parse Pango Markup và không hiển thị được giao diện.
- **Biện pháp khắc phục:**
  Bọc các biến hiển thị bằng `g_markup_escape_text()` trước khi đưa vào template markup.

---

## 4. KẾT LUẬN & KHUYẾN NGHỊ ƯU TIÊN

Ứng dụng MultiWiFi có kiến trúc phân tầng rõ ràng (tách biệt GUI không đặc quyền và Daemon chạy ngầm), tuy nhiên tồn tại các điểm yếu nghiêm trọng cần ưu tiên xử lý ngay trước khi đưa vào sử dụng thực tế:

1. **Ưu tiên 1 (Ngay lập tức):** Thắt chặt cấu hình Polkit (`auth_admin`) để ngăn chặn việc người dùng thông thường thực hiện các thay đổi mạng mức root mà không cần nhập mật khẩu.
2. **Ưu tiên 2:** Thay thế socket thô không mã hóa của Bonding Tunnel bằng kết nối TLS 1.3 hoặc đường hầm VPN có xác thực tính toàn vẹn.
3. **Ưu tiên 3:** Sửa lỗi cắt chuỗi tiền tố `/CIDR` trong `multiwifi_netlink_add_rule` để đảm bảo quy tắc định tuyến nguồn hoạt động chính xác.
4. **Ưu tiên 4:** Cố định đường dẫn nhị phân `/usr/bin/multiwifi-daemon` trong GUI để loại bỏ rủi ro Untrusted Search Path.
