#include <gio/gunixinputstream.h>
#include <libsoup/soup.h>
#include <unistd.h>

#include "../../transport/clipboard_png.h"
#include "app.h"
#include "clipboard.h"
#include "clipboard_image.h"
#include "files.h"
#include "net.h"

static SoupSession *session;
static GCancellable *current;
static bool confirming;

typedef struct {
    GCancellable *cancel;
    GInputStream *in;
    GFile *file;
    gint64 size;
    int dimensions;
    char *from;
    uint8_t *png;
} Transfer;

static bool ready(void) {
    return app.clipboard_image && app.clipboard_enabled && app_operational() && clipboard_available();
}

static bool valid(Transfer *transfer) {
    return ready() && current == transfer->cancel && !g_cancellable_is_cancelled(current);
}

void clipboard_image_cancel(void) {
    if (current) g_cancellable_cancel(current);
    g_clear_object(&current);
}

static Transfer *start(void) {
    clipboard_image_cancel();
    current = g_cancellable_new();
    Transfer *transfer = g_new0(Transfer, 1);
    transfer->cancel = g_object_ref(current);
    return transfer;
}

static void transfer_free(Transfer *transfer) {
    if (transfer->file) g_file_delete(transfer->file, NULL, NULL);
    g_clear_object(&transfer->file);
    g_clear_object(&transfer->in);
    g_object_unref(transfer->cancel);
    g_free(transfer->from);
    g_free(transfer->png);
    g_free(transfer);
}

static SoupMessage *request(const char *method, const char *id) {
    if (!session) session = soup_session_new_with_options("timeout", 60, NULL);
    char *url = g_strdup_printf("https://lzc-mesh.%s/api/clipboard/image%s%s",
                               app.box_domain, id ? "/" : "", id ? id : "");
    SoupMessage *message = soup_message_new(method, url);
    g_free(url);
    if (!message) return NULL;
    soup_message_add_flags(message, SOUP_MESSAGE_NO_REDIRECT);
    SoupMessageHeaders *headers = soup_message_get_request_headers(message);
    char *cookie = g_strdup_printf("Lzc-Auth-Token=%s", app.cookie);
    soup_message_headers_append(headers, "Cookie", cookie);
    soup_message_headers_append(headers, "X-Lzc-Mesh-Device-Id", app.device_id);
    soup_message_headers_append(headers, "X-Lzc-Mesh-Device-Token", app.file_token);
    g_free(cookie);
    return message;
}

static void share_choice(GObject *object, GAsyncResult *result, gpointer data) {
    Transfer *transfer = data;
    confirming = false;
    if (gtk_alert_dialog_choose_finish(GTK_ALERT_DIALOG(object), result, NULL) == 0 && valid(transfer)) {
        char *suffix = g_uuid_string_random();
        char *name = g_strdup_printf("clipboard-%s.png", suffix);
        files_upload_named(transfer->file, name);
        g_free(name);
        g_free(suffix);
    }
    transfer_free(transfer);
}

static void confirm_share(Transfer *transfer) {
    if (confirming) { transfer_free(transfer); return; }
    confirming = true;
    GtkAlertDialog *dialog = gtk_alert_dialog_new("图片超出同步限制");
    gtk_alert_dialog_set_detail(dialog, "图片剪贴板支持最高 8K、最大 32 MiB。可以将原分辨率图片上传到文件共享，由其他设备下载。");
    const char *buttons[] = {"通过文件共享发送", "取消", NULL};
    gtk_alert_dialog_set_buttons(dialog, buttons);
    gtk_alert_dialog_set_cancel_button(dialog, 1);
    gtk_alert_dialog_set_default_button(dialog, 1);
    GtkApplication *application = GTK_APPLICATION(g_application_get_default());
    GtkWindow *window = application ? gtk_application_get_active_window(application) : NULL;
    gtk_alert_dialog_choose(dialog, window && gtk_widget_get_visible(GTK_WIDGET(window)) ? window : NULL,
                            transfer->cancel, share_choice, transfer);
    g_object_unref(dialog);
}

static void uploaded(GObject *object, GAsyncResult *result, gpointer data) {
    Transfer *transfer = data;
    SoupSession *http = SOUP_SESSION(object);
    GBytes *payload = soup_session_send_and_read_finish(http, result, NULL);
    SoupMessage *message = soup_session_get_async_result_message(http, result);
    const unsigned status = message ? soup_message_get_status(message) : 0;
    if (valid(transfer)) {
        if (status == 413 || status == 422) {
            g_clear_pointer(&payload, g_bytes_unref);
            confirm_share(transfer);
            return;
        }
        if (status == 201 && payload && g_bytes_get_size(payload) == 43) {
            char *id = g_strndup(g_bytes_get_data(payload, NULL), 43);
            if (mesh_clipboard_image_id(id)) net_send_clipboard_image(id);
            g_free(id);
        } else app_log("图片剪贴板上传失败（HTTP %u）", status);
    }
    g_clear_pointer(&payload, g_bytes_unref);
    transfer_free(transfer);
}

