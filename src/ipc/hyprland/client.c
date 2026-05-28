/*
 * axctl - Hyprland compositor backend
 *
 * Communicates with Hyprland via two Unix sockets:
 *   .socket.sock  - command/query socket (request-response)
 *   .socket2.sock - event stream socket (line-based events)
 *
 * Supports both legacy and Lua dispatcher syntax (Hyprland >= 0.55).
 */
#include "ipc/hyprland/hyprland.h"
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
#include <errno.h>
#include <json-c/json.h>
#include <time.h>

/* ── Private data ───────────────────────────────────────────────────── */
typedef struct {
    char *signature;
    pthread_mutex_t mu;
    pthread_mutex_t version_mu;
    bool version_known;
    bool use_lua_dispatch;
    axctl_event_callback_t event_cb;
    void *event_userdata;
    pthread_t event_thread;
    bool event_running;
} hyprland_data_t;

/* ── Socket path construction ───────────────────────────────────────── */
static char *get_socket_path(hyprland_data_t *d, const char *socket_name) {
    const char *rt = getenv("XDG_RUNTIME_DIR");
    if (!rt) rt = "/tmp";
    return axctl_sprintf("%s/hypr/%s/%s", rt, d->signature, socket_name);
}

/* ── Low-level IPC dispatch ─────────────────────────────────────────── */
static int hypr_dispatch(hyprland_data_t *d, const char *cmd, char **out) {
    char *path = get_socket_path(d, ".socket.sock");
    if (!path) return AXCTL_ERR_OOM;

    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) { free(path); return AXCTL_ERR_CONNECT; }

    struct sockaddr_un addr = {0};
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, path, sizeof(addr.sun_path) - 1);
    free(path);

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(fd); return AXCTL_ERR_CONNECT;
    }

    if (write(fd, cmd, strlen(cmd)) < 0) {
        close(fd); return AXCTL_ERR_IO;
    }

    /* Read full response */
    char *resp = NULL;
    size_t resp_len = 0;
    char buf[4096];
    ssize_t n;
    while ((n = read(fd, buf, sizeof(buf))) > 0) {
        char *new_resp = realloc(resp, resp_len + (size_t)n + 1);
        if (!new_resp) { free(resp); close(fd); return AXCTL_ERR_OOM; }
        resp = new_resp;
        memcpy(resp + resp_len, buf, (size_t)n);
        resp_len += (size_t)n;
    }
    close(fd);

    if (!resp) resp = axctl_strdup("");
    else resp[resp_len] = '\0';

    /* Check for errors */
    char *trimmed = axctl_strtrim(resp);
    if (axctl_starts_with(trimmed, "error:") || strcmp(trimmed, "unknown request") == 0) {
        axctl_set_error("hyprland rejected request: %s", trimmed);
        if (out) *out = resp;
        else free(resp);
        return AXCTL_ERR_OPERATION_FAILED;
    }

    if (out) *out = resp;
    else free(resp);
    return AXCTL_OK;
}

static int hypr_dispatch_locked(hyprland_data_t *d, const char *cmd, char **out) {
    pthread_mutex_lock(&d->mu);
    int rc = hypr_dispatch(d, cmd, out);
    pthread_mutex_unlock(&d->mu);
    return rc;
}

/* ── Version detection ──────────────────────────────────────────────── */
static bool hypr_supports_lua(hyprland_data_t *d) {
    pthread_mutex_lock(&d->version_mu);
    if (d->version_known) {
        bool use = d->use_lua_dispatch;
        pthread_mutex_unlock(&d->version_mu);
        return use;
    }
    pthread_mutex_unlock(&d->version_mu);

    char *resp = NULL;
    int rc = hypr_dispatch_locked(d, "j/version", &resp);
    if (rc != AXCTL_OK || !resp) { free(resp); return false; }

    bool use_lua = false;
    struct json_object *root = json_tokener_parse(resp);
    if (root) {
        const char *ver = json_get_string(root, "version");
        if (!ver || !*ver) ver = json_get_string(root, "tag");
        if (ver && *ver) {
            /* Parse version: skip 'v' prefix, compare major.minor */
            const char *v = ver;
            if (*v == 'v') v++;
            int major = 0, minor = 0;
            sscanf(v, "%d.%d", &major, &minor);
            use_lua = (major > 0) || (major == 0 && minor >= 55);
        }
        json_object_put(root);
    }
    free(resp);

    pthread_mutex_lock(&d->version_mu);
    d->use_lua_dispatch = use_lua;
    d->version_known = true;
    pthread_mutex_unlock(&d->version_mu);
    return use_lua;
}

/* Dispatch using appropriate syntax based on version */
static int hypr_dispatch_versioned(hyprland_data_t *d, const char *legacy,
                                     const char *lua, char **out) {
    char *cmd;
    if (hypr_supports_lua(d))
        cmd = axctl_sprintf("dispatch %s", lua);
    else
        cmd = axctl_sprintf("dispatch %s", legacy);
    if (!cmd) return AXCTL_ERR_OOM;
    int rc = hypr_dispatch_locked(d, cmd, out);
    free(cmd);
    return rc;
}

