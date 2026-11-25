#include "multiwifi/core/netlink_route.h"
#include "multiwifi/core/logging.h"
#include <gio/gio.h>
#include <sys/socket.h>
#include <linux/rtnetlink.h>
#include <linux/fib_rules.h>
#include <net/if.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <sys/time.h>

#define MULTIWIFI_NL_BUF_SIZE 8192

static uint32_t next_nl_seq(void)
{
    static gint s_seq = 1;
    return (uint32_t)g_atomic_int_add(&s_seq, 1);
}

int multiwifi_netlink_open(GError **error)
{
    int fd = socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC, NETLINK_ROUTE);
    if (fd < 0) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                    "Failed to open NETLINK_ROUTE socket: %s", g_strerror(errno));
        return -1;
    }

    struct timeval tv = { .tv_sec = 3, .tv_usec = 0 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    struct sockaddr_nl sa;
    memset(&sa, 0, sizeof(sa));
    sa.nl_family = AF_NETLINK;

    if (bind(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                    "Failed to bind NETLINK_ROUTE socket: %s", g_strerror(errno));
        close(fd);
        return -1;
    }

    return fd;
}

void multiwifi_netlink_close(int nl_fd)
{
    if (nl_fd >= 0) {
        close(nl_fd);
    }
}

static void add_rtattr(struct nlmsghdr *n, size_t maxlen, unsigned short type, const void *data, size_t alen)
{
    unsigned short len = (unsigned short)RTA_LENGTH(alen);
    unsigned short rta_len = (unsigned short)RTA_ALIGN(len);

    if (NLMSG_ALIGN(n->nlmsg_len) + rta_len > maxlen) {
        return;
    }

    struct rtattr *rta = (struct rtattr *)(((char *)n) + NLMSG_ALIGN(n->nlmsg_len));
    rta->rta_type = type;
    rta->rta_len = len;
    if (alen > 0 && data != NULL) {
        memcpy(RTA_DATA(rta), data, alen);
    }
    n->nlmsg_len = (uint32_t)(NLMSG_ALIGN(n->nlmsg_len) + rta_len);
}

static gboolean send_and_recv_ack(int nl_fd, struct nlmsghdr *nlh, GError **error)
{
    struct sockaddr_nl sa;
    memset(&sa, 0, sizeof(sa));
    sa.nl_family = AF_NETLINK;

    uint32_t seq = next_nl_seq();
    nlh->nlmsg_seq = seq;
    nlh->nlmsg_flags |= NLM_F_ACK;

    if (sendto(nl_fd, nlh, nlh->nlmsg_len, 0, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                    "Netlink sendto failed: %s", g_strerror(errno));
        return FALSE;
    }

    char buf[MULTIWIFI_NL_BUF_SIZE];
    while (TRUE) {
        ssize_t len;
        do {
            len = recv(nl_fd, buf, sizeof(buf), 0);
        } while (len < 0 && errno == EINTR);

        if (len < 0) {
            g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                        "Netlink recv failed: %s", g_strerror(errno));
            return FALSE;
        }

        size_t rem_len = (size_t)len;
        struct nlmsghdr *resp = (struct nlmsghdr *)buf;
        for (; NLMSG_OK(resp, rem_len); resp = NLMSG_NEXT(resp, rem_len)) {
            if (resp->nlmsg_seq != seq) {
                continue;
            }
            if (resp->nlmsg_type == NLMSG_ERROR) {
                if (NLMSG_PAYLOAD(resp, 0) < sizeof(struct nlmsgerr)) {
                    g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "Truncated NLMSG_ERROR packet");
                    return FALSE;
                }
                struct nlmsgerr *err = (struct nlmsgerr *)NLMSG_DATA(resp);
                if (err->error != 0) {
                    int code = -err->error;
                    g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                                "Netlink returned error: %s (errno %d)", g_strerror(code), code);
                    return FALSE;
                }
                return TRUE;
            }
        }
    }
}

