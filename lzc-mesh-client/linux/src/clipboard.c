// Shared clipboard on Linux through Wayland data-control: watches the selection
// without needing focus, and sets it when text arrives from another device.
#define _GNU_SOURCE
#include <fcntl.h>
#include <glib-unix.h>
#include <string.h>
#include <unistd.h>
#include <wayland-client.h>

#include "app.h"
#include "clipboard.h"
#include "net.h"
#include "ext-data-control-v1-client-protocol.h"
#include "wlr-data-control-unstable-v1-client-protocol.h"

#define MAX_TEXT (256 * 1024)

static struct {
    struct wl_display *display;
    struct wl_seat *seat;
    struct wl_proxy *manager;
    struct wl_proxy *device;
    struct wl_proxy *source;
    bool ext;
    char *owned;      // text we are currently offering (from a remote device)
    char *last;       // last text seen or set, used to suppress echoes
    guint read_watch;
    int read_fd;
    bool active, skip_initial;
    GString *reading;
} C = {.read_fd = -1};

static bool enabled(void) { return C.active && app.clipboard_enabled && app_operational(); }

static void remember(const char *text) {
    g_free(C.last);
    C.last = g_strdup(text);
}

// ------------------------------------------------------------ outgoing (local copy)

static gboolean on_read(gint fd, GIOCondition cond, gpointer data) {
    char buf[8192];
    ssize_t n = read(fd, buf, sizeof buf);
    if (n > 0 && C.reading->len + n <= MAX_TEXT) {
        g_string_append_len(C.reading, buf, n);
        return G_SOURCE_CONTINUE;
    }
    bool ok = n == 0;
    close(fd);
    C.read_watch = 0; C.read_fd = -1;
    if (ok && C.reading->len && g_utf8_validate(C.reading->str, C.reading->len, NULL) && g_strcmp0(C.reading->str, C.last) != 0) {
        remember(C.reading->str);
        if (enabled()) {
            net_send_clipboard(C.reading->str);
            app_log("已同步本机剪贴板（%zu 字节）", C.reading->len);
        }
    }
    g_string_free(C.reading, TRUE);
    C.reading = NULL;
    return G_SOURCE_REMOVE;
}

// The two protocols have equivalent clipboard operations. Generated wrappers
// marshal each request with the interface of the selected backend.
#define DATA_CONTROL(type, request, proxy, ...) \
    (C.ext ? ext_data_control_##type##_v1_##request((struct ext_data_control_##type##_v1 *)(proxy), ##__VA_ARGS__) : \
           zwlr_data_control_##type##_v1_##request((struct zwlr_data_control_##type##_v1 *)(proxy), ##__VA_ARGS__))

static const char *const mimes[] = {"text/plain;charset=utf-8", "UTF8_STRING", "text/plain", "TEXT", "STRING"};
enum { MIME_SECRET = 1 << 3 };

static void offer_mime(struct wl_proxy *offer, const char *mime) {
    unsigned flags = GPOINTER_TO_UINT(wl_proxy_get_user_data(offer));
    for (unsigned i = 0; i < 3; i++) {
        if (!strcmp(mime, mimes[i])) flags |= 1u << i;
    }
    // Password managers mark secrets; never forward them.
    if (!strcmp(mime, "x-kde-passwordManagerHint") || !strcmp(mime, "application/x-nspasteboard-concealed-type"))
        flags |= MIME_SECRET;
    wl_proxy_set_user_data(offer, GUINT_TO_POINTER(flags));
}

static void dev_selection(struct wl_proxy *offer) {
    bool skip = C.skip_initial;
    C.skip_initial = false;
    if (!offer) return;
    unsigned flags = GPOINTER_TO_UINT(wl_proxy_get_user_data(offer));
    if (!skip && !C.source && flags && !(flags & MIME_SECRET) && enabled() && !C.read_watch) {
        const char *mime = mimes[g_bit_nth_lsf(flags, -1)];
        int fds[2];
        if (pipe2(fds, O_CLOEXEC) == 0) {
            DATA_CONTROL(offer, receive, offer, mime, fds[1]);
            close(fds[1]);
            wl_display_flush(C.display);
            C.reading = g_string_new(NULL);
            C.read_fd = fds[0];
            C.read_watch = g_unix_fd_add(fds[0], G_IO_IN | G_IO_HUP | G_IO_ERR, on_read, NULL);
        }
    }
    DATA_CONTROL(offer, destroy, offer);
}

static void dev_finished(struct wl_proxy *device) {
    DATA_CONTROL(device, destroy, device);
    C.device = NULL;
}

// ------------------------------------------------------------ incoming (remote copy)

static void src_send(int32_t fd) {
    if (C.owned) {
        const char *p = C.owned;
        size_t left = strlen(p);
        while (left) {
            ssize_t n = write(fd, p, left);
            if (n <= 0) break;
            p += n;
            left -= n;
        }
    }
    close(fd);
}
static void src_cancelled(struct wl_proxy *s) {
    DATA_CONTROL(source, destroy, s);
    if (C.source == s) {
        C.source = NULL;
        g_clear_pointer(&C.owned, g_free);
    }
}
// Keep typed listener signatures while sharing selection and privacy handling.
#define DATA_CONTROL_LISTENERS(prefix) \
    static void prefix##_mime(void *d, struct prefix##_offer_v1 *offer, const char *mime) { \
        offer_mime((struct wl_proxy *)offer, mime); \
    } \
    static const struct prefix##_offer_v1_listener prefix##_offer_listener = {prefix##_mime}; \
    static void prefix##_offer(void *d, struct prefix##_device_v1 *device, struct prefix##_offer_v1 *offer) { \
        prefix##_offer_v1_add_listener(offer, &prefix##_offer_listener, NULL); \
    } \
    static void prefix##_selection(void *d, struct prefix##_device_v1 *device, struct prefix##_offer_v1 *offer) { \
        dev_selection((struct wl_proxy *)offer); \
    } \
    static void prefix##_finished(void *d, struct prefix##_device_v1 *device) { \
        dev_finished((struct wl_proxy *)device); \
    } \
    static void prefix##_primary(void *d, struct prefix##_device_v1 *device, struct prefix##_offer_v1 *offer) { \
        if (offer) prefix##_offer_v1_destroy(offer); \
    } \
    static const struct prefix##_device_v1_listener prefix##_device_listener = { \
        prefix##_offer, prefix##_selection, prefix##_finished, prefix##_primary \
    }; \
    static void prefix##_send(void *d, struct prefix##_source_v1 *source, const char *mime, int32_t fd) { \
        src_send(fd); \
    } \
    static void prefix##_cancelled(void *d, struct prefix##_source_v1 *source) { \
        src_cancelled((struct wl_proxy *)source); \
    } \
    static const struct prefix##_source_v1_listener prefix##_source_listener = {prefix##_send, prefix##_cancelled};