/* ── Event subscription thread ──────────────────────────────────────── */
static axctl_event_type_t parse_hypr_event_type(const char *name) {
    if (strcmp(name, "openwindow") == 0) return EVENT_WINDOW_CREATED;
    if (strcmp(name, "closewindow") == 0) return EVENT_WINDOW_CLOSED;
    if (strcmp(name, "activewindow") == 0 || strcmp(name, "activewindowv2") == 0)
        return EVENT_WINDOW_FOCUSED;
    if (strcmp(name, "windowtitle") == 0 || strcmp(name, "windowtitlev2") == 0)
        return EVENT_WINDOW_TITLE_CHANGED;
    if (strcmp(name, "movewindow") == 0 || strcmp(name, "movewindowv2") == 0)
        return EVENT_WINDOW_MOVED;
    if (strcmp(name, "workspace") == 0 || strcmp(name, "workspacev2") == 0)
        return EVENT_WORKSPACE_CHANGED;
    if (strcmp(name, "monitoradded") == 0 || strcmp(name, "monitorremoved") == 0 ||
        strcmp(name, "monitoraddedv2") == 0)
        return EVENT_MONITOR_CHANGED;
    if (strcmp(name, "configreloaded") == 0) return EVENT_CONFIG_RELOADED;
    if (strcmp(name, "fullscreen") == 0) return EVENT_FULLSCREEN_CHANGED;
    if (strcmp(name, "focusedmon") == 0) return EVENT_FOCUSED_MONITOR_CHANGED;
    if (strcmp(name, "changefloatingmode") == 0) return EVENT_FLOATING_CHANGED;
    return EVENT_UNKNOWN;
}

static void *event_thread_func(void *arg) {
    hyprland_data_t *d = (hyprland_data_t *)arg;

    char *path = get_socket_path(d, ".socket2.sock");
    if (!path) return NULL;

    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) { free(path); return NULL; }

    struct sockaddr_un addr = {0};
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, path, sizeof(addr.sun_path) - 1);
    free(path);

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(fd); return NULL;
    }

    FILE *fp = fdopen(fd, "r");
    if (!fp) { close(fd); return NULL; }

    char line[4096];
    while (d->event_running && fgets(line, sizeof(line), fp)) {
        /* Format: "eventname>>data" */
        char *sep = strstr(line, ">>");
        if (!sep) continue;
        *sep = '\0';
        char *data = sep + 2;
        /* Remove trailing newline */
        size_t dlen = strlen(data);
        if (dlen > 0 && data[dlen-1] == '\n') data[dlen-1] = '\0';

        axctl_event_t evt = {0};
        evt.type = parse_hypr_event_type(line);
        evt.timestamp = (int64_t)time(NULL);
        evt.payload = json_object_new_object();

        /* Parse event-specific data */
        if (evt.type == EVENT_WINDOW_CREATED) {
            /* data: address,workspace,class,title */
            char **parts; int n = axctl_strsplit(data, ',', &parts);
            if (n >= 1) json_object_object_add(evt.payload, "address",
                json_object_new_string(axctl_sprintf("0x%s", parts[0])));
            if (n >= 3) json_object_object_add(evt.payload, "class", json_object_new_string(parts[2]));
            if (n >= 4) json_object_object_add(evt.payload, "title", json_object_new_string(parts[3]));
            if (n >= 2) json_object_object_add(evt.payload, "workspace", json_object_new_string(parts[1]));
            /* Create a window object */
            evt.window = calloc(1, sizeof(axctl_window_t));
            if (evt.window && n >= 1) {
                evt.window->id = axctl_sprintf("0x%s", parts[0]);
                evt.window->title = (n >= 4) ? axctl_strdup(parts[3]) : axctl_strdup("");
                evt.window->app_id = (n >= 3) ? axctl_strdup(parts[2]) : axctl_strdup("");
                evt.window->workspace_id = (n >= 2) ? axctl_strdup(parts[1]) : axctl_strdup("");
            }
            axctl_strsplit_free(parts, n);
        } else if (evt.type == EVENT_WINDOW_CLOSED) {
            char *addr = axctl_sprintf("0x%s", data);
            json_object_object_add(evt.payload, "address", json_object_new_string(addr));
            json_object_object_add(evt.payload, "id", json_object_new_string(addr));
            free(addr);
        } else if (evt.type == EVENT_WINDOW_FOCUSED) {
            /* activewindowv2: address */
            if (strcmp(line, "activewindowv2") == 0) {
                char *addr = axctl_sprintf("0x%s", data);
                json_object_object_add(evt.payload, "address", json_object_new_string(addr));
                free(addr);
            } else {
                /* activewindow: class,title */
                char **parts; int n = axctl_strsplit(data, ',', &parts);
                if (n >= 1) json_object_object_add(evt.payload, "class", json_object_new_string(parts[0]));
                if (n >= 2) json_object_object_add(evt.payload, "title", json_object_new_string(parts[1]));
                axctl_strsplit_free(parts, n);
            }
        } else if (evt.type == EVENT_WINDOW_TITLE_CHANGED) {
            json_object_object_add(evt.payload, "address", json_object_new_string(data));
        } else if (evt.type == EVENT_WORKSPACE_CHANGED) {
            json_object_object_add(evt.payload, "name", json_object_new_string(data));
        } else if (evt.type == EVENT_WINDOW_MOVED) {
            /* movewindowv2: address,workspace_id,workspace_name */
            char **parts; int n = axctl_strsplit(data, ',', &parts);
            if (n >= 1) {
                char *addr = axctl_sprintf("0x%s", parts[0]);
                json_object_object_add(evt.payload, "address", json_object_new_string(addr));
                json_object_object_add(evt.payload, "id", json_object_new_string(addr));
                free(addr);
            }
            if (n >= 3) json_object_object_add(evt.payload, "workspace", json_object_new_string(parts[2]));
            axctl_strsplit_free(parts, n);
        } else if (evt.type == EVENT_FULLSCREEN_CHANGED) {
            json_object_object_add(evt.payload, "fullscreen",
                json_object_new_boolean(data[0] == '1'));
            /* windowtitlev2 provides address,title */
        } else if (evt.type == EVENT_FLOATING_CHANGED) {
            /* changefloatingmode: address,state */
            char **parts; int n = axctl_strsplit(data, ',', &parts);
            if (n >= 1) {
                char *addr = axctl_sprintf("0x%s", parts[0]);
                json_object_object_add(evt.payload, "address", json_object_new_string(addr));
                free(addr);
            }
            if (n >= 2) json_object_object_add(evt.payload, "floating",
                json_object_new_boolean(parts[1][0] == '1'));
            axctl_strsplit_free(parts, n);
        } else {
            json_object_object_add(evt.payload, "data", json_object_new_string(data));
        }

        if (d->event_cb) d->event_cb(&evt, d->event_userdata);
        axctl_event_free(&evt);
    }

    fclose(fp);
    return NULL;
}

