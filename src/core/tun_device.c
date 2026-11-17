#define _GNU_SOURCE
#include "multiwifi/core/tun_device.h"
#include <gio/gio.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <net/if.h>
#include <linux/if_tun.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <arpa/inet.h>
#include <string.h>

struct _MultiWifiTunDevice {
    gchar *desired_name;
    gchar *actual_name;
    uint32_t mtu;
    int fd;
    int mock_fd;
    gchar *assigned_ip;
    gchar *assigned_netmask;
};

MultiWifiTunDevice *multiwifi_tun_device_new(const gchar *name, uint32_t mtu)
{
    MultiWifiTunDevice *self = g_new0(MultiWifiTunDevice, 1);
    self->desired_name = g_strdup(name ? name : MULTIWIFI_DEFAULT_TUN_NAME);
    self->actual_name = g_strdup(self->desired_name);
    self->mtu = (mtu > 0) ? mtu : MULTIWIFI_DEFAULT_TUN_MTU;
    self->fd = -1;
    self->mock_fd = -1;
    return self;
}

void multiwifi_tun_device_free(MultiWifiTunDevice *self)
{
    if (self == NULL) {
        return;
    }
    multiwifi_tun_device_close(self);
    g_free(self->desired_name);
    g_free(self->actual_name);
    g_free(self->assigned_ip);
    g_free(self->assigned_netmask);
    g_free(self);
}

void multiwifi_tun_device_set_mock_fd(MultiWifiTunDevice *self, int fd)
{
    if (self == NULL) {
        return;
    }
    self->mock_fd = fd;
}

gboolean multiwifi_tun_device_open(MultiWifiTunDevice *self, GError **error)
{
    if (self == NULL) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "TUN device is NULL");
        return FALSE;
    }

    if (self->mock_fd >= 0) {
        /* Mock backend active for testing */
        return TRUE;
    }

    if (self->fd >= 0) {
        return TRUE; /* Already open */
    }

    int fd = open("/dev/net/tun", O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) {
        int err = errno;
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                    "Failed to open /dev/net/tun: %s (%d)", g_strerror(err), err);
        return FALSE;
    }

    struct ifreq ifr;
    memset(&ifr, 0, sizeof(ifr));
    ifr.ifr_flags = IFF_TUN | IFF_NO_PI;
    g_strlcpy(ifr.ifr_name, self->desired_name, IFNAMSIZ);

    if (ioctl(fd, TUNSETIFF, (void *)&ifr) < 0) {
        int err = errno;
        close(fd);
        if (err == ENODEV) {
            g_set_error(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                        "Kernel TUN module not loaded or unsupported by running kernel");
        } else {
            g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                        "ioctl(TUNSETIFF) failed: %s (%d)", g_strerror(err), err);
        }
        return FALSE;
    }

    self->fd = fd;
    g_free(self->actual_name);
    self->actual_name = g_strdup(ifr.ifr_name);

    /* Set MTU */
    int sock = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (sock >= 0) {
        memset(&ifr, 0, sizeof(ifr));
        g_strlcpy(ifr.ifr_name, self->actual_name, IFNAMSIZ);
        ifr.ifr_mtu = (int)self->mtu;
        ioctl(sock, SIOCSIFMTU, &ifr);
        close(sock);
    }

    return TRUE;
}

void multiwifi_tun_device_close(MultiWifiTunDevice *self)
{
    if (self == NULL) {
        return;
    }
    if (self->fd >= 0) {
        close(self->fd);
        self->fd = -1;
    }
    if (self->mock_fd >= 0) {
        close(self->mock_fd);
        self->mock_fd = -1;
    }
}

gboolean multiwifi_tun_device_is_open(MultiWifiTunDevice *self)
{
    if (self == NULL) {
        return FALSE;
    }
    return (self->fd >= 0 || self->mock_fd >= 0);
}

int multiwifi_tun_device_get_fd(MultiWifiTunDevice *self)
{
    if (self == NULL) {
        return -1;
    }
    return (self->mock_fd >= 0) ? self->mock_fd : self->fd;
}

const gchar *multiwifi_tun_device_get_name(MultiWifiTunDevice *self)
{
    if (self == NULL) {
        return NULL;
    }
    return self->actual_name;
}

