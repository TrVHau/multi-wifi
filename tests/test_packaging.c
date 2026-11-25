#define _GNU_SOURCE
#include <sched.h>
#include <fcntl.h>
#include <glib.h>
#include <glib/gstdio.h>
#include <gio/gio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include "multiwifi/core/firewall.h"
#include "multiwifi/core/routing_manager.h"
#include "multiwifi/shared/dbus_defs.h"
#include "multiwifi/version.h"

#ifndef MULTIWIFI_SOURCE_DIR
#define MULTIWIFI_SOURCE_DIR ".."
#endif

#ifndef MULTIWIFI_BUILD_DIR
#define MULTIWIFI_BUILD_DIR "."
#endif

static gboolean enter_isolated_network_namespace(void)
{
    uid_t uid = getuid();
    gid_t gid = getgid();

    if (unshare(CLONE_NEWUSER | CLONE_NEWNET) != 0) {
        return FALSE;
    }

    int fd = open("/proc/self/setgroups", O_WRONLY);
    if (fd >= 0) {
        const char *deny = "deny";
        if (write(fd, deny, strlen(deny)) < 0) {
            /* ignore */
        }
        close(fd);
    }

    char map[64];
    fd = open("/proc/self/gid_map", O_WRONLY);
    if (fd >= 0) {
        snprintf(map, sizeof(map), "0 %u 1\n", gid);
        if (write(fd, map, strlen(map)) < 0) {
            /* ignore */
        }
        close(fd);
    }

    fd = open("/proc/self/uid_map", O_WRONLY);
    if (fd >= 0) {
        snprintf(map, sizeof(map), "0 %u 1\n", uid);
        if (write(fd, map, strlen(map)) < 0) {
            /* ignore */
        }
        close(fd);
    }

    return TRUE;
}

static gchar *read_repo_file(const gchar *relative_path)
{
    gchar *full_path = g_build_filename(MULTIWIFI_SOURCE_DIR, relative_path, NULL);
    gchar *contents = NULL;
    GError *error = NULL;

    gboolean ok = g_file_get_contents(full_path, &contents, NULL, &error);
    g_assert_no_error(error);
    g_assert_true(ok);
    g_assert_nonnull(contents);

    g_free(full_path);
    return contents;
}

/* 1. D-Bus Policy and Polkit Action Security Checks */

static void test_dbus_policy_safety(void)
{
    gchar *xml = read_repo_file("data/io.github.multiwifi.Daemon.conf");

    /* Only user root may own the bus name */
    g_assert_nonnull(strstr(xml, "<policy user=\"root\">"));
    g_assert_nonnull(strstr(xml, "<allow own=\"io.github.multiwifi.Daemon\"/>"));

    /* Default context must NOT allow own */
    const gchar *default_policy = strstr(xml, "<policy context=\"default\">");
    g_assert_nonnull(default_policy);
    const gchar *end_default_policy = strstr(default_policy, "</policy>");
    g_assert_nonnull(end_default_policy);

    gchar *def_segment = g_strndup(default_policy, (gsize)(end_default_policy - default_policy));
    g_assert_null(strstr(def_segment, "allow own"));
    g_free(def_segment);

    g_free(xml);
}

static void test_polkit_action_safety(void)
{
    gchar *xml = read_repo_file("data/io.github.multiwifi.policy");

    /* Action ID matches D-Bus implementation */
    g_assert_nonnull(strstr(xml, "action id=\"io.github.multiwifi.modify\""));

    /* Active desktop console user requires admin auth keep; remote/inactive are denied */
    g_assert_nonnull(strstr(xml, "<allow_active>auth_admin_keep</allow_active>"));
    g_assert_nonnull(strstr(xml, "<allow_any>no</allow_any>"));
    g_assert_nonnull(strstr(xml, "<allow_inactive>no</allow_inactive>"));

    g_free(xml);

    /* Verify polkit rules file */
    gchar *rules = read_repo_file("data/io.github.multiwifi.rules");
    g_assert_nonnull(strstr(rules, "io.github.multiwifi.modify"));
    g_assert_nonnull(strstr(rules, "polkit.Result.AUTH_ADMIN_KEEP"));
    g_free(rules);
}

/* 2. Systemd Service and Desktop File Sandboxing */

static void test_systemd_unit_safety(void)
{
    gchar *service = read_repo_file("data/multiwifi-daemon.service");

    /* D-Bus activation type and bus name */
    g_assert_nonnull(strstr(service, "Type=dbus"));
    g_assert_nonnull(strstr(service, "BusName=io.github.multiwifi.Daemon"));
    g_assert_nonnull(strstr(service, "ExecStart=/usr/bin/multiwifi-daemon"));

    /* Sandboxing directives */
    g_assert_nonnull(strstr(service, "ProtectSystem=strict"));
    g_assert_nonnull(strstr(service, "ProtectHome=true"));
    g_assert_nonnull(strstr(service, "CapabilityBoundingSet=CAP_NET_ADMIN CAP_NET_RAW"));

    g_free(service);
}