/* ── Compositor vtable implementations ──────────────────────────────── */
static void hypr_destroy(void *priv) {
    hyprland_data_t *d = (hyprland_data_t *)priv;
    if (d->event_running) {
        d->event_running = false;
        pthread_join(d->event_thread, NULL);
    }
    pthread_mutex_destroy(&d->mu);
    pthread_mutex_destroy(&d->version_mu);
    free(d->signature);
    free(d);
}

static int hypr_list_windows(void *priv, axctl_window_array_t *out) {
    hyprland_data_t *d = (hyprland_data_t *)priv;
    axctl_window_array_init(out);

    char *resp = NULL;
    int rc = hypr_dispatch_locked(d, "j/clients", &resp);
    if (rc != AXCTL_OK) { free(resp); return rc; }

    struct json_object *arr = json_tokener_parse(resp);
    free(resp);
    if (!arr || json_object_get_type(arr) != json_type_array) {
        if (arr) json_object_put(arr);
        return AXCTL_ERR_PARSE;
    }

    int len = json_object_array_length(arr);
    for (int i = 0; i < len; i++) {
        struct json_object *w = json_object_array_get_idx(arr, i);
        axctl_window_t win = {0};
        /* Hyprland uses hex addresses as IDs */
        win.id = axctl_sprintf("0x%llx", (long long)json_get_int(w, "address", 0));
        /* Try "address" as string first */
        const char *addr_str = json_get_string(w, "address");
        if (addr_str && *addr_str) {
            free(win.id);
            win.id = axctl_strdup(addr_str);
        }
        win.title = axctl_strdup(json_get_string(w, "title"));
        /* Hyprland uses "class" instead of "app_id" */
        const char *cls = json_get_string(w, "class");
        if (!cls || !*cls) cls = json_get_string(w, "initialClass");
        win.app_id = axctl_strdup(cls);

        /* Workspace */
        struct json_object *ws_obj = json_get_object(w, "workspace");
        if (ws_obj) {
            win.workspace_id = axctl_sprintf("%d", json_get_int(ws_obj, "id", 0));
        } else {
            win.workspace_id = axctl_strdup("");
        }

        win.is_focused = json_get_bool(w, "focusHistoryID", false) == 0;
        win.is_floating = json_get_bool(w, "floating", false);
        win.is_fullscreen = json_get_bool(w, "fullscreen", false);
        win.is_hidden = json_get_bool(w, "hidden", false);

        /* Metadata with full Hyprland-specific fields */
        win.metadata = json_object_new_object();
        json_object_object_add(win.metadata, "monitor_id",
            json_object_new_string(axctl_sprintf("%d", json_get_int(w, "monitor", -1))));
        json_object_object_add(win.metadata, "pinned",
            json_object_new_boolean(json_get_bool(w, "pinned", false)));

        struct json_object *at_arr = json_get_array(w, "at");
        if (at_arr && json_object_array_length(at_arr) >= 2) {
            json_object_object_add(win.metadata, "x",
                json_object_new_int(json_object_get_int(json_object_array_get_idx(at_arr, 0))));
            json_object_object_add(win.metadata, "y",
                json_object_new_int(json_object_get_int(json_object_array_get_idx(at_arr, 1))));
        }

        struct json_object *size_arr = json_get_array(w, "size");
        if (size_arr && json_object_array_length(size_arr) >= 2) {
            json_object_object_add(win.metadata, "width",
                json_object_new_int(json_object_get_int(json_object_array_get_idx(size_arr, 0))));
            json_object_object_add(win.metadata, "height",
                json_object_new_int(json_object_get_int(json_object_array_get_idx(size_arr, 1))));
        }

        axctl_window_array_push(out, win);
    }

    json_object_put(arr);
    return AXCTL_OK;
}

