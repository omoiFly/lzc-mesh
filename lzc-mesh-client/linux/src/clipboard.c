// Shared clipboard on Linux through Wayland data-control: watches the selection
// without needing focus, and sets it when content arrives from another device.
#define _GNU_SOURCE
#include <fcntl.h>
#include <glib-unix.h>
#include <gio/gunixoutputstream.h>
#include <string.h>
#include <unistd.h>
#include <wayland-client.h>

#include "app.h"
#include "clipboard.h"
#include "clipboard_image.h"
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
    GBytes *owned;
    GCancellable *owned_cancel;
    char *last;       // last text seen or set, used to suppress echoes
    guint read_watch;
    int read_fd, image_fd;
    bool active, skip_initial;
    GString *reading;
} C = {.read_fd = -1, .image_fd = -1};

static bool enabled(void) { return C.active && app.clipboard_enabled && app_operational(); }

static void stop_read(void) {
    if (C.read_watch) { g_source_remove(C.read_watch); C.read_watch = 0; }
    if (C.read_fd >= 0) { close(C.read_fd); C.read_fd = -1; }
    if (C.image_fd >= 0) { close(C.image_fd); C.image_fd = -1; }
    if (C.reading) { g_string_free(C.reading, TRUE); C.reading = NULL; }
}

static void remember(const char *text) {
    g_free(C.last);
    C.last = g_strdup(text);
}

static void start_image_read(void) {
    if (C.image_fd < 0) return;
    int fd = C.image_fd;
    C.image_fd = -1;
    clipboard_image_copy(fd);
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
    start_image_read();
    return G_SOURCE_REMOVE;
}