static void test_desktop_file_safety(void)
{
    gchar *desktop = read_repo_file("data/io.github.multiwifi.MultiWiFi.desktop");

    /* GUI runs unprivileged: must execute multiwifi-gui directly without sudo or pkexec */
    g_assert_nonnull(strstr(desktop, "Exec=multiwifi-gui"));
    g_assert_nonnull(strstr(desktop, "Icon=io.github.multiwifi.MultiWiFi"));
    g_assert_null(strstr(desktop, "sudo"));
    g_assert_null(strstr(desktop, "pkexec"));
    g_assert_nonnull(strstr(desktop, "Type=Application"));

    g_free(desktop);
}

static void test_style_css_integrity(void)
{
    gchar *css = read_repo_file("data/style.css");
    g_assert_nonnull(css);
    g_assert_cmpuint(strlen(css), >, 500);

    /* Verify essential UI classes for modern card styling */
    g_assert_nonnull(strstr(css, ".modern-card"));
    g_assert_nonnull(strstr(css, ".hero-card"));
    g_assert_nonnull(strstr(css, ".badge"));
    g_assert_nonnull(strstr(css, "button.btn-primary"));
    g_assert_nonnull(strstr(css, "button.btn-secondary"));

    g_free(css);
}

/* 3. Arch Linux PKGBUILD and Install Scripts */

static void test_arch_pkgbuild_and_install_integrity(void)
{
    gchar *pkgbuild = read_repo_file("packaging/arch/PKGBUILD");

    g_assert_nonnull(strstr(pkgbuild, "pkgname=multiwifi"));
    g_assert_nonnull(strstr(pkgbuild, "pkgver=" MULTIWIFI_VERSION_STRING));
    g_assert_nonnull(strstr(pkgbuild, "install=multiwifi.install"));
    g_assert_nonnull(strstr(pkgbuild, "'gtk4>=4.10'"));
    g_assert_nonnull(strstr(pkgbuild, "'libnm>=1.40'"));
    g_assert_nonnull(strstr(pkgbuild, "'polkit'"));
    g_assert_nonnull(strstr(pkgbuild, "'nftables'"));

    g_free(pkgbuild);

    gchar *install_script = read_repo_file("packaging/arch/multiwifi.install");

    /* Mandatory rule: Package installation must NOT automatically activate networking modes */
    g_assert_null(strstr(install_script, "systemctl start"));
    g_assert_null(strstr(install_script, "systemctl enable"));
    g_assert_null(strstr(install_script, "ip route"));

    g_free(install_script);
}

/* 4. Debian Packaging and Maintainer Scripts */

static void test_debian_packaging_integrity(void)
{
    gchar *control = read_repo_file("packaging/debian/control");
    g_assert_nonnull(strstr(control, "Package: multiwifi"));
    g_assert_nonnull(strstr(control, "libgtk-4-1"));
    g_assert_nonnull(strstr(control, "libnm0"));
    g_assert_nonnull(strstr(control, "libpolkit-gobject-1-0"));
    g_assert_nonnull(strstr(control, "libnftables1"));
    g_free(control);

    gchar *postinst = read_repo_file("packaging/debian/postinst");
    g_assert_nonnull(strstr(postinst, "chmod 0755 /usr/bin/multiwifi-daemon"));
    /* Postinst must not start daemon or activate routing */
    g_assert_null(strstr(postinst, "systemctl start"));
    g_assert_null(strstr(postinst, "ip route"));
    g_free(postinst);

    gchar *prerm = read_repo_file("packaging/debian/prerm");
    g_assert_nonnull(strstr(prerm, "systemctl stop multiwifi-daemon.service"));
    g_free(prerm);

    gchar *postrm = read_repo_file("packaging/debian/postrm");
    /* Purge vs remove: user configs preserved on remove, cleaned on purge */
    g_assert_nonnull(strstr(postrm, "purge"));
    g_assert_nonnull(strstr(postrm, "rm -rf /etc/multiwifi"));
    g_free(postrm);
}

/* 5. RPM Spec File Integrity */

static void test_rpm_spec_integrity(void)
{
    gchar *spec = read_repo_file("packaging/rpm/multiwifi.spec");

    g_assert_nonnull(strstr(spec, "Name:           multiwifi"));
    g_assert_nonnull(strstr(spec, "Version:        " MULTIWIFI_VERSION_STRING));
    g_assert_nonnull(strstr(spec, "License:        MIT"));
    g_assert_nonnull(strstr(spec, "%systemd_post multiwifi-daemon.service"));
    g_assert_nonnull(strstr(spec, "%systemd_preun multiwifi-daemon.service"));
    g_assert_nonnull(strstr(spec, "%systemd_postun_with_restart multiwifi-daemon.service"));
    g_assert_nonnull(strstr(spec, "%{_bindir}/multiwifi-gui"));
    g_assert_nonnull(strstr(spec, "%{_bindir}/multiwifi-daemon"));

    g_free(spec);
}

