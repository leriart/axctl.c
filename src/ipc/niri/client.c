/*
 * axctl - Niri compositor backend
 *
 * Communicates with Niri via JSON-RPC over Unix socket (NIRI_SOCKET).
 * Response format: { "Reply": { "Ok": ..., "Err": ... } }
 */
#include "ipc/niri/niri.h"
#include "ipc/errors.h"
#include "utils/log.h"
#include "utils/strutil.h"
#include "utils/json_helpers.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <pthread.h>
#include <time.h>

typedef struct {
    char *socket_path;
    pthread_mutex_t mu;
    axctl_event_callback_t event_cb;
    void *event_userdata;
    pthread_t event_thread;
    bool event_running;
} niri_data_t;

/* Send a JSON-RPC request to Niri and parse the Reply */
static int niri_request(niri_data_t *d, struct json_object *req,
                         struct json_object **resp_out) {
    pthread_mutex_lock(&d->mu);

    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) { pthread_mutex_unlock(&d->mu); return AXCTL_ERR_CONNECT; }

    struct sockaddr_un addr = {0};
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, d->socket_path, sizeof(addr.sun_path) - 1);

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(fd); pthread_mutex_unlock(&d->mu); return AXCTL_ERR_CONNECT;
    }

    /* Encode and send request */
    const char *req_str = json_object_to_json_string(req);
    size_t req_len = strlen(req_str);
    if (write(fd, req_str, req_len) < 0 || write(fd, "\n", 1) < 0) {
        close(fd); pthread_mutex_unlock(&d->mu); return AXCTL_ERR_IO;
    }

    /* Read response */
    char *resp_buf = NULL;
    size_t resp_len = 0;
    char buf[4096];
    ssize_t n;
    while ((n = read(fd, buf, sizeof(buf))) > 0) {
        char *new_buf = realloc(resp_buf, resp_len + (size_t)n + 1);
        if (!new_buf) { free(resp_buf); close(fd); pthread_mutex_unlock(&d->mu); return AXCTL_ERR_OOM; }
        resp_buf = new_buf;
        memcpy(resp_buf + resp_len, buf, (size_t)n);
        resp_len += (size_t)n;
    }
    close(fd);
    pthread_mutex_unlock(&d->mu);

    if (!resp_buf) return AXCTL_ERR_IO;
    resp_buf[resp_len] = '\0';

    /* Parse reply */
    struct json_object *root = json_tokener_parse(resp_buf);
    free(resp_buf);
    if (!root) return AXCTL_ERR_PARSE;

    /* Extract Reply.Ok / Reply.Err */
    struct json_object *reply = json_get_object(root, "Reply");
    if (!reply) {
        /* Some responses are direct */
        if (resp_out) *resp_out = root;
        else json_object_put(root);
        return AXCTL_OK;
    }

    struct json_object *err_obj = NULL;
    if (json_object_object_get_ex(reply, "Err", &err_obj) && err_obj &&
        json_object_get_type(err_obj) != json_type_null) {
        axctl_set_error("niri error: %s", json_object_get_string(err_obj));
        json_object_put(root);
        return AXCTL_ERR_OPERATION_FAILED;
    }

    struct json_object *ok_obj = NULL;
    if (json_object_object_get_ex(reply, "Ok", &ok_obj) && ok_obj) {
        if (resp_out) *resp_out = json_object_get(ok_obj);
        json_object_put(root);
        return AXCTL_OK;
    }

    json_object_put(root);
    if (resp_out) *resp_out = NULL;
    return AXCTL_OK;
}

/* Convenience: send a string command */
static int niri_request_str(niri_data_t *d, const char *cmd, struct json_object **out) {
    struct json_object *req = json_object_new_string(cmd);
    int rc = niri_request(d, req, out);
    json_object_put(req);
    return rc;
}

/* Convenience: send an Action command */
static int niri_action(niri_data_t *d, const char *action_name, struct json_object *params) {
    struct json_object *req = json_object_new_object();
    struct json_object *action = json_object_new_object();
    if (params) {
        json_object_object_add(action, action_name, params);
    } else {
        json_object_object_add(action, action_name, json_object_new_object());
    }
    json_object_object_add(req, "Action", action);
    int rc = niri_request(d, req, NULL);
    json_object_put(req);
    return rc;
}