static gboolean parse_src_ip_clean(const gchar *src_ip, struct in_addr *out_in)
{
    if (src_ip == NULL || *src_ip == '\0') {
        return FALSE;
    }

    gchar *clean_ip = g_strdup(src_ip);
    gchar *slash = strchr(clean_ip, '/');
    if (slash != NULL) {
        *slash = '\0';
    }

    int rc = inet_pton(AF_INET, clean_ip, out_in);
    g_free(clean_ip);
    return (rc == 1);
}

gboolean multiwifi_netlink_add_rule(int nl_fd,
                                    uint32_t table_id,
                                    uint32_t pref,
                                    uint32_t fwmark,
                                    uint32_t fwmask,
                                    const gchar *src_ip,
                                    GError **error)
{
    if (nl_fd < 0 || table_id == 0 || pref == 0) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "Invalid rule parameters");
        return FALSE;
    }

    char buf[MULTIWIFI_NL_BUF_SIZE];
    memset(buf, 0, sizeof(buf));

    struct nlmsghdr *n = (struct nlmsghdr *)buf;
    n->nlmsg_len = NLMSG_LENGTH(sizeof(struct fib_rule_hdr));
    n->nlmsg_flags = NLM_F_REQUEST | NLM_F_CREATE | NLM_F_EXCL;
    n->nlmsg_type = RTM_NEWRULE;

    struct fib_rule_hdr *frh = (struct fib_rule_hdr *)NLMSG_DATA(n);
    frh->family = AF_INET;
    frh->action = FR_ACT_TO_TBL;
    frh->flags = 0;

    if (table_id < 256) {
        frh->table = (uint8_t)table_id;
    } else {
        frh->table = RT_TABLE_UNSPEC;
        add_rtattr(n, sizeof(buf), FRA_TABLE, &table_id, sizeof(table_id));
    }

    add_rtattr(n, sizeof(buf), FRA_PRIORITY, &pref, sizeof(pref));

    if (fwmark > 0) {
        add_rtattr(n, sizeof(buf), FRA_FWMARK, &fwmark, sizeof(fwmark));
        if (fwmask > 0) {
            add_rtattr(n, sizeof(buf), FRA_FWMASK, &fwmask, sizeof(fwmask));
        }
    }

    if (src_ip != NULL && *src_ip != '\0') {
        struct in_addr in;
        if (parse_src_ip_clean(src_ip, &in)) {
            frh->src_len = 32;
            add_rtattr(n, sizeof(buf), FRA_SRC, &in, sizeof(in));
        }
    }

    return send_and_recv_ack(nl_fd, n, error);
}

gboolean multiwifi_netlink_del_rule(int nl_fd,
                                    uint32_t table_id,
                                    uint32_t pref,
                                    uint32_t fwmark,
                                    uint32_t fwmask,
                                    const gchar *src_ip,
                                    GError **error)
{
    if (nl_fd < 0 || pref == 0) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "Invalid rule parameters for deletion");
        return FALSE;
    }

    char buf[MULTIWIFI_NL_BUF_SIZE];
    memset(buf, 0, sizeof(buf));

    struct nlmsghdr *n = (struct nlmsghdr *)buf;
    n->nlmsg_len = NLMSG_LENGTH(sizeof(struct fib_rule_hdr));
    n->nlmsg_flags = NLM_F_REQUEST;
    n->nlmsg_type = RTM_DELRULE;

    struct fib_rule_hdr *frh = (struct fib_rule_hdr *)NLMSG_DATA(n);
    frh->family = AF_INET;
    frh->action = FR_ACT_TO_TBL;

    if (table_id < 256) {
        frh->table = (uint8_t)table_id;
    } else {
        frh->table = RT_TABLE_UNSPEC;
        add_rtattr(n, sizeof(buf), FRA_TABLE, &table_id, sizeof(table_id));
    }

    add_rtattr(n, sizeof(buf), FRA_PRIORITY, &pref, sizeof(pref));

    if (fwmark > 0) {
        add_rtattr(n, sizeof(buf), FRA_FWMARK, &fwmark, sizeof(fwmark));
        if (fwmask > 0) {
            add_rtattr(n, sizeof(buf), FRA_FWMASK, &fwmask, sizeof(fwmask));
        }
    }

    if (src_ip != NULL && *src_ip != '\0') {
        struct in_addr in;
        if (parse_src_ip_clean(src_ip, &in)) {
            frh->src_len = 32;
            add_rtattr(n, sizeof(buf), FRA_SRC, &in, sizeof(in));
        }
    }

    return send_and_recv_ack(nl_fd, n, error);
}