static int hypr_active_window(void *priv, char **out_id) {
    hyprland_data_t *d = (hyprland_data_t *)priv;
    char *resp = NULL;
    int rc = hypr_dispatch_locked(d, "j/activewindow", &resp);
    if (rc != AXCTL_OK) { free(resp); return rc; }

    struct json_object *obj = json_tokener_parse(resp);
    free(resp);
    if (!obj) return AXCTL_ERR_PARSE;

    const char *addr = json_get_string(obj, "address");
    *out_id = axctl_strdup(addr && *addr ? addr : "");
    json_object_put(obj);
    return AXCTL_OK;
}

static int hypr_focus_window(void *priv, const char *id) {
    hyprland_data_t *d = (hyprland_data_t *)priv;
    char *cmd = axctl_sprintf("dispatch focuswindow address:%s", id);
    int rc = hypr_dispatch_locked(d, cmd, NULL);
    free(cmd);
    return rc;
}

static int hypr_focus_dir(void *priv, const char *direction) {
    hyprland_data_t *d = (hyprland_data_t *)priv;
    return hypr_dispatch_versioned(d,
        axctl_sprintf("movefocus %s", direction),
        axctl_sprintf("moveFocus(%s)", direction), NULL);
}

static int hypr_close_window(void *priv, const char *id) {
    hyprland_data_t *d = (hyprland_data_t *)priv;
    char *cmd = axctl_sprintf("dispatch closewindow address:%s", id);
    int rc = hypr_dispatch_locked(d, cmd, NULL);
    free(cmd);
    return rc;
}

static int hypr_move_window(void *priv, const char *id, const char *direction) {
    hyprland_data_t *d = (hyprland_data_t *)priv;
    (void)id; /* Hyprland moves the active window */
    return hypr_dispatch_versioned(d,
        axctl_sprintf("movewindow %s", direction),
        axctl_sprintf("moveWindow(%s)", direction), NULL);
}

static int hypr_resize_window(void *priv, const char *id, int w, int h) {
    hyprland_data_t *d = (hyprland_data_t *)priv;
    char *cmd = axctl_sprintf("dispatch resizewindowpixel exact %d %d,address:%s", w, h, id);
    int rc = hypr_dispatch_locked(d, cmd, NULL);
    free(cmd);
    return rc;
}

static int hypr_toggle_floating(void *priv, const char *id) {
    hyprland_data_t *d = (hyprland_data_t *)priv;
    char *cmd = axctl_sprintf("dispatch togglefloating address:%s", id);
    int rc = hypr_dispatch_locked(d, cmd, NULL);
    free(cmd);
    return rc;
}

static int hypr_set_fullscreen(void *priv, const char *id, int state) {
    hyprland_data_t *d = (hyprland_data_t *)priv;
    (void)id;
    char *cmd = axctl_sprintf("dispatch fullscreen %d", state ? 1 : 0);
    int rc = hypr_dispatch_locked(d, cmd, NULL);
    free(cmd);
    return rc;
}

static int hypr_set_maximized(void *priv, const char *id, int state) {
    hyprland_data_t *d = (hyprland_data_t *)priv;
    (void)id;
    char *cmd = axctl_sprintf("dispatch fullscreen %d", state ? 1 : 0);
    int rc = hypr_dispatch_locked(d, cmd, NULL);
    free(cmd);
    return rc;
}

static int hypr_pin_window(void *priv, const char *id, int state) {
    hyprland_data_t *d = (hyprland_data_t *)priv;
    (void)state;
    char *cmd = axctl_sprintf("dispatch pin address:%s", id);
    int rc = hypr_dispatch_locked(d, cmd, NULL);
    free(cmd);
    return rc;
}