/* ── Event thread ───────────────────────────────────────────────────── */
static void *niri_event_thread(void *arg) {
    niri_data_t *d = (niri_data_t *)arg;

    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return NULL;

    struct sockaddr_un addr = {0};
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, d->socket_path, sizeof(addr.sun_path) - 1);

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(fd); return NULL;
    }

    /* Send EventStream request */
    const char *req = "\"EventStream\"\n";
    write(fd, req, strlen(req));

    FILE *fp = fdopen(fd, "r");
    if (!fp) { close(fd); return NULL; }

    char line[8192];
    while (d->event_running && fgets(line, sizeof(line), fp)) {
        struct json_object *obj = json_tokener_parse(line);
        if (!obj) continue;

        axctl_event_t evt = {0};
        evt.timestamp = (int64_t)time(NULL);
        evt.payload = json_object_new_object();

        /* Niri events are keyed by type */
        json_object_object_foreach(obj, key, val) {
            if (strcmp(key, "WorkspacesChanged") == 0) {
                evt.type = EVENT_WORKSPACE_CHANGED;
            } else if (strcmp(key, "WindowOpened") == 0 || strcmp(key, "WindowOpenedOrChanged") == 0) {
                evt.type = EVENT_WINDOW_CREATED;
                struct json_object *w = json_get_object(val, "window");
                if (w) {
                    evt.window = calloc(1, sizeof(axctl_window_t));
                    evt.window->id = axctl_sprintf("%d", json_get_int(w, "id", 0));
                    evt.window->title = axctl_strdup(json_get_string(w, "title"));
                    evt.window->app_id = axctl_strdup(json_get_string(w, "app_id"));
                    struct json_object *wid;
                    if (json_object_object_get_ex(w, "workspace_id", &wid))
                        evt.window->workspace_id = axctl_sprintf("%d", json_object_get_int(wid));
                    else
                        evt.window->workspace_id = axctl_strdup("");
                    evt.window->is_focused = json_get_bool(w, "is_focused", false);
                }
            } else if (strcmp(key, "WindowClosed") == 0) {
                evt.type = EVENT_WINDOW_CLOSED;
                struct json_object *id_obj;
                if (json_object_object_get_ex(val, "id", &id_obj)) {
                    char *id_str = axctl_sprintf("%d", json_object_get_int(id_obj));
                    json_object_object_add(evt.payload, "id", json_object_new_string(id_str));
                    free(id_str);
                }
            } else if (strcmp(key, "WindowFocusChanged") == 0) {
                evt.type = EVENT_WINDOW_FOCUSED;
                struct json_object *id_obj;
                if (json_object_object_get_ex(val, "id", &id_obj)) {
                    char *id_str = axctl_sprintf("%d", json_object_get_int(id_obj));
                    json_object_object_add(evt.payload, "address", json_object_new_string(id_str));
                    free(id_str);
                }
            } else {
                evt.type = EVENT_UNKNOWN;
            }
            break; /* Only process first key */
        }

        if (d->event_cb) d->event_cb(&evt, d->event_userdata);
        axctl_event_free(&evt);
        json_object_put(obj);
    }

    fclose(fp);
    return NULL;
}

/* ── Vtable implementations ─────────────────────────────────────────── */
static void niri_destroy(void *priv) {
    niri_data_t *d = (niri_data_t *)priv;
    if (d->event_running) {
        d->event_running = false;
        pthread_join(d->event_thread, NULL);
    }
    pthread_mutex_destroy(&d->mu);
    free(d->socket_path);
    free(d);
}