DATA_CONTROL_LISTENERS(ext_data_control)
DATA_CONTROL_LISTENERS(zwlr_data_control)

void clipboard_set(const char *text, const char *from) {
    if (!enabled() || !C.device || !text || !*text || !g_utf8_validate(text, -1, NULL)) return;
    if (g_strcmp0(text, C.last) == 0) return;
    remember(text);
    if (C.source) DATA_CONTROL(source, destroy, C.source);
    g_free(C.owned);
    C.owned = g_strdup(text);
    if (C.ext) {
        struct ext_data_control_source_v1 *source = ext_data_control_manager_v1_create_data_source((void *)C.manager);
        ext_data_control_source_v1_add_listener(source, &ext_data_control_source_listener, NULL);
        C.source = (struct wl_proxy *)source;
    } else {
        struct zwlr_data_control_source_v1 *source = zwlr_data_control_manager_v1_create_data_source((void *)C.manager);
        zwlr_data_control_source_v1_add_listener(source, &zwlr_data_control_source_listener, NULL);
        C.source = (struct wl_proxy *)source;
    }
    for (size_t i = 0; i < G_N_ELEMENTS(mimes); i++) DATA_CONTROL(source, offer, C.source, mimes[i]);
    if (C.ext) ext_data_control_device_v1_set_selection((void *)C.device, (void *)C.source);
    else zwlr_data_control_device_v1_set_selection((void *)C.device, (void *)C.source);
    wl_display_flush(C.display);
    app_log("已接收来自 %s 的剪贴板（%zu 字节）", from && *from ? from : "其他设备", strlen(text));
}

bool clipboard_bind(struct wl_display *display, struct wl_registry *registry, uint32_t name, const char *iface, uint32_t version) {
    bool ext = !strcmp(iface, ext_data_control_manager_v1_interface.name);
    bool wlr = !strcmp(iface, zwlr_data_control_manager_v1_interface.name);
    if (!ext && !wlr) return false;
    if (C.manager) {
        // Prefer the existing wlroots backend when both globals are advertised
        // during startup. Keep the selected backend once the seat is in use.
        if (ext || !C.ext || C.seat) return true;
        ext_data_control_manager_v1_destroy((struct ext_data_control_manager_v1 *)C.manager);
    }
    C.display = display;
    C.ext = ext;
    C.manager = wl_registry_bind(registry, name,
                                 ext ? &ext_data_control_manager_v1_interface : &zwlr_data_control_manager_v1_interface,
                                 ext ? 1 : MIN(version, 2));
    return true;
}

void clipboard_start(struct wl_seat *seat) {
    C.seat = seat;
    if (app_operational()) clipboard_set_active(true);
}

void clipboard_set_active(bool on) {
    C.active = on;
    if (!on) {
        if (C.read_watch) { g_source_remove(C.read_watch); C.read_watch = 0; }
        if (C.read_fd >= 0) { close(C.read_fd); C.read_fd = -1; }
        if (C.reading) { g_string_free(C.reading, TRUE); C.reading = NULL; }
        if (C.device) { DATA_CONTROL(device, destroy, C.device); C.device = NULL; }
        if (C.source) { DATA_CONTROL(source, destroy, C.source); C.source = NULL; }
        g_clear_pointer(&C.owned, g_free); g_clear_pointer(&C.last, g_free);
    } else if (C.manager && C.seat && !C.device) {
        C.skip_initial = true; // Existing clipboard content is not a new copy after wake.
        if (C.ext) {
            struct ext_data_control_device_v1 *device = ext_data_control_manager_v1_get_data_device((void *)C.manager, C.seat);
            ext_data_control_device_v1_add_listener(device, &ext_data_control_device_listener, NULL);
            C.device = (struct wl_proxy *)device;
        } else {
            struct zwlr_data_control_device_v1 *device = zwlr_data_control_manager_v1_get_data_device((void *)C.manager, C.seat);
            zwlr_data_control_device_v1_add_listener(device, &zwlr_data_control_device_listener, NULL);
            C.device = (struct wl_proxy *)device;
        }
    }
    if (C.display) wl_display_flush(C.display);
}

bool clipboard_available(void) { return C.manager && C.seat; }