gboolean multiwifi_netlink_verify_rule(int nl_fd,
                                       uint32_t table_id,
                                       uint32_t pref,
                                       gboolean *exists,
                                       GError **error)
{
    if (exists == NULL) {
        return FALSE;
    }
    *exists = FALSE;

    if (nl_fd < 0) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "Invalid socket");
        return FALSE;
    }

    char snd_buf[MULTIWIFI_NL_BUF_SIZE];
    memset(snd_buf, 0, sizeof(snd_buf));

    struct nlmsghdr *n = (struct nlmsghdr *)snd_buf;
    n->nlmsg_len = NLMSG_LENGTH(sizeof(struct fib_rule_hdr));
    n->nlmsg_flags = NLM_F_REQUEST | NLM_F_DUMP;
    n->nlmsg_type = RTM_GETRULE;

    struct fib_rule_hdr *frh = (struct fib_rule_hdr *)NLMSG_DATA(n);
    frh->family = AF_INET;

    struct sockaddr_nl sa;
    memset(&sa, 0, sizeof(sa));
    sa.nl_family = AF_NETLINK;

    uint32_t seq = next_nl_seq();
    n->nlmsg_seq = seq;

    if (sendto(nl_fd, n, n->nlmsg_len, 0, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "Failed to send RTM_GETRULE dump request");
        return FALSE;
    }

    char recv_buf[MULTIWIFI_NL_BUF_SIZE];
    gboolean done = FALSE;

    while (!done) {
        ssize_t len = recv(nl_fd, recv_buf, sizeof(recv_buf), 0);
        if (len < 0) {
            if (errno == EINTR) {
                continue;
            }
            g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "Recv error during rule dump: %s", g_strerror(errno));
            return FALSE;
        }

        struct nlmsghdr *resp = (struct nlmsghdr *)recv_buf;
        for (; NLMSG_OK(resp, (uint32_t)len); resp = NLMSG_NEXT(resp, len)) {
            if (resp->nlmsg_seq != seq) {
                continue;
            }
            if (resp->nlmsg_type == NLMSG_DONE) {
                done = TRUE;
                break;
            }
            if (resp->nlmsg_type == NLMSG_ERROR) {
                int code = EIO;
                if (NLMSG_PAYLOAD(resp, 0) >= sizeof(struct nlmsgerr)) {
                    struct nlmsgerr *err = (struct nlmsgerr *)NLMSG_DATA(resp);
                    if (err->error == 0) {
                        done = TRUE;
                        break;
                    }
                    code = -err->error;
                }
                g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                            "Rule dump returned error: %s (errno %d)", g_strerror(code), code);
                return FALSE;
            }

            if (resp->nlmsg_type == RTM_NEWRULE && resp->nlmsg_len >= NLMSG_LENGTH(sizeof(struct fib_rule_hdr))) {
                struct fib_rule_hdr *rule = (struct fib_rule_hdr *)NLMSG_DATA(resp);
                uint32_t r_table = rule->table;
                uint32_t r_pref = 0;

                size_t hdr_len = NLMSG_LENGTH(sizeof(*rule));
                int rta_len = (resp->nlmsg_len >= hdr_len) ? (int)(resp->nlmsg_len - hdr_len) : 0;
                struct rtattr *rta = (struct rtattr *)(((char *)rule) + NLMSG_ALIGN(sizeof(*rule)));

                for (; rta_len >= (int)sizeof(*rta) && RTA_OK(rta, (unsigned int)rta_len); ) {
                    if (rta->rta_type == FRA_TABLE && RTA_PAYLOAD(rta) >= sizeof(uint32_t)) {
                        memcpy(&r_table, RTA_DATA(rta), sizeof(uint32_t));
                    } else if (rta->rta_type == FRA_PRIORITY && RTA_PAYLOAD(rta) >= sizeof(uint32_t)) {
                        memcpy(&r_pref, RTA_DATA(rta), sizeof(uint32_t));
                    }
                    unsigned int aligned = (unsigned int)RTA_ALIGN(rta->rta_len);
                    if (aligned == 0 || (int)aligned > rta_len) {
                        break;
                    }
                    rta_len -= (int)aligned;
                    rta = (struct rtattr *)(((char *)rta) + aligned);
                }

                if (r_pref == pref && (table_id == 0 || r_table == table_id)) {
                    *exists = TRUE;
                }
            }
        }
    }

    return TRUE;
}