/* 6. AppImage Portable GUI Structure and Isolation */

static void test_appimage_structure_and_isolation(void)
{
    gchar *apprun = read_repo_file("packaging/appimage/AppRun");

    /* AppRun must execute multiwifi-gui directly */
    g_assert_nonnull(strstr(apprun, "exec \"${HERE}/usr/bin/multiwifi-gui\""));
    g_assert_null(strstr(apprun, "sudo"));
    g_assert_null(strstr(apprun, "pkexec"));
    g_free(apprun);

    /* Run build-appimage.sh script to generate staged AppDir */
    gchar *script_path = g_build_filename(MULTIWIFI_SOURCE_DIR, "packaging/appimage/build-appimage.sh", NULL);
    gchar *cmd = g_strdup_printf("bash \"%s\" \"%s\"", script_path, MULTIWIFI_BUILD_DIR);
    int ret = system(cmd);
    g_assert_cmpint(ret, ==, 0);
    g_free(cmd);
    g_free(script_path);

    /* Verify AppDir contents */
    gchar *appdir = g_build_filename(MULTIWIFI_BUILD_DIR, "AppDir", NULL);
    gchar *apprun_dest = g_build_filename(appdir, "AppRun", NULL);
    gchar *gui_dest = g_build_filename(appdir, "usr/bin/multiwifi-gui", NULL);
    gchar *daemon_dest = g_build_filename(appdir, "usr/bin/multiwifi-daemon", NULL);

    g_assert_true(g_file_test(apprun_dest, G_FILE_TEST_IS_EXECUTABLE));
    g_assert_true(g_file_test(gui_dest, G_FILE_TEST_IS_EXECUTABLE));

    /* AppImage includes application logo */
    gchar *icon_dest = g_build_filename(appdir, "io.github.multiwifi.MultiWiFi.png", NULL);
    g_assert_true(g_file_test(icon_dest, G_FILE_TEST_EXISTS));
    g_free(icon_dest);

    /* AppImage MUST NOT contain the privileged daemon (GUI only) */
    g_assert_false(g_file_test(daemon_dest, G_FILE_TEST_EXISTS));

    g_free(daemon_dest);
    g_free(gui_dest);
    g_free(apprun_dest);
    g_free(appdir);
}

/* 7. Residual Resource Verification (Clean teardown leaves no residue) */

static void test_clean_uninstall_and_residual_resources(void)
{
    /* If running unprivileged without host CAP_NET_ADMIN, test isolated netns in child process */
    pid_t pid = fork();
    g_assert_cmpint(pid, >=, 0);

    if (pid == 0) {
        if (enter_isolated_network_namespace()) {
            MultiWifiFirewall *fw = multiwifi_firewall_new();
            gboolean exists = FALSE;
            GError *error = NULL;

            /* Verify firewall table is absent if kernel permits netfilter in user namespace */
            if (multiwifi_firewall_verify(fw, &exists, &error)) {
                if (exists) _exit(1);
            }
            g_clear_error(&error);
            g_object_unref(fw);
        }
        _exit(0);
    }

    int status = 0;
    waitpid(pid, &status, 0);
    g_assert_true(WIFEXITED(status));
    g_assert_cmpint(WEXITSTATUS(status), ==, 0);

    /* Verify journal directory does not contain leftover active journals */
    gchar *runtime_journal_dir = g_build_filename(g_get_user_runtime_dir(), "multiwifi", "journals", NULL);
    if (g_file_test(runtime_journal_dir, G_FILE_TEST_IS_DIR)) {
        GDir *dir = g_dir_open(runtime_journal_dir, 0, NULL);
        if (dir != NULL) {
            const gchar *name = NULL;
            while ((name = g_dir_read_name(dir)) != NULL) {
                g_assert_false(g_str_has_suffix(name, ".journal"));
            }
            g_dir_close(dir);
        }
    }
    g_free(runtime_journal_dir);
}

int main(int argc, char *argv[])
{
    g_test_init(&argc, &argv, NULL);

    g_test_add_func("/packaging/dbus_policy_safety", test_dbus_policy_safety);
    g_test_add_func("/packaging/polkit_action_safety", test_polkit_action_safety);
    g_test_add_func("/packaging/systemd_unit_safety", test_systemd_unit_safety);
    g_test_add_func("/packaging/desktop_file_safety", test_desktop_file_safety);
    g_test_add_func("/packaging/style_css_integrity", test_style_css_integrity);
    g_test_add_func("/packaging/arch_pkgbuild_and_install", test_arch_pkgbuild_and_install_integrity);
    g_test_add_func("/packaging/debian_packaging_integrity", test_debian_packaging_integrity);
    g_test_add_func("/packaging/rpm_spec_integrity", test_rpm_spec_integrity);
    g_test_add_func("/packaging/appimage_structure_and_isolation", test_appimage_structure_and_isolation);
    g_test_add_func("/packaging/clean_residual_resources", test_clean_uninstall_and_residual_resources);

    return g_test_run();
}