static int niri_list_windows(void *priv, axctl_window_array_t *out) {
    niri_data_t *d = (niri_data_t *)priv;
    axctl_window_array_init(out);

    /* First get workspace->output mapping */
    struct json_object *ws_resp = NULL;
    niri_request_str(d, "Workspaces", &ws_resp);

    /* Build workspace->monitor map */
    struct { int ws_id; char *output; } ws_map[256];
    int ws_map_count = 0;
    if (ws_resp && json_object_get_type(ws_resp) == json_type_array) {
        int len = json_object_array_length(ws_resp);
        for (int i = 0; i < len && ws_map_count < 256; i++) {
            struct json_object *w = json_object_array_get_idx(ws_resp, i);
            ws_map[ws_map_count].ws_id = json_get_int(w, "id", 0);
            ws_map[ws_map_count].output = axctl_strdup(json_get_string(w, "output"));
            ws_map_count++;
        }
    }
    if (ws_resp) json_object_put(ws_resp);

    /* Build output name->index map (for numerical monitor_id) */
    struct json_object *out_resp = NULL;
    niri_request_str(d, "Outputs", &out_resp);
    struct { char *name; int idx; } out_map[16];
    int out_count = 0;
    if (out_resp && json_object_get_type(out_resp) == json_type_array) {
        int len = json_object_array_length(out_resp);
        for (int i = 0; i < len && out_count < 16; i++) {
            struct json_object *o = json_object_array_get_idx(out_resp, i);
            out_map[out_count].name = axctl_strdup(json_get_string(o, "name"));
            out_map[out_count].idx = i;
            out_count++;
        }
    }
    if (out_resp) json_object_put(out_resp);

    struct json_object *resp = NULL;
    int rc = niri_request_str(d, "Windows", &resp);
    if (rc != AXCTL_OK) return rc;

    if (resp && json_object_get_type(resp) == json_type_array) {
        int len = json_object_array_length(resp);
        for (int i = 0; i < len; i++) {
            struct json_object *w = json_object_array_get_idx(resp, i);
            axctl_window_t win = {0};
            win.id = axctl_sprintf("%d", json_get_int(w, "id", 0));
            win.title = axctl_strdup(json_get_string(w, "title"));
            win.app_id = axctl_strdup(json_get_string(w, "app_id"));

            struct json_object *wid_obj;
            int wid = 0;
            if (json_object_object_get_ex(w, "workspace_id", &wid_obj) && wid_obj) {
                wid = json_object_get_int(wid_obj);
                win.workspace_id = axctl_sprintf("%d", wid);
            } else {
                win.workspace_id = axctl_strdup("");
            }

            win.is_focused = json_get_bool(w, "is_focused", false);
            win.is_floating = json_get_bool(w, "is_floating", false);
            win.is_fullscreen = json_get_bool(w, "is_fullscreen", false);

            /* Find monitor name from workspace map, then convert to index */
            const char *output_name = "";
            for (int j = 0; j < ws_map_count; j++) {
                if (ws_map[j].ws_id == wid) {
                    output_name = ws_map[j].output;
                    break;
                }
            }
            int mon_idx = 0;
            for (int j = 0; j < out_count; j++) {
                if (out_map[j].name && strcmp(out_map[j].name, output_name) == 0) {
                    mon_idx = out_map[j].idx;
                    break;
                }
            }

            win.metadata = json_object_new_object();
            json_object_object_add(win.metadata, "monitor_id",
                json_object_new_string(axctl_sprintf("%d", mon_idx)));
            /* Niri doesn't expose window geometry via IPC, use defaults */
            json_object_object_add(win.metadata, "x",
                json_object_new_int(0));
            json_object_object_add(win.metadata, "y",
                json_object_new_int(0));
            json_object_object_add(win.metadata, "width",
                json_object_new_int(100));
            json_object_object_add(win.metadata, "height",
                json_object_new_int(100));

            axctl_window_array_push(out, win);
        }
    }
    if (resp) json_object_put(resp);
    for (int i = 0; i < ws_map_count; i++) free(ws_map[i].output);
    for (int i = 0; i < out_count; i++) free(out_map[i].name);
    return AXCTL_OK;
}