gboolean multiwifi_netlink_add_route(int nl_fd,
                                     uint32_t table_id,
                                     const gchar *gateway,
                                     const gchar *ifname,
                                     uint32_t metric,
                                     GError **error)
{
    if (nl_fd < 0 || table_id == 0 || gateway == NULL || ifname == NULL) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "Invalid route parameters");
        return FALSE;
    }

    unsigned int ifindex = if_nametoindex(ifname);
    if (ifindex == 0) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                    "Interface '%s' not found for route addition", ifname);
        return FALSE;
    }

    struct in_addr gw_in;
    if (inet_pton(AF_INET, gateway, &gw_in) != 1) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                    "Invalid gateway IPv4 address '%s'", gateway);
        return FALSE;
    }

    char buf[MULTIWIFI_NL_BUF_SIZE];
    memset(buf, 0, sizeof(buf));

    struct nlmsghdr *n = (struct nlmsghdr *)buf;
    n->nlmsg_len = NLMSG_LENGTH(sizeof(struct rtmsg));
    n->nlmsg_flags = NLM_F_REQUEST | NLM_F_CREATE | NLM_F_EXCL;
    n->nlmsg_type = RTM_NEWROUTE;

    struct rtmsg *rtm = (struct rtmsg *)NLMSG_DATA(n);
    rtm->rtm_family = AF_INET;
    rtm->rtm_table = (table_id < 256) ? (uint8_t)table_id : RT_TABLE_UNSPEC;
    rtm->rtm_protocol = RTPROT_STATIC;
    rtm->rtm_scope = RT_SCOPE_UNIVERSE;
    rtm->rtm_type = RTN_UNICAST;
    rtm->rtm_dst_len = 0; /* default route */

    if (table_id >= 256) {
        add_rtattr(n, sizeof(buf), RTA_TABLE, &table_id, sizeof(table_id));
    }
    add_rtattr(n, sizeof(buf), RTA_GATEWAY, &gw_in, sizeof(gw_in));
    add_rtattr(n, sizeof(buf), RTA_OIF, &ifindex, sizeof(ifindex));
    if (metric > 0) {
        add_rtattr(n, sizeof(buf), RTA_PRIORITY, &metric, sizeof(metric));
    }

    return send_and_recv_ack(nl_fd, n, error);
}

gboolean multiwifi_netlink_del_route(int nl_fd,
                                     uint32_t table_id,
                                     const gchar *gateway,
                                     const gchar *ifname,
                                     GError **error)
{
    if (nl_fd < 0 || table_id == 0 || gateway == NULL || ifname == NULL) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "Invalid route parameters for deletion");
        return FALSE;
    }

    unsigned int ifindex = if_nametoindex(ifname);
    if (ifindex == 0) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                    "Interface '%s' not found for route deletion", ifname);
        return FALSE;
    }

    struct in_addr gw_in;
    if (inet_pton(AF_INET, gateway, &gw_in) != 1) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "Invalid gateway IPv4 address '%s'", gateway);
        return FALSE;
    }

    char buf[MULTIWIFI_NL_BUF_SIZE];
    memset(buf, 0, sizeof(buf));

    struct nlmsghdr *n = (struct nlmsghdr *)buf;
    n->nlmsg_len = NLMSG_LENGTH(sizeof(struct rtmsg));
    n->nlmsg_flags = NLM_F_REQUEST;
    n->nlmsg_type = RTM_DELROUTE;

    struct rtmsg *rtm = (struct rtmsg *)NLMSG_DATA(n);
    rtm->rtm_family = AF_INET;
    rtm->rtm_table = (table_id < 256) ? (uint8_t)table_id : RT_TABLE_UNSPEC;
    rtm->rtm_scope = RT_SCOPE_UNIVERSE;
    rtm->rtm_type = RTN_UNICAST;
    rtm->rtm_dst_len = 0;

    if (table_id >= 256) {
        add_rtattr(n, sizeof(buf), RTA_TABLE, &table_id, sizeof(table_id));
    }
    add_rtattr(n, sizeof(buf), RTA_GATEWAY, &gw_in, sizeof(gw_in));
    add_rtattr(n, sizeof(buf), RTA_OIF, &ifindex, sizeof(ifindex));

    return send_and_recv_ack(nl_fd, n, error);
}