uint32_t multiwifi_tun_device_get_mtu(MultiWifiTunDevice *self)
{
    if (self == NULL) {
        return 0;
    }
    return self->mtu;
}

gboolean multiwifi_tun_device_configure_ip(MultiWifiTunDevice *self,
                                           const gchar *ipv4_addr,
                                           const gchar *netmask,
                                           GError **error)
{
    if (self == NULL) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "TUN device is NULL");
        return FALSE;
    }
    if (ipv4_addr == NULL || *ipv4_addr == '\0') {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "IPv4 address is required");
        return FALSE;
    }

    if (self->mock_fd >= 0) {
        g_free(self->assigned_ip);
        self->assigned_ip = g_strdup(ipv4_addr);
        g_free(self->assigned_netmask);
        self->assigned_netmask = g_strdup(netmask ? netmask : "255.255.255.0");
        return TRUE;
    }

    if (self->fd < 0) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND, "TUN device not opened");
        return FALSE;
    }

    int sock = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (sock < 0) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                    "Failed to create configuration socket: %s", g_strerror(errno));
        return FALSE;
    }

    struct ifreq ifr;
    memset(&ifr, 0, sizeof(ifr));
    g_strlcpy(ifr.ifr_name, self->actual_name, IFNAMSIZ);

    struct sockaddr_in *addr = (struct sockaddr_in *)&ifr.ifr_addr;
    addr->sin_family = AF_INET;
    if (inet_pton(AF_INET, ipv4_addr, &addr->sin_addr) <= 0) {
        close(sock);
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                    "Invalid IPv4 address '%s'", ipv4_addr);
        return FALSE;
    }
    if (ioctl(sock, SIOCSIFADDR, &ifr) < 0) {
        int err = errno;
        close(sock);
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                    "ioctl(SIOCSIFADDR) failed: %s", g_strerror(err));
        return FALSE;
    }

    if (netmask != NULL && *netmask != '\0') {
        memset(&ifr, 0, sizeof(ifr));
        g_strlcpy(ifr.ifr_name, self->actual_name, IFNAMSIZ);
        struct sockaddr_in *netm = (struct sockaddr_in *)&ifr.ifr_netmask;
        netm->sin_family = AF_INET;
        if (inet_pton(AF_INET, netmask, &netm->sin_addr) > 0) {
            ioctl(sock, SIOCSIFNETMASK, &ifr);
        }
    }

    memset(&ifr, 0, sizeof(ifr));
    g_strlcpy(ifr.ifr_name, self->actual_name, IFNAMSIZ);
    if (ioctl(sock, SIOCGIFFLAGS, &ifr) >= 0) {
        ifr.ifr_flags |= (IFF_UP | IFF_RUNNING);
        ioctl(sock, SIOCSIFFLAGS, &ifr);
    }

    close(sock);

    g_free(self->assigned_ip);
    self->assigned_ip = g_strdup(ipv4_addr);
    g_free(self->assigned_netmask);
    self->assigned_netmask = g_strdup(netmask ? netmask : "255.255.255.0");
    return TRUE;
}

gssize multiwifi_tun_device_read(MultiWifiTunDevice *self,
                                 guint8 *buf,
                                 gsize max_len,
                                 GError **error)
{
    if (self == NULL || buf == NULL || max_len == 0) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "Invalid arguments to read");
        return -1;
    }
    int active_fd = (self->mock_fd >= 0) ? self->mock_fd : self->fd;
    if (active_fd < 0) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND, "Device is closed");
        return -1;
    }

    ssize_t ret = read(active_fd, buf, max_len);
    if (ret < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return 0;
        }
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "Read error: %s", g_strerror(errno));
        return -1;
    }
    return (gssize)ret;
}

gssize multiwifi_tun_device_write(MultiWifiTunDevice *self,
                                  const guint8 *buf,
                                  gsize len,
                                  GError **error)
{
    if (self == NULL || buf == NULL || len == 0) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "Invalid arguments to write");
        return -1;
    }
    int active_fd = (self->mock_fd >= 0) ? self->mock_fd : self->fd;
    if (active_fd < 0) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND, "Device is closed");
        return -1;
    }

    ssize_t ret = write(active_fd, buf, len);
    if (ret < 0) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "Write error: %s", g_strerror(errno));
        return -1;
    }
    return (gssize)ret;
}