static int hypr_toggle_group(void *priv, const char *id) {
    hyprland_data_t *d = (hyprland_data_t *)priv;
    (void)id;
    return hypr_dispatch_locked(d, "dispatch togglegroup", NULL);
}

static int hypr_group_nav(void *priv, const char *direction) {
    hyprland_data_t *d = (hyprland_data_t *)priv;
    char *cmd = axctl_sprintf("dispatch changegroupactive %s", direction);
    int rc = hypr_dispatch_locked(d, cmd, NULL);
    free(cmd);
    return rc;
}

static int hypr_set_layout_property(void *priv, const char *id,
                                      const char *key, const char *value) {
    (void)priv; (void)id; (void)key; (void)value;
    return AXCTL_ERR_NOT_SUPPORTED;
}

static int hypr_move_window_pixel(void *priv, const char *id, int x, int y) {
    hyprland_data_t *d = (hyprland_data_t *)priv;
    char *cmd = axctl_sprintf("dispatch movewindowpixel %d %d,address:%s", x, y, id);
    int rc = hypr_dispatch_locked(d, cmd, NULL);
    free(cmd);
    return rc;
}

static int hypr_list_workspaces(void *priv, axctl_workspace_array_t *out) {
    hyprland_data_t *d = (hyprland_data_t *)priv;
    axctl_workspace_array_init(out);

    char *resp = NULL;
    int rc = hypr_dispatch_locked(d, "j/workspaces", &resp);
    if (rc != AXCTL_OK) { free(resp); return rc; }

    struct json_object *arr = json_tokener_parse(resp);
    free(resp);
    if (!arr) return AXCTL_ERR_PARSE;

    /* Get active workspace */
    char *active_resp = NULL;
    int active_id = -1;
    if (hypr_dispatch_locked(d, "j/activeworkspace", &active_resp) == AXCTL_OK && active_resp) {
        struct json_object *aw = json_tokener_parse(active_resp);
        if (aw) { active_id = json_get_int(aw, "id", -1); json_object_put(aw); }
        free(active_resp);
    }

    int len = json_object_array_length(arr);
    for (int i = 0; i < len; i++) {
        struct json_object *w = json_object_array_get_idx(arr, i);
        int ws_id = json_get_int(w, "id", 0);
        axctl_workspace_t ws = {0};
        ws.id = axctl_sprintf("%d", ws_id);
        ws.name = axctl_strdup(json_get_string(w, "name"));
        ws.monitor_id = axctl_sprintf("%s", json_get_string(w, "monitor"));
        ws.is_active = (ws_id == active_id);
        ws.is_empty = (json_get_int(w, "windows", 0) == 0);
        axctl_workspace_array_push(out, ws);
    }

    json_object_put(arr);
    return AXCTL_OK;
}

static int hypr_active_workspace(void *priv, axctl_workspace_t *out) {
    hyprland_data_t *d = (hyprland_data_t *)priv;
    char *resp = NULL;
    int rc = hypr_dispatch_locked(d, "j/activeworkspace", &resp);
    if (rc != AXCTL_OK) { free(resp); return rc; }

    struct json_object *obj = json_tokener_parse(resp);
    free(resp);
    if (!obj) return AXCTL_ERR_PARSE;

    memset(out, 0, sizeof(*out));
    out->id = axctl_sprintf("%d", json_get_int(obj, "id", 0));
    out->name = axctl_strdup(json_get_string(obj, "name"));
    out->monitor_id = axctl_strdup(json_get_string(obj, "monitor"));
    out->is_active = true;
    out->is_empty = (json_get_int(obj, "windows", 0) == 0);

    json_object_put(obj);
    return AXCTL_OK;
}

static int hypr_switch_workspace(void *priv, const char *id) {
    hyprland_data_t *d = (hyprland_data_t *)priv;
    char *cmd = axctl_sprintf("dispatch workspace %s", id);
    int rc = hypr_dispatch_locked(d, cmd, NULL);
    free(cmd);
    return rc;
}

static int hypr_move_to_workspace(void *priv, const char *win_id, const char *ws_id) {
    hyprland_data_t *d = (hyprland_data_t *)priv;
    char *cmd = axctl_sprintf("dispatch movetoworkspace %s,address:%s", ws_id, win_id);
    int rc = hypr_dispatch_locked(d, cmd, NULL);
    free(cmd);
    return rc;
}

static int hypr_move_to_workspace_silent(void *priv, const char *win_id, const char *ws_id) {
    hyprland_data_t *d = (hyprland_data_t *)priv;
    char *cmd = axctl_sprintf("dispatch movetoworkspacesilent %s,address:%s", ws_id, win_id);
    int rc = hypr_dispatch_locked(d, cmd, NULL);
    free(cmd);
    return rc;
}