static void capture_image(GTask *task, gpointer object, gpointer data, GCancellable *cancel) {
    Transfer *transfer = data;
    GError *error = NULL;
    GFileIOStream *temp = NULL;
    transfer->file = g_file_new_tmp("lzc-mesh-clipboard-XXXXXX", &temp, &error);
    if (!transfer->file) { g_task_return_error(task, error); return; }
    GOutputStream *out = g_io_stream_get_output_stream(G_IO_STREAM(temp));
    uint8_t buffer[65536];
    gsize prefix;
    if (g_input_stream_read_all(transfer->in, buffer, 24, &prefix, cancel, &error) && prefix == 24) {
        transfer->dimensions = mesh_clipboard_png_size(buffer, prefix);
        if (transfer->dimensions && g_output_stream_write_all(out, buffer, prefix, NULL, cancel, &error)) {
            transfer->size = prefix;
            gssize count;
            while ((count = g_input_stream_read(transfer->in, buffer, sizeof buffer, cancel, &error)) > 0) {
                if (transfer->size + count > G_GINT64_CONSTANT(4) * 1024 * 1024 * 1024) {
                    g_set_error_literal(&error, G_IO_ERROR, G_IO_ERROR_NO_SPACE, "图片超过文件共享的 4 GiB 上限");
                    break;
                }
                if (!g_output_stream_write_all(out, buffer, count, NULL, cancel, &error)) break;
                transfer->size += count;
            }
        }
    }
    g_input_stream_close(transfer->in, NULL, NULL);
    if (!error) g_io_stream_close(G_IO_STREAM(temp), cancel, &error);
    g_object_unref(temp);
    if (error) g_task_return_error(task, error);
    else g_task_return_boolean(task, transfer->dimensions && transfer->size >= 45);
}

static void captured(GObject *object, GAsyncResult *result, gpointer data) {
    Transfer *transfer = data;
    GError *error = NULL;
    bool ok = g_task_propagate_boolean(G_TASK(result), &error);
    if (valid(transfer) && ok) {
        if (transfer->size > MESH_CLIPBOARD_IMAGE_LIMIT || transfer->dimensions < 0) {
            confirm_share(transfer);
            return;
        }
        GFileInputStream *stream = g_file_read(transfer->file, transfer->cancel, NULL);
        SoupMessage *message = stream ? request("PUT", NULL) : NULL;
        if (message) {
            soup_message_set_request_body(message, "image/png", G_INPUT_STREAM(stream), transfer->size);
            soup_session_send_and_read_async(session, message, G_PRIORITY_DEFAULT, transfer->cancel, uploaded, transfer);
            g_object_unref(message);
            g_object_unref(stream);
            return;
        }
        g_clear_object(&stream);
        app_log("无法读取图片剪贴板");
    } else if (valid(transfer) && error) {
        app_log("无法读取图片剪贴板：%s", error->message);
    }
    g_clear_error(&error);
    transfer_free(transfer);
}

void clipboard_image_copy(int fd) {
    if (!ready()) { close(fd); return; }
    Transfer *transfer = start();
    transfer->in = g_unix_input_stream_new(fd, TRUE);
    GTask *task = g_task_new(NULL, transfer->cancel, captured, transfer);
    g_task_set_task_data(task, transfer, NULL);
    g_task_run_in_thread(task, capture_image);
    g_object_unref(task);
}

static void validate_image(GTask *task, gpointer object, gpointer data, GCancellable *cancel) {
    Transfer *transfer = data;
    GInputStream *memory = g_memory_input_stream_new_from_data(transfer->png, transfer->size, NULL);
    GdkPixbuf *pixbuf = gdk_pixbuf_new_from_stream(memory, cancel, NULL);
    g_object_unref(memory);
    g_task_return_boolean(task, pixbuf != NULL);
    g_clear_object(&pixbuf);
}

static void downloaded(GObject *object, GAsyncResult *result, gpointer data) {
    Transfer *transfer = data;
    bool decoded = g_task_propagate_boolean(G_TASK(result), NULL);
    if (valid(transfer)) {
        if (decoded) {
            GBytes *png = g_bytes_new_take(g_steal_pointer(&transfer->png), transfer->size);
            clipboard_set_png(png, transfer->from);
            g_bytes_unref(png);
        } else app_log("图片剪贴板下载失败");
    }
    transfer_free(transfer);
}

static void read_download(GObject *object, GAsyncResult *result, gpointer data) {
    Transfer *transfer = data;
    gsize size = 0;
    if (g_input_stream_read_all_finish(G_INPUT_STREAM(object), result, &size, NULL)
        && valid(transfer) && size == (gsize)transfer->size && mesh_clipboard_png_size(transfer->png, size) == 1) {
        GTask *task = g_task_new(NULL, transfer->cancel, downloaded, transfer);
        g_task_set_task_data(task, transfer, NULL);
        g_task_run_in_thread(task, validate_image);
        g_object_unref(task);
        return;
    }
    if (valid(transfer)) app_log("图片剪贴板下载失败");
    transfer_free(transfer);
}

static void download_response(GObject *object, GAsyncResult *result, gpointer data) {
    Transfer *transfer = data;
    SoupSession *http = SOUP_SESSION(object);
    transfer->in = soup_session_send_finish(http, result, NULL);
    SoupMessage *message = soup_session_get_async_result_message(http, result);
    if (valid(transfer) && transfer->in && message && soup_message_get_status(message) == 200) {
        transfer->png = g_malloc(transfer->size + 1);
        g_input_stream_read_all_async(transfer->in, transfer->png, transfer->size + 1, G_PRIORITY_DEFAULT,
                                      transfer->cancel, read_download, transfer);
        return;
    }
    if (valid(transfer)) app_log("图片剪贴板下载失败");
    transfer_free(transfer);
}

void clipboard_image_download(const char *id, gint64 size, const char *from) {
    if (!ready() || size < 45 || size > MESH_CLIPBOARD_IMAGE_LIMIT || !mesh_clipboard_image_id(id)) return;
    SoupMessage *message = request("GET", id);
    if (!message) return;
    Transfer *transfer = start();
    transfer->size = size;
    transfer->from = g_strdup(from);
    soup_session_send_async(session, message, G_PRIORITY_DEFAULT, transfer->cancel, download_response, transfer);
    g_object_unref(message);
}