static int niri_active_window(void *priv, char **out_id) {
    niri_data_t *d = (niri_data_t *)priv;
    struct json_object *resp = NULL;
    int rc = niri_request_str(d, "FocusedWindow", &resp);
    if (rc != AXCTL_OK) return rc;
    if (!resp || json_object_get_type(resp) == json_type_null) {
        *out_id = axctl_strdup("");
        if (resp) json_object_put(resp);
        return AXCTL_OK;
    }
    *out_id = axctl_sprintf("%d", json_get_int(resp, "id", 0));
    json_object_put(resp);
    return AXCTL_OK;
}

static int niri_focus_window(void *priv, const char *id) {
    niri_data_t *d = (niri_data_t *)priv;
    int id_int = atoi(id);
    struct json_object *params = json_object_new_object();
    json_object_object_add(params, "id", json_object_new_int(id_int));
    return niri_action(d, "FocusWindow", params);
}

static int niri_focus_dir(void *priv, const char *direction) {
    niri_data_t *d = (niri_data_t *)priv;
    /* Niri direction mapping */
    const char *niri_dir = direction;
    if (strcmp(direction, "l") == 0) niri_dir = "Left";
    else if (strcmp(direction, "r") == 0) niri_dir = "Right";
    else if (strcmp(direction, "u") == 0) niri_dir = "Up";
    else if (strcmp(direction, "d") == 0) niri_dir = "Down";

    struct json_object *params = json_object_new_string(niri_dir);
    struct json_object *req = json_object_new_object();
    struct json_object *action = json_object_new_object();
    json_object_object_add(action, "FocusColumnDirection", params);
    json_object_object_add(req, "Action", action);
    int rc = niri_request(d, req, NULL);
    json_object_put(req);
    return rc;
}

static int niri_close_window(void *priv, const char *id) {
    niri_data_t *d = (niri_data_t *)priv;
    int id_int = atoi(id);
    struct json_object *params = json_object_new_object();
    json_object_object_add(params, "id", json_object_new_int(id_int));
    return niri_action(d, "CloseWindow", params);
}

static int niri_move_window(void *priv, const char *id, const char *direction) {
    (void)id;
    niri_data_t *d = (niri_data_t *)priv;
    const char *niri_dir = direction;
    if (strcmp(direction, "l") == 0) niri_dir = "Left";
    else if (strcmp(direction, "r") == 0) niri_dir = "Right";
    else if (strcmp(direction, "u") == 0) niri_dir = "Up";
    else if (strcmp(direction, "d") == 0) niri_dir = "Down";

    struct json_object *params = json_object_new_string(niri_dir);
    struct json_object *req = json_object_new_object();
    struct json_object *action = json_object_new_object();
    json_object_object_add(action, "MoveColumnDirection", params);
    json_object_object_add(req, "Action", action);
    int rc = niri_request(d, req, NULL);
    json_object_put(req);
    return rc;
}

static int niri_resize_window(void *priv, const char *id, int w, int h) {
    (void)priv; (void)id; (void)w; (void)h;
    return AXCTL_ERR_NOT_SUPPORTED;
}

static int niri_toggle_floating(void *priv, const char *id) {
    (void)id;
    niri_data_t *d = (niri_data_t *)priv;
    return niri_action(d, "ToggleWindowFloating", NULL);
}

static int niri_set_fullscreen(void *priv, const char *id, int state) {
    (void)id;
    niri_data_t *d = (niri_data_t *)priv;
    return niri_action(d, state ? "FullscreenWindow" : "UnfullscreenWindow", NULL);
}

static int niri_set_maximized(void *priv, const char *id, int state) {
    (void)id;
    niri_data_t *d = (niri_data_t *)priv;
    return niri_action(d, state ? "MaximizeColumn" : "UnmaximizeColumn", NULL);
}

static int niri_pin_window(void *priv, const char *id, int state) {
    (void)priv; (void)id; (void)state;
    return AXCTL_ERR_NOT_SUPPORTED;
}

static int niri_toggle_group(void *priv, const char *id) {
    (void)priv; (void)id;
    return AXCTL_ERR_NOT_SUPPORTED;
}

static int niri_group_nav(void *priv, const char *direction) {
    (void)priv; (void)direction;
    return AXCTL_ERR_NOT_SUPPORTED;
}