static int hypr_toggle_special(void *priv, const char *name) {
    hyprland_data_t *d = (hyprland_data_t *)priv;
    char *cmd = (name && *name) ?
        axctl_sprintf("dispatch togglespecialworkspace %s", name) :
        axctl_strdup("dispatch togglespecialworkspace");
    int rc = hypr_dispatch_locked(d, cmd, NULL);
    free(cmd);
    return rc;
}

static int hypr_list_monitors(void *priv, axctl_monitor_array_t *out) {
    hyprland_data_t *d = (hyprland_data_t *)priv;
    axctl_monitor_array_init(out);

    char *resp = NULL;
    int rc = hypr_dispatch_locked(d, "j/monitors", &resp);
    if (rc != AXCTL_OK) { free(resp); return rc; }

    struct json_object *arr = json_tokener_parse(resp);
    free(resp);
    if (!arr) return AXCTL_ERR_PARSE;

    int len = json_object_array_length(arr);
    for (int i = 0; i < len; i++) {
        struct json_object *m = json_object_array_get_idx(arr, i);
        axctl_monitor_t mon = {0};
        mon.id = axctl_sprintf("%d", json_get_int(m, "id", 0));
        mon.name = axctl_strdup(json_get_string(m, "name"));
        mon.description = axctl_strdup(json_get_string(m, "description"));
        mon.width = json_get_int(m, "width", 0);
        mon.height = json_get_int(m, "height", 0);
        mon.refresh_rate = json_get_double(m, "refreshRate", 0.0);
        mon.scale = json_get_double(m, "scale", 1.0);
        mon.is_focused = json_get_bool(m, "focused", false);
        axctl_monitor_array_push(out, mon);
    }

    json_object_put(arr);
    return AXCTL_OK;
}

static int hypr_focus_monitor(void *priv, const char *id) {
    hyprland_data_t *d = (hyprland_data_t *)priv;
    char *cmd = axctl_sprintf("dispatch focusmonitor %s", id);
    int rc = hypr_dispatch_locked(d, cmd, NULL);
    free(cmd);
    return rc;
}

static int hypr_move_to_monitor(void *priv, const char *win_id, const char *mon_id) {
    hyprland_data_t *d = (hyprland_data_t *)priv;
    (void)win_id;
    char *cmd = axctl_sprintf("dispatch movewindow mon:%s", mon_id);
    int rc = hypr_dispatch_locked(d, cmd, NULL);
    free(cmd);
    return rc;
}

static int hypr_set_dpms(void *priv, const char *mon_id, int on) {
    hyprland_data_t *d = (hyprland_data_t *)priv;
    char *cmd = axctl_sprintf("dispatch dpms %s %s", on ? "on" : "off", mon_id);
    int rc = hypr_dispatch_locked(d, cmd, NULL);
    free(cmd);
    return rc;
}

static int hypr_set_layout(void *priv, const char *name) {
    hyprland_data_t *d = (hyprland_data_t *)priv;
    char *cmd = axctl_sprintf("keyword general:layout %s", name);
    int rc = hypr_dispatch_locked(d, cmd, NULL);
    free(cmd);
    return rc;
}

static int hypr_get_config(void *priv, const char *key, struct json_object **out) {
    hyprland_data_t *d = (hyprland_data_t *)priv;
    char *cmd = axctl_sprintf("j/getoption %s", key);
    char *resp = NULL;
    int rc = hypr_dispatch_locked(d, cmd, &resp);
    free(cmd);
    if (rc != AXCTL_OK) { free(resp); return rc; }

    *out = json_tokener_parse(resp);
    free(resp);
    return *out ? AXCTL_OK : AXCTL_ERR_PARSE;
}

static int hypr_set_config(void *priv, const char *key, const char *value) {
    hyprland_data_t *d = (hyprland_data_t *)priv;
    char *cmd = axctl_sprintf("keyword %s %s", key, value ? value : "");
    int rc = hypr_dispatch_locked(d, cmd, NULL);
    free(cmd);
    return rc;
}

static int hypr_batch_config(void *priv, struct json_object *configs) {
    hyprland_data_t *d = (hyprland_data_t *)priv;
    char *batch = axctl_strdup("[[BATCH]]");

    json_object_object_foreach(configs, key, val) {
        const char *v = json_object_get_string(val);
        char *part = axctl_sprintf(";keyword %s %s", key, v ? v : "");
        axctl_str_append(&batch, part);
        free(part);
    }

    int rc = hypr_dispatch_locked(d, batch, NULL);
    free(batch);
    return rc;
}