gboolean multiwifi_netlink_add_host_route(int nl_fd,
                                          uint32_t table_id,
                                          const gchar *dst_ip,
                                          const gchar *gateway,
                                          const gchar *ifname,
                                          uint32_t metric,
                                          GError **error)
{
    if (nl_fd < 0 || table_id == 0 || dst_ip == NULL || gateway == NULL || ifname == NULL) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "Invalid host route parameters");
        return FALSE;
    }

    unsigned int ifindex = if_nametoindex(ifname);
    if (ifindex == 0) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                    "Interface '%s' not found for host route", ifname);
        return FALSE;
    }

    struct in_addr dst_in;
    if (inet_pton(AF_INET, dst_ip, &dst_in) != 1) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                    "Invalid destination IPv4 address '%s'", dst_ip);
        return FALSE;
    }

    struct in_addr gw_in;
    if (inet_pton(AF_INET, gateway, &gw_in) != 1) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                    "Invalid gateway IPv4 address '%s'", gateway);
        return FALSE;
    }

    char buf[MULTIWIFI_NL_BUF_SIZE];
    memset(buf, 0, sizeof(buf));

    struct nlmsghdr *n = (struct nlmsghdr *)buf;
    n->nlmsg_len = NLMSG_LENGTH(sizeof(struct rtmsg));
    n->nlmsg_flags = NLM_F_REQUEST | NLM_F_CREATE | NLM_F_REPLACE;
    n->nlmsg_type = RTM_NEWROUTE;

    struct rtmsg *rtm = (struct rtmsg *)NLMSG_DATA(n);
    rtm->rtm_family = AF_INET;
    rtm->rtm_table = (table_id < 256) ? (uint8_t)table_id : RT_TABLE_UNSPEC;
    rtm->rtm_protocol = RTPROT_STATIC;
    rtm->rtm_scope = RT_SCOPE_UNIVERSE;
    rtm->rtm_type = RTN_UNICAST;
    rtm->rtm_dst_len = 32; /* /32 host route */

    if (table_id >= 256) {
        add_rtattr(n, sizeof(buf), RTA_TABLE, &table_id, sizeof(table_id));
    }
    add_rtattr(n, sizeof(buf), RTA_DST, &dst_in, sizeof(dst_in));
    add_rtattr(n, sizeof(buf), RTA_GATEWAY, &gw_in, sizeof(gw_in));
    add_rtattr(n, sizeof(buf), RTA_OIF, &ifindex, sizeof(ifindex));
    if (metric > 0) {
        add_rtattr(n, sizeof(buf), RTA_PRIORITY, &metric, sizeof(metric));
    }

    return send_and_recv_ack(nl_fd, n, error);
}