static int niri_set_layout_property(void *priv, const char *id,
                                      const char *key, const char *value) {
    (void)id;
    niri_data_t *d = (niri_data_t *)priv;
    struct json_object *params = json_object_new_object();
    json_object_object_add(params, key, json_object_new_string(value));
    return niri_action(d, "SetWindowProperty", params);
}

static int niri_move_window_pixel(void *priv, const char *id, int x, int y) {
    (void)priv; (void)id; (void)x; (void)y;
    return AXCTL_ERR_NOT_SUPPORTED;
}

static int niri_list_workspaces(void *priv, axctl_workspace_array_t *out) {
    niri_data_t *d = (niri_data_t *)priv;
    axctl_workspace_array_init(out);

    struct json_object *resp = NULL;
    int rc = niri_request_str(d, "Workspaces", &resp);
    if (rc != AXCTL_OK) return rc;

    if (resp && json_object_get_type(resp) == json_type_array) {
        int len = json_object_array_length(resp);
        for (int i = 0; i < len; i++) {
            struct json_object *w = json_object_array_get_idx(resp, i);
            axctl_workspace_t ws = {0};
            ws.id = axctl_sprintf("%d", json_get_int(w, "id", 0));
            ws.name = axctl_strdup(json_get_string(w, "name"));
            ws.monitor_id = axctl_strdup(json_get_string(w, "output"));
            ws.is_active = json_get_bool(w, "is_active", false);
            ws.is_empty = false;
            /* Add metadata with focused field (matches Go original) */
            ws.metadata = json_object_new_object();
            json_object_object_add(ws.metadata, "focused",
                json_object_new_boolean(ws.is_active));
            axctl_workspace_array_push(out, ws);
        }
    }
    if (resp) json_object_put(resp);
    return AXCTL_OK;
}

static int niri_active_workspace(void *priv, axctl_workspace_t *out) {
    niri_data_t *d = (niri_data_t *)priv;
    struct json_object *resp = NULL;
    int rc = niri_request_str(d, "FocusedOutput", &resp);
    if (rc != AXCTL_OK) return rc;

    memset(out, 0, sizeof(*out));
    if (resp) {
        /* Get active workspace from workspaces list */
        struct json_object *ws_resp = NULL;
        niri_request_str(d, "Workspaces", &ws_resp);
        if (ws_resp && json_object_get_type(ws_resp) == json_type_array) {
            int len = json_object_array_length(ws_resp);
            for (int i = 0; i < len; i++) {
                struct json_object *w = json_object_array_get_idx(ws_resp, i);
                if (json_get_bool(w, "is_active", false)) {
                    out->id = axctl_sprintf("%d", json_get_int(w, "id", 0));
                    out->name = axctl_strdup(json_get_string(w, "name"));
                    out->monitor_id = axctl_strdup(json_get_string(w, "output"));
                    out->is_active = true;
                    /* Add metadata with focused field (matches Go original) */
                    out->metadata = json_object_new_object();
                    json_object_object_add(out->metadata, "focused",
                        json_object_new_boolean(1));
                    break;
                }
            }
        }
        if (ws_resp) json_object_put(ws_resp);
        json_object_put(resp);
    }
    if (!out->id) out->id = axctl_strdup("");
    if (!out->name) out->name = axctl_strdup("");
    if (!out->monitor_id) out->monitor_id = axctl_strdup("");
    return AXCTL_OK;
}

static int niri_switch_workspace(void *priv, const char *id) {
    niri_data_t *d = (niri_data_t *)priv;
    struct json_object *params = json_object_new_object();
    struct json_object *ref = json_object_new_object();
    json_object_object_add(ref, "Id", json_object_new_int(atoi(id)));
    json_object_object_add(params, "reference", ref);
    return niri_action(d, "FocusWorkspace", params);
}