static int hypr_batch_keybinds(void *priv, const char *json_payload) {
    hyprland_data_t *d = (hyprland_data_t *)priv;
    struct json_object *root = json_tokener_parse(json_payload);
    if (!root) return AXCTL_ERR_PARSE;

    char *batch = axctl_strdup("[[BATCH]]");

    struct json_object *binds = json_get_array(root, "binds");
    if (binds) {
        int len = json_object_array_length(binds);
        for (int i = 0; i < len; i++) {
            struct json_object *b = json_object_array_get_idx(binds, i);
            const char *key = json_get_string(b, "key");
            const char *dispatcher = json_get_string(b, "dispatcher");
            const char *argument = json_get_string(b, "argument");
            const char *flags = json_get_string(b, "flags");
            struct json_object *mods_arr = json_get_array(b, "modifiers");

            char *mods_str = axctl_strdup("");
            if (mods_arr) {
                int mlen = json_object_array_length(mods_arr);
                for (int j = 0; j < mlen; j++) {
                    if (j > 0) axctl_str_append(&mods_str, " ");
                    axctl_str_append(&mods_str,
                        json_object_get_string(json_object_array_get_idx(mods_arr, j)));
                }
            }

            char *part;
            const char *bind_kw = "bind";
            if (flags && *flags) {
                /* e.g. "m" -> bindm, "l" -> bindl, "r" -> bindr */
                bind_kw = flags;
            }

            /* Hyprland's [[BATCH]] dispatch uses keyword command format:
             *   keyword bind MODS,KEY,DISPATCHER,ARG
             * NOT config file format (no '=' sign).
             * Go version does:  keyword bind SUPER,Q,closewindow  */
            if (argument && *argument) {
                part = axctl_sprintf(";keyword %s %s, %s, %s, %s",
                    bind_kw, mods_str, key, dispatcher, argument);
            } else {
                part = axctl_sprintf(";keyword %s %s, %s, %s",
                    bind_kw, mods_str, key, dispatcher);
            }
            axctl_str_append(&batch, part);
            free(part);
            free(mods_str);
        }
    }

    struct json_object *unbinds = json_get_array(root, "unbinds");
    if (unbinds) {
        int len = json_object_array_length(unbinds);
        for (int i = 0; i < len; i++) {
            struct json_object *u = json_object_array_get_idx(unbinds, i);
            const char *key = json_get_string(u, "key");
            struct json_object *mods_arr = json_get_array(u, "modifiers");

            char *mods_str = axctl_strdup("");
            if (mods_arr) {
                int mlen = json_object_array_length(mods_arr);
                for (int j = 0; j < mlen; j++) {
                    if (j > 0) axctl_str_append(&mods_str, " ");
                    axctl_str_append(&mods_str,
                        json_object_get_string(json_object_array_get_idx(mods_arr, j)));
                }
            }

            /* Hyprland dispatch uses keyword unbind, not 'unbind =' */
            char *part = axctl_sprintf(";keyword unbind %s, %s", mods_str, key);
            axctl_str_append(&batch, part);
            free(part); free(mods_str);
        }
    }

    json_object_put(root);
    int rc = hypr_dispatch_locked(d, batch, NULL);
    free(batch);
    return rc;
}

static int hypr_raw_batch(void *priv, const char *command) {
    hyprland_data_t *d = (hyprland_data_t *)priv;
    return hypr_dispatch_locked(d, command, NULL);
}

static int hypr_reload_config(void *priv) {
    hyprland_data_t *d = (hyprland_data_t *)priv;
    return hypr_dispatch_locked(d, "reload", NULL);
}

static int hypr_get_animations(void *priv, struct json_object **out) {
    hyprland_data_t *d = (hyprland_data_t *)priv;
    char *resp = NULL;
    int rc = hypr_dispatch_locked(d, "j/animations", &resp);
    if (rc != AXCTL_OK) { free(resp); return rc; }
    *out = json_tokener_parse(resp);
    free(resp);
    return *out ? AXCTL_OK : AXCTL_ERR_PARSE;
}

static int hypr_get_cursor_position(void *priv, int *x, int *y) {
    hyprland_data_t *d = (hyprland_data_t *)priv;
    char *resp = NULL;
    int rc = hypr_dispatch_locked(d, "j/cursorpos", &resp);
    if (rc != AXCTL_OK) { free(resp); return rc; }
    struct json_object *obj = json_tokener_parse(resp);
    free(resp);
    if (!obj) return AXCTL_ERR_PARSE;
    *x = json_get_int(obj, "x", 0);
    *y = json_get_int(obj, "y", 0);
    json_object_put(obj);
    return AXCTL_OK;
}

static int hypr_bind_key(void *priv, const char *mods, const char *key, const char *cmd) {
    hyprland_data_t *d = (hyprland_data_t *)priv;
    char *command = axctl_sprintf("keyword bind %s, %s, %s", mods, key, cmd);
    int rc = hypr_dispatch_locked(d, command, NULL);
    free(command);
    return rc;
}

static int hypr_unbind_key(void *priv, const char *mods, const char *key) {
    hyprland_data_t *d = (hyprland_data_t *)priv;
    char *cmd = axctl_sprintf("keyword unbind %s, %s", mods, key);
    int rc = hypr_dispatch_locked(d, cmd, NULL);
    free(cmd);
    return rc;
}