gboolean multiwifi_netlink_del_host_route(int nl_fd,
                                          uint32_t table_id,
                                          const gchar *dst_ip,
                                          const gchar *gateway,
                                          const gchar *ifname,
                                          GError **error)
{
    if (nl_fd < 0 || table_id == 0 || dst_ip == NULL || gateway == NULL || ifname == NULL) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "Invalid host route parameters for deletion");
        return FALSE;
    }

    unsigned int ifindex = if_nametoindex(ifname);
    if (ifindex == 0) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                    "Interface '%s' not found for host route deletion", ifname);
        return FALSE;
    }

    struct in_addr dst_in;
    if (inet_pton(AF_INET, dst_ip, &dst_in) != 1) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                    "Invalid destination IPv4 address '%s'", dst_ip);
        return FALSE;
    }

    struct in_addr gw_in;
    if (inet_pton(AF_INET, gateway, &gw_in) != 1) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                    "Invalid gateway IPv4 address '%s'", gateway);
        return FALSE;
    }

    char buf[MULTIWIFI_NL_BUF_SIZE];
    memset(buf, 0, sizeof(buf));

    struct nlmsghdr *n = (struct nlmsghdr *)buf;
    n->nlmsg_len = NLMSG_LENGTH(sizeof(struct rtmsg));
    n->nlmsg_flags = NLM_F_REQUEST;
    n->nlmsg_type = RTM_DELROUTE;

    struct rtmsg *rtm = (struct rtmsg *)NLMSG_DATA(n);
    rtm->rtm_family = AF_INET;
    rtm->rtm_table = (table_id < 256) ? (uint8_t)table_id : RT_TABLE_UNSPEC;
    rtm->rtm_scope = RT_SCOPE_UNIVERSE;
    rtm->rtm_type = RTN_UNICAST;
    rtm->rtm_dst_len = 32;

    if (table_id >= 256) {
        add_rtattr(n, sizeof(buf), RTA_TABLE, &table_id, sizeof(table_id));
    }
    add_rtattr(n, sizeof(buf), RTA_DST, &dst_in, sizeof(dst_in));
    add_rtattr(n, sizeof(buf), RTA_GATEWAY, &gw_in, sizeof(gw_in));
    add_rtattr(n, sizeof(buf), RTA_OIF, &ifindex, sizeof(ifindex));

    return send_and_recv_ack(nl_fd, n, error);
}