static int niri_move_to_workspace(void *priv, const char *win_id, const char *ws_id) {
    (void)win_id;
    niri_data_t *d = (niri_data_t *)priv;
    struct json_object *params = json_object_new_object();
    struct json_object *ref = json_object_new_object();
    json_object_object_add(ref, "Id", json_object_new_int(atoi(ws_id)));
    json_object_object_add(params, "reference", ref);
    return niri_action(d, "MoveColumnToWorkspace", params);
}

static int niri_move_to_workspace_silent(void *priv, const char *win_id, const char *ws_id) {
    return niri_move_to_workspace(priv, win_id, ws_id);
}

static int niri_toggle_special(void *priv, const char *name) {
    (void)priv; (void)name;
    return AXCTL_ERR_NOT_SUPPORTED;
}

static int niri_list_monitors(void *priv, axctl_monitor_array_t *out) {
    niri_data_t *d = (niri_data_t *)priv;
    axctl_monitor_array_init(out);

    /* Fetch workspaces to map active workspace IDs to outputs */
    struct json_object *ws_resp = NULL;
    niri_request_str(d, "Workspaces", &ws_resp);
    /* Build output->active_workspace_name map */
    char out_ws_name[256][64];
    int out_ws_count = 0;
    if (ws_resp && json_object_get_type(ws_resp) == json_type_array) {
        int len = json_object_array_length(ws_resp);
        for (int i = 0; i < len && out_ws_count < 256; i++) {
            struct json_object *w = json_object_array_get_idx(ws_resp, i);
            const char *output = json_get_string(w, "output");
            const char *name = json_get_string(w, "name");
            if (output && name && json_get_bool(w, "is_active", false)) {
                snprintf(out_ws_name[out_ws_count], sizeof(out_ws_name[0]),
                         "%s=%s", output, name);
                out_ws_count++;
            }
        }
    }
    if (ws_resp) json_object_put(ws_resp);

    struct json_object *resp = NULL;
    int rc = niri_request_str(d, "Outputs", &resp);
    if (rc != AXCTL_OK) return rc;

    if (resp && json_object_get_type(resp) == json_type_array) {
        int len = json_object_array_length(resp);
        for (int i = 0; i < len; i++) {
            struct json_object *m = json_object_array_get_idx(resp, i);
            axctl_monitor_t mon = {0};
            const char *mon_name = json_get_string(m, "name");
            mon.name = axctl_strdup(mon_name);
            mon.id = axctl_strdup(mon_name);
            mon.description = axctl_strdup(json_get_string(m, "make"));

            struct json_object *mode = json_get_object(m, "current_mode");
            if (mode) {
                mon.width = json_get_int(mode, "width", 0);
                mon.height = json_get_int(mode, "height", 0);
                mon.refresh_rate = json_get_int(mode, "refresh", 0) / 1000.0;
            }
            mon.scale = json_get_double(m, "scale", 1.0);
            mon.is_focused = json_get_bool(m, "is_focused", false);

            /* Metadata with active_workspace, x, y, transform */
            mon.metadata = json_object_new_object();
            json_object_object_add(mon.metadata, "x",
                json_object_new_int(json_get_int(m, "x", 0)));
            json_object_object_add(mon.metadata, "y",
                json_object_new_int(json_get_int(m, "y", 0)));
            json_object_object_add(mon.metadata, "transform",
                json_object_new_int(json_get_int(m, "transform", 0)));
            /* Find active workspace for this output */
            const char *active_ws = "";
            if (mon_name) {
                for (int j = 0; j < out_ws_count; j++) {
                    char expected[64];
                    snprintf(expected, sizeof(expected), "%s=", mon_name);
                    if (strncmp(out_ws_name[j], expected, strlen(expected)) == 0) {
                        active_ws = out_ws_name[j] + strlen(expected);
                        break;
                    }
                }
            }
            json_object_object_add(mon.metadata, "active_workspace",
                json_object_new_string(active_ws));

            axctl_monitor_array_push(out, mon);
        }
    }
    if (resp) json_object_put(resp);
    return AXCTL_OK;
}