static int hypr_execute(void *priv, const char *command) {
    hyprland_data_t *d = (hyprland_data_t *)priv;
    char *cmd = axctl_sprintf("dispatch exec %s", command);
    int rc = hypr_dispatch_locked(d, cmd, NULL);
    free(cmd);
    return rc;
}

static int hypr_exit(void *priv) {
    hyprland_data_t *d = (hyprland_data_t *)priv;
    return hypr_dispatch_locked(d, "dispatch exit", NULL);
}

static int hypr_switch_keyboard_layout(void *priv, const char *action) {
    hyprland_data_t *d = (hyprland_data_t *)priv;
    char *cmd = axctl_sprintf("switchxkblayout all %s", action);
    int rc = hypr_dispatch_locked(d, cmd, NULL);
    free(cmd);
    return rc;
}

static int hypr_set_keyboard_layouts(void *priv, const char *layouts, const char *variants) {
    hyprland_data_t *d = (hyprland_data_t *)priv;
    char *batch = axctl_sprintf("[[BATCH]];keyword input:kb_layout %s;keyword input:kb_variant %s",
                                 layouts, variants ? variants : "");
    int rc = hypr_dispatch_locked(d, batch, NULL);
    free(batch);
    return rc;
}

static int hypr_subscribe(void *priv, axctl_event_callback_t cb, void *userdata) {
    hyprland_data_t *d = (hyprland_data_t *)priv;
    d->event_cb = cb;
    d->event_userdata = userdata;
    d->event_running = true;
    if (pthread_create(&d->event_thread, NULL, event_thread_func, d) != 0) {
        return AXCTL_ERR_SUBSCRIPTION_FAILED;
    }
    return AXCTL_OK;
}

static int hypr_get_capabilities(void *priv, axctl_capabilities_t *out) {
    (void)priv;
    *out = (axctl_capabilities_t){
        .blur = true, .shadows = true, .animations = true,
        .rounded_corners = true, .workspaces_supported = true,
        .windows_supported = true
    };
    return AXCTL_OK;
}

/* ── vtable ─────────────────────────────────────────────────────────── */


axctl_compositor_t *hyprland_compositor_create(void) {
    const char *sig = getenv("HYPRLAND_INSTANCE_SIGNATURE");
    if (!sig || !*sig) return NULL;

    hyprland_data_t *d = calloc(1, sizeof(hyprland_data_t));
    if (!d) return NULL;
    d->signature = axctl_strdup(sig);
    pthread_mutex_init(&d->mu, NULL);
    pthread_mutex_init(&d->version_mu, NULL);

    axctl_compositor_t *c = calloc(1, sizeof(axctl_compositor_t));
    if (!c) { free(d->signature); free(d); return NULL; }

    c->name = "Hyprland";
    c->priv = d;
    c->destroy = hypr_destroy;
    c->list_windows = hypr_list_windows;
    c->active_window = hypr_active_window;
    c->focus_window = hypr_focus_window;
    c->focus_dir = hypr_focus_dir;
    c->close_window = hypr_close_window;
    c->move_window = hypr_move_window;
    c->resize_window = hypr_resize_window;
    c->toggle_floating = hypr_toggle_floating;
    c->set_fullscreen = hypr_set_fullscreen;
    c->set_maximized = hypr_set_maximized;
    c->pin_window = hypr_pin_window;
    c->toggle_group = hypr_toggle_group;
    c->group_nav = hypr_group_nav;
    c->set_layout_property = hypr_set_layout_property;
    c->move_window_pixel = hypr_move_window_pixel;
    c->list_workspaces = hypr_list_workspaces;
    c->active_workspace = hypr_active_workspace;
    c->switch_workspace = hypr_switch_workspace;
    c->move_to_workspace = hypr_move_to_workspace;
    c->move_to_workspace_silent = hypr_move_to_workspace_silent;
    c->toggle_special_workspace = hypr_toggle_special;
    c->list_monitors = hypr_list_monitors;
    c->focus_monitor = hypr_focus_monitor;
    c->move_to_monitor = hypr_move_to_monitor;
    c->set_dpms = hypr_set_dpms;
    c->set_layout = hypr_set_layout;
    c->get_config = hypr_get_config;
    c->set_config = hypr_set_config;
    c->batch_config = hypr_batch_config;
    c->batch_keybinds = hypr_batch_keybinds;
    c->raw_batch = hypr_raw_batch;
    c->reload_config = hypr_reload_config;
    c->get_animations = hypr_get_animations;
    c->get_cursor_position = hypr_get_cursor_position;
    c->bind_key = hypr_bind_key;
    c->unbind_key = hypr_unbind_key;
    c->execute = hypr_execute;
    c->compositor_exit = hypr_exit;
    c->switch_keyboard_layout = hypr_switch_keyboard_layout;
    c->set_keyboard_layouts = hypr_set_keyboard_layouts;
    c->subscribe = hypr_subscribe;
    c->get_capabilities = hypr_get_capabilities;

    return c;
}
