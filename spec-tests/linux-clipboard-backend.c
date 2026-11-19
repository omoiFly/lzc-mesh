// Run with make -C lzc-mesh-client/linux check-clipboard-backend.
// Exercise registry selection without a compositor or access to the clipboard.
#define _GNU_SOURCE
#include <assert.h>
#include "clipboard.c"

App app;
bool app_operational(void) { return false; }
void app_log(const char *format, ...) {}
void net_send_clipboard(const char *text) {}

static int registry_token, seat_token, ext_token, wlr_token;
static unsigned ext_binds, wlr_binds, ext_destroys;

uint32_t __wrap_wl_proxy_get_version(struct wl_proxy *proxy) { return 1; }

struct wl_proxy *__wrap_wl_proxy_marshal_flags(struct wl_proxy *proxy, uint32_t opcode,
    const struct wl_interface *interface, uint32_t version, uint32_t flags, ...) {
    if (flags & WL_MARSHAL_FLAG_DESTROY) {
        assert(proxy == (struct wl_proxy *)&ext_token);
        assert(opcode == EXT_DATA_CONTROL_MANAGER_V1_DESTROY);
        ext_destroys++;
        return NULL;
    }
    assert(proxy == (struct wl_proxy *)&registry_token && opcode == WL_REGISTRY_BIND);
    if (interface == &ext_data_control_manager_v1_interface) {
        assert(version == 1);
        ext_binds++;
        return (struct wl_proxy *)&ext_token;
    }
    assert(interface == &zwlr_data_control_manager_v1_interface && version == 2);
    wlr_binds++;
    return (struct wl_proxy *)&wlr_token;
}

static void advertise(const struct wl_interface *interface, uint32_t name, uint32_t version) {
    assert(clipboard_bind(NULL, (struct wl_registry *)&registry_token, name, interface->name, version));
}

static void reset(void) {
    C.manager = NULL;
    C.seat = NULL;
    C.ext = false;
    ext_binds = wlr_binds = ext_destroys = 0;
}

int main(void) {
    struct wl_seat *seat = (struct wl_seat *)&seat_token;
    const struct wl_interface *ext = &ext_data_control_manager_v1_interface;
    const struct wl_interface *wlr = &zwlr_data_control_manager_v1_interface;

    advertise(ext, 1, 1);
    advertise(wlr, 2, 2);
    clipboard_start(seat);
    assert(clipboard_available() && !C.ext && C.manager == (struct wl_proxy *)&wlr_token);
    assert(ext_binds == 1 && wlr_binds == 1 && ext_destroys == 1);

    reset();
    advertise(wlr, 2, 2);
    advertise(ext, 1, 1);
    clipboard_start(seat);
    assert(clipboard_available() && !C.ext && C.manager == (struct wl_proxy *)&wlr_token);
    assert(ext_binds == 0 && wlr_binds == 1 && ext_destroys == 0);

    reset();
    advertise(ext, 1, 1);
    clipboard_start(seat);
    assert(clipboard_available() && C.ext && C.manager == (struct wl_proxy *)&ext_token);
    advertise(wlr, 2, 2);
    assert(C.ext && C.manager == (struct wl_proxy *)&ext_token);
    assert(ext_binds == 1 && wlr_binds == 0 && ext_destroys == 0);
}