static int niri_focus_monitor(void *priv, const char *id) {
    niri_data_t *d = (niri_data_t *)priv;
    struct json_object *params = json_object_new_string(id);
    struct json_object *req = json_object_new_object();
    struct json_object *action = json_object_new_object();
    json_object_object_add(action, "FocusOutput", params);
    json_object_object_add(req, "Action", action);
    int rc = niri_request(d, req, NULL);
    json_object_put(req);
    return rc;
}

static int niri_move_to_monitor(void *priv, const char *win_id, const char *mon_id) {
    (void)win_id;
    niri_data_t *d = (niri_data_t *)priv;
    struct json_object *params = json_object_new_string(mon_id);
    struct json_object *req = json_object_new_object();
    struct json_object *action = json_object_new_object();
    json_object_object_add(action, "MoveColumnToOutput", params);
    json_object_object_add(req, "Action", action);
    int rc = niri_request(d, req, NULL);
    json_object_put(req);
    return rc;
}

static int niri_set_dpms(void *priv, const char *mon_id, int on) {
    (void)priv; (void)mon_id; (void)on;
    return AXCTL_ERR_NOT_SUPPORTED;
}

static int niri_set_layout(void *priv, const char *name) {
    (void)priv; (void)name;
    return AXCTL_ERR_NOT_SUPPORTED;
}

static int niri_get_config(void *priv, const char *key, struct json_object **out) {
    (void)priv; (void)key;
    *out = json_object_new_object();
    return AXCTL_ERR_NOT_SUPPORTED;
}

static int niri_set_config(void *priv, const char *key, const char *value) {
    niri_data_t *d = (niri_data_t *)priv;
    /* Niri supports limited config via IPC */
    if (strstr(key, "border") || strstr(key, "color")) {
        struct json_object *params = json_object_new_object();
        json_object_object_add(params, key, json_object_new_string(value ? value : ""));
        return niri_action(d, "SetConfigValue", params);
    }
    return AXCTL_ERR_NOT_SUPPORTED;
}

static int niri_batch_config(void *priv, struct json_object *configs) {
    (void)priv; (void)configs;
    return AXCTL_ERR_NOT_SUPPORTED;
}

static int niri_batch_keybinds(void *priv, const char *json_payload) {
    (void)priv; (void)json_payload;
    return AXCTL_ERR_NOT_SUPPORTED;
}

static int niri_raw_batch(void *priv, const char *command) {
    (void)priv; (void)command;
    return AXCTL_ERR_NOT_SUPPORTED;
}

static int niri_reload_config(void *priv) {
    (void)priv;
    /* Niri auto-reloads when config file changes */
    return AXCTL_OK;
}

static int niri_get_animations(void *priv, struct json_object **out) {
    (void)priv;
    *out = json_object_new_object();
    return AXCTL_ERR_NOT_SUPPORTED;
}

static int niri_get_cursor_position(void *priv, int *x, int *y) {
    (void)priv; *x = 0; *y = 0;
    return AXCTL_ERR_NOT_SUPPORTED;
}

static int niri_bind_key(void *priv, const char *mods, const char *key, const char *cmd) {
    (void)priv; (void)mods; (void)key; (void)cmd;
    return AXCTL_ERR_NOT_SUPPORTED;
}

static int niri_unbind_key(void *priv, const char *mods, const char *key) {
    (void)priv; (void)mods; (void)key;
    return AXCTL_ERR_NOT_SUPPORTED;
}

static int niri_execute(void *priv, const char *command) {
    niri_data_t *d = (niri_data_t *)priv;
    struct json_object *params = json_object_new_object();
    struct json_object *cmd_arr = json_object_new_array();
    json_object_array_add(cmd_arr, json_object_new_string("sh"));
    json_object_array_add(cmd_arr, json_object_new_string("-c"));
    json_object_array_add(cmd_arr, json_object_new_string(command));
    json_object_object_add(params, "command", cmd_arr);
    return niri_action(d, "Spawn", params);
}

static int niri_exit(void *priv) {
    niri_data_t *d = (niri_data_t *)priv;
    return niri_action(d, "Quit", NULL);
}