gboolean multiwifi_netlink_verify_route(int nl_fd,
                                        uint32_t table_id,
                                        const gchar *gateway,
                                        const gchar *ifname,
                                        gboolean *exists,
                                        GError **error)
{
    if (exists == NULL) {
        return FALSE;
    }
    *exists = FALSE;

    if (nl_fd < 0 || table_id == 0) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "Invalid arguments for verify route");
        return FALSE;
    }

    unsigned int target_oif = ifname ? if_nametoindex(ifname) : 0;
    struct in_addr target_gw;
    gboolean has_gw = FALSE;
    if (gateway != NULL && *gateway != '\0') {
        if (inet_pton(AF_INET, gateway, &target_gw) == 1) {
            has_gw = TRUE;
        }
    }

    char snd_buf[MULTIWIFI_NL_BUF_SIZE];
    memset(snd_buf, 0, sizeof(snd_buf));

    struct nlmsghdr *n = (struct nlmsghdr *)snd_buf;
    n->nlmsg_len = NLMSG_LENGTH(sizeof(struct rtmsg));
    n->nlmsg_flags = NLM_F_REQUEST | NLM_F_DUMP;
    n->nlmsg_type = RTM_GETROUTE;

    struct rtmsg *rtm = (struct rtmsg *)NLMSG_DATA(n);
    rtm->rtm_family = AF_INET;

    if (table_id < 256) {
        rtm->rtm_table = (uint8_t)table_id;
    } else {
        rtm->rtm_table = RT_TABLE_UNSPEC;
        add_rtattr(n, sizeof(snd_buf), RTA_TABLE, &table_id, sizeof(table_id));
    }

    struct sockaddr_nl sa;
    memset(&sa, 0, sizeof(sa));
    sa.nl_family = AF_NETLINK;

    uint32_t seq = next_nl_seq();
    n->nlmsg_seq = seq;

    if (sendto(nl_fd, n, n->nlmsg_len, 0, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "Failed to send RTM_GETROUTE dump request");
        return FALSE;
    }

    char recv_buf[MULTIWIFI_NL_BUF_SIZE];
    gboolean done = FALSE;

    while (!done) {
        ssize_t len = recv(nl_fd, recv_buf, sizeof(recv_buf), 0);
        if (len < 0) {
            if (errno == EINTR) {
                continue;
            }
            g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "Recv error during route dump: %s", g_strerror(errno));
            return FALSE;
        }

        size_t rem_len = (size_t)len;
        struct nlmsghdr *resp = (struct nlmsghdr *)recv_buf;
        for (; NLMSG_OK(resp, rem_len); resp = NLMSG_NEXT(resp, rem_len)) {
            if (resp->nlmsg_seq != seq) {
                continue;
            }
            if (resp->nlmsg_type == NLMSG_DONE) {
                done = TRUE;
                break;
            }
            if (resp->nlmsg_type == NLMSG_ERROR) {
                if (NLMSG_PAYLOAD(resp, 0) >= sizeof(struct nlmsgerr)) {
                    struct nlmsgerr *err = (struct nlmsgerr *)NLMSG_DATA(resp);
                    int err_code = (err->error < 0) ? -err->error : err->error;
                    if (err_code != 0) {
                        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                                    "Route dump returned error: %s", g_strerror(err_code));
                        return FALSE;
                    }
                }
                g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "Route dump returned error");
                return FALSE;
            }

            if (resp->nlmsg_type == RTM_NEWROUTE &&
                NLMSG_PAYLOAD(resp, 0) >= sizeof(struct rtmsg)) {
                struct rtmsg *r = (struct rtmsg *)NLMSG_DATA(resp);
                if (r->rtm_family != AF_INET) {
                    continue;
                }
                uint32_t r_table = r->rtm_table;
                uint32_t r_oif = 0;
                struct in_addr r_gw;
                memset(&r_gw, 0, sizeof(r_gw));
                gboolean r_has_gw = FALSE;

                size_t hdr_len = NLMSG_LENGTH(sizeof(*r));
                size_t rta_len = (resp->nlmsg_len >= hdr_len) ? (size_t)(resp->nlmsg_len - hdr_len) : 0;
                struct rtattr *rta = (struct rtattr *)(((char *)r) + NLMSG_ALIGN(sizeof(*r)));

                while (rta_len >= sizeof(struct rtattr) &&
                       rta->rta_len >= sizeof(struct rtattr) &&
                       rta->rta_len <= rta_len) {
                    size_t payload_len = RTA_PAYLOAD(rta);
                    if (rta->rta_type == RTA_TABLE && payload_len >= sizeof(uint32_t)) {
                        memcpy(&r_table, RTA_DATA(rta), sizeof(r_table));
                    } else if (rta->rta_type == RTA_OIF && payload_len >= sizeof(uint32_t)) {
                        memcpy(&r_oif, RTA_DATA(rta), sizeof(r_oif));
                    } else if (rta->rta_type == RTA_GATEWAY && payload_len >= sizeof(r_gw)) {
                        memcpy(&r_gw, RTA_DATA(rta), sizeof(r_gw));
                        r_has_gw = TRUE;
                    }
                    size_t step = RTA_ALIGN(rta->rta_len);
                    if (step > rta_len) {
                        break;
                    }
                    rta = (struct rtattr *)(((char *)rta) + step);
                    rta_len -= step;
                }

                if (r_table == table_id) {
                    gboolean match = TRUE;
                    if (target_oif > 0 && r_oif != target_oif) {
                        match = FALSE;
                    }
                    if (has_gw) {
                        if (!r_has_gw || memcmp(&r_gw, &target_gw, sizeof(target_gw)) != 0) {
                            match = FALSE;
                        }
                    }
                    if (match) {
                        *exists = TRUE;
                    }
                }
            }
        }
    }

    return TRUE;
}

gboolean multiwifi_netlink_table_has_routes(int nl_fd,
                                            uint32_t table_id,
                                            gboolean *has_routes,
                                            GError **error)
{
    if (has_routes == NULL) {
        return FALSE;
    }
    *has_routes = FALSE;

    /* Verify if any route exists in table_id */
    return multiwifi_netlink_verify_route(nl_fd, table_id, NULL, NULL, has_routes, error);
}