// The two protocols have equivalent clipboard operations. Generated wrappers
// marshal each request with the interface of the selected backend.
#define DATA_CONTROL(type, request, proxy, ...) \
    (C.ext ? ext_data_control_##type##_v1_##request((struct ext_data_control_##type##_v1 *)(proxy), ##__VA_ARGS__) : \
           zwlr_data_control_##type##_v1_##request((struct zwlr_data_control_##type##_v1 *)(proxy), ##__VA_ARGS__))

static const char *const mimes[] = {"text/plain;charset=utf-8", "UTF8_STRING", "text/plain", "TEXT", "STRING"};
static const char png_mime[] = "image/png";
static const char own_mime[] = "application/x-lzc-mesh-clipboard-owner";
enum { MIME_TEXT = 7, MIME_SECRET = 1 << 3, MIME_PNG = 1 << 4, MIME_OWN = 1 << 5, MIME_FILE = 1 << 6 };

static void offer_mime(struct wl_proxy *offer, const char *mime) {
    unsigned flags = GPOINTER_TO_UINT(wl_proxy_get_user_data(offer));
    for (unsigned i = 0; i < 3; i++) {
        if (!strcmp(mime, mimes[i])) flags |= 1u << i;
    }
    // Password managers mark secrets; never forward them.
    if (!strcmp(mime, "x-kde-passwordManagerHint") || !strcmp(mime, "application/x-nspasteboard-concealed-type"))
        flags |= MIME_SECRET;
    if (!strcmp(mime, png_mime)) flags |= MIME_PNG;
    if (!strcmp(mime, own_mime)) flags |= MIME_OWN;
    if (!strcmp(mime, "text/uri-list")) flags |= MIME_FILE;
    wl_proxy_set_user_data(offer, GUINT_TO_POINTER(flags));
}

static int request_selection(struct wl_proxy *offer, const char *mime) {
    int fds[2];
    if (pipe2(fds, O_CLOEXEC) != 0) return -1;
    DATA_CONTROL(offer, receive, offer, mime, fds[1]);
    close(fds[1]);
    return fds[0];
}

static void dev_selection(struct wl_proxy *offer) {
    bool skip = C.skip_initial;
    C.skip_initial = false;
    unsigned flags = offer ? GPOINTER_TO_UINT(wl_proxy_get_user_data(offer)) : 0;
    if (!skip && !(flags & MIME_OWN) && (offer || !C.source)) {
        clipboard_image_cancel();
        stop_read();
        if (offer && !(flags & MIME_SECRET) && enabled()) {
            bool image = app.clipboard_image && (flags & MIME_PNG) && !(flags & MIME_FILE);
            if (flags & MIME_TEXT)
                C.read_fd = request_selection(offer, mimes[g_bit_nth_lsf(flags & MIME_TEXT, -1)]);
            if (image) C.image_fd = request_selection(offer, png_mime);
            if (C.read_fd >= 0) {
                C.reading = g_string_new(NULL);
                C.read_watch = g_unix_fd_add(C.read_fd, G_IO_IN | G_IO_HUP | G_IO_ERR, on_read, NULL);
            } else {
                if (image) g_clear_pointer(&C.last, g_free);
                start_image_read();
            }
            wl_display_flush(C.display);
        }
    }
    if (offer) DATA_CONTROL(offer, destroy, offer);
}

static void dev_finished(struct wl_proxy *device) {
    DATA_CONTROL(device, destroy, device);
    C.device = NULL;
    clipboard_image_cancel();
    stop_read();
}

// ------------------------------------------------------------ incoming (remote copy)

static void src_write(GTask *task, gpointer object, gpointer data, GCancellable *cancel) {
    GOutputStream *stream = G_OUTPUT_STREAM(object);
    gsize size;
    const void *bytes = g_bytes_get_data(data, &size);
    bool written = true;
    while (size && written) {
        gsize amount = MIN(size, 4096);
        written = g_output_stream_write_all(stream, bytes, amount, NULL, cancel, NULL);
        bytes = (const char *)bytes + amount;
        size -= amount;
    }
    g_output_stream_close(stream, NULL, NULL);
    g_task_return_boolean(task, written);
}

static void src_send(struct wl_proxy *source, const char *mime, int32_t fd) {
    if (C.source != source || !C.owned || !strcmp(mime, own_mime)) {
        close(fd);
        return;
    }
    GOutputStream *stream = g_unix_output_stream_new(fd, TRUE);
    GTask *task = g_task_new(stream, C.owned_cancel, NULL, NULL);
    g_task_set_task_data(task, g_bytes_ref(C.owned), (GDestroyNotify)g_bytes_unref);
    g_task_run_in_thread(task, src_write);
    g_object_unref(task);
    g_object_unref(stream);
}

static void clear_owned(void) {
    if (C.owned_cancel) g_cancellable_cancel(C.owned_cancel);
    g_clear_object(&C.owned_cancel);
    g_clear_pointer(&C.owned, g_bytes_unref);
}

static void src_cancelled(struct wl_proxy *s) {
    DATA_CONTROL(source, destroy, s);
    if (C.source == s) {
        C.source = NULL;
        clear_owned();
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
        src_send((struct wl_proxy *)source, mime, fd); \
    } \
    static void prefix##_cancelled(void *d, struct prefix##_source_v1 *source) { \
        src_cancelled((struct wl_proxy *)source); \
    } \
    static const struct prefix##_source_v1_listener prefix##_source_listener = {prefix##_send, prefix##_cancelled};

DATA_CONTROL_LISTENERS(ext_data_control)
DATA_CONTROL_LISTENERS(zwlr_data_control)

static void set_selection(GBytes *bytes, bool image) {
    stop_read();
    if (C.source) DATA_CONTROL(source, destroy, C.source);
    clear_owned();
    C.owned = g_bytes_ref(bytes);
    C.owned_cancel = g_cancellable_new();
    if (C.ext) {
        struct ext_data_control_source_v1 *source = ext_data_control_manager_v1_create_data_source((void *)C.manager);
        ext_data_control_source_v1_add_listener(source, &ext_data_control_source_listener, NULL);
        C.source = (struct wl_proxy *)source;
    } else {
        struct zwlr_data_control_source_v1 *source = zwlr_data_control_manager_v1_create_data_source((void *)C.manager);
        zwlr_data_control_source_v1_add_listener(source, &zwlr_data_control_source_listener, NULL);
        C.source = (struct wl_proxy *)source;
    }
    DATA_CONTROL(source, offer, C.source, own_mime);
    if (image) DATA_CONTROL(source, offer, C.source, png_mime);
    else for (size_t i = 0; i < G_N_ELEMENTS(mimes); i++) DATA_CONTROL(source, offer, C.source, mimes[i]);
    if (C.ext) ext_data_control_device_v1_set_selection((void *)C.device, (void *)C.source);
    else zwlr_data_control_device_v1_set_selection((void *)C.device, (void *)C.source);
    wl_display_flush(C.display);
}

void clipboard_set(const char *text, const char *from) {
    if (!enabled() || !C.device || !text || !*text || !g_utf8_validate(text, -1, NULL)) return;
    if (g_strcmp0(text, C.last) == 0) return;
    clipboard_image_cancel();
    remember(text);
    GBytes *bytes = g_bytes_new(text, strlen(text));
    set_selection(bytes, false);
    g_bytes_unref(bytes);
    app_log("已接收来自 %s 的剪贴板（%zu 字节）", from && *from ? from : "其他设备", strlen(text));
}

void clipboard_set_png(GBytes *png, const char *from) {
    if (!enabled() || !C.device) return;
    g_clear_pointer(&C.last, g_free);
    set_selection(png, true);
    app_log("已接收来自 %s 的剪贴板图片", from && *from ? from : "其他设备");
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
        clipboard_image_cancel();
        stop_read();
        if (C.device) { DATA_CONTROL(device, destroy, C.device); C.device = NULL; }
        if (C.source) { DATA_CONTROL(source, destroy, C.source); C.source = NULL; }
        clear_owned();
        g_clear_pointer(&C.last, g_free);
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