static int niri_switch_keyboard_layout(void *priv, const char *action) {
    niri_data_t *d = (niri_data_t *)priv;
    struct json_object *params = json_object_new_string(
        (strcmp(action, "prev") == 0) ? "Prev" : "Next");
    struct json_object *req = json_object_new_object();
    struct json_object *act = json_object_new_object();
    json_object_object_add(act, "SwitchLayout", params);
    json_object_object_add(req, "Action", act);
    int rc = niri_request(d, req, NULL);
    json_object_put(req);
    return rc;
}

static int niri_set_keyboard_layouts(void *priv, const char *layouts, const char *variants) {
    (void)priv; (void)layouts; (void)variants;
    return AXCTL_ERR_NOT_SUPPORTED;
}

static int niri_subscribe(void *priv, axctl_event_callback_t cb, void *userdata) {
    niri_data_t *d = (niri_data_t *)priv;
    d->event_cb = cb;
    d->event_userdata = userdata;
    d->event_running = true;
    if (pthread_create(&d->event_thread, NULL, niri_event_thread, d) != 0) {
        return AXCTL_ERR_SUBSCRIPTION_FAILED;
    }
    return AXCTL_OK;
}

static int niri_get_capabilities(void *priv, axctl_capabilities_t *out) {
    (void)priv;
    *out = (axctl_capabilities_t){
        .blur = false, .shadows = false, .animations = true,
        .rounded_corners = true, .workspaces_supported = true,
        .windows_supported = true
    };
    return AXCTL_OK;
}



axctl_compositor_t *niri_compositor_create(void) {
    const char *path = getenv("NIRI_SOCKET");
    if (!path || !*path) return NULL;

    niri_data_t *d = calloc(1, sizeof(niri_data_t));
    if (!d) return NULL;
    d->socket_path = axctl_strdup(path);
    pthread_mutex_init(&d->mu, NULL);

    axctl_compositor_t *c = calloc(1, sizeof(axctl_compositor_t));
    if (!c) { free(d->socket_path); free(d); return NULL; }

    c->name = "Niri";
    c->priv = d;
    c->destroy = niri_destroy;
    c->list_windows = niri_list_windows;
    c->active_window = niri_active_window;
    c->focus_window = niri_focus_window;
    c->focus_dir = niri_focus_dir;
    c->close_window = niri_close_window;
    c->move_window = niri_move_window;
    c->resize_window = niri_resize_window;
    c->toggle_floating = niri_toggle_floating;
    c->set_fullscreen = niri_set_fullscreen;
    c->set_maximized = niri_set_maximized;
    c->pin_window = niri_pin_window;
    c->toggle_group = niri_toggle_group;
    c->group_nav = niri_group_nav;
    c->set_layout_property = niri_set_layout_property;
    c->move_window_pixel = niri_move_window_pixel;
    c->list_workspaces = niri_list_workspaces;
    c->active_workspace = niri_active_workspace;
    c->switch_workspace = niri_switch_workspace;
    c->move_to_workspace = niri_move_to_workspace;
    c->move_to_workspace_silent = niri_move_to_workspace_silent;
    c->toggle_special_workspace = niri_toggle_special;
    c->list_monitors = niri_list_monitors;
    c->focus_monitor = niri_focus_monitor;
    c->move_to_monitor = niri_move_to_monitor;
    c->set_dpms = niri_set_dpms;
    c->set_layout = niri_set_layout;
    c->get_config = niri_get_config;
    c->set_config = niri_set_config;
    c->batch_config = niri_batch_config;
    c->batch_keybinds = niri_batch_keybinds;
    c->raw_batch = niri_raw_batch;
    c->reload_config = niri_reload_config;
    c->get_animations = niri_get_animations;
    c->get_cursor_position = niri_get_cursor_position;
    c->bind_key = niri_bind_key;
    c->unbind_key = niri_unbind_key;
    c->execute = niri_execute;
    c->compositor_exit = niri_exit;
    c->switch_keyboard_layout = niri_switch_keyboard_layout;
    c->set_keyboard_layouts = niri_set_keyboard_layouts;
    c->subscribe = niri_subscribe;
    c->get_capabilities = niri_get_capabilities;

    return c;
}
