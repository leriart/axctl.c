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

/* ── Helper functions (matching Go helpers) ─────────────────────────── */

/* hyprTarget: returns "address:<id>" or "" if id is empty/NULL */
static char *hypr_target(const char *id) {
    if (!id || !*id) return axctl_strdup("");
    return axctl_sprintf("address:%s", id);
}

/* luaTargetField: returns ', window = "address:<id>"' or "" if id is empty */
static char *lua_target_field(const char *id) {
    if (!id || !*id) return axctl_strdup("");
    return axctl_sprintf(", window = \"address:%s\"", id);
}

/* luaDirection: maps short direction codes to full names */
static const char *lua_direction(const char *dir) {
    if (!dir) return "";
    if (strcmp(dir, "l") == 0) return "left";
    if (strcmp(dir, "r") == 0) return "right";
    if (strcmp(dir, "u") == 0) return "up";
    if (strcmp(dir, "d") == 0) return "down";
    return dir;
}

/* Config key mapping (matches Go's mapping) */
static const char *hypr_map_config_key(const char *key) {
    if (!key) return key;
    if (strcmp(key, "gaps.inner") == 0) return "general:gaps_in";
    if (strcmp(key, "gaps.outer") == 0) return "general:gaps_out";
    if (strcmp(key, "border.width") == 0) return "general:border_size";
    if (strcmp(key, "border.active_color") == 0) return "general:col.active_border";
    if (strcmp(key, "border.inactive_color") == 0) return "general:col.inactive_border";
    if (strcmp(key, "opacity.active") == 0) return "decoration:active_opacity";
    if (strcmp(key, "opacity.inactive") == 0) return "decoration:inactive_opacity";
    if (strcmp(key, "blur.enabled") == 0) return "decoration:blur:enabled";
    if (strcmp(key, "blur.size") == 0) return "decoration:blur:size";
    if (strcmp(key, "blur.passes") == 0) return "decoration:blur:passes";
    return key;
}

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
            if (sscanf(v, "%d.%d", &major, &minor) >= 2) {
                use_lua = (major > 0) || (major == 0 && minor >= 55);
            }
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
            if (n >= 1) {
                char *addr_str = axctl_sprintf("0x%s", parts[0]);
                json_object_object_add(evt.payload, "address",
                    json_object_new_string(addr_str));
                free(addr_str);
            }
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
            char *addr_str = axctl_sprintf("0x%s", data);
            json_object_object_add(evt.payload, "address", json_object_new_string(addr_str));
            json_object_object_add(evt.payload, "id", json_object_new_string(addr_str));
            free(addr_str);
        } else if (evt.type == EVENT_WINDOW_FOCUSED) {
            /* activewindowv2: address */
            if (strcmp(line, "activewindowv2") == 0) {
                char *addr_str = axctl_sprintf("0x%s", data);
                json_object_object_add(evt.payload, "address", json_object_new_string(addr_str));
                free(addr_str);
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
                char *addr_str = axctl_sprintf("0x%s", parts[0]);
                json_object_object_add(evt.payload, "address", json_object_new_string(addr_str));
                json_object_object_add(evt.payload, "id", json_object_new_string(addr_str));
                free(addr_str);
            }
            if (n >= 3) json_object_object_add(evt.payload, "workspace", json_object_new_string(parts[2]));
            axctl_strsplit_free(parts, n);
        } else if (evt.type == EVENT_FULLSCREEN_CHANGED) {
            json_object_object_add(evt.payload, "fullscreen",
                json_object_new_boolean(data[0] == '1'));
        } else if (evt.type == EVENT_FLOATING_CHANGED) {
            /* changefloatingmode: address,state */
            char **parts; int n = axctl_strsplit(data, ',', &parts);
            if (n >= 1) {
                char *addr_str = axctl_sprintf("0x%s", parts[0]);
                json_object_object_add(evt.payload, "address", json_object_new_string(addr_str));
                free(addr_str);
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

        /* Go uses c.Address directly (which is the hex string from JSON) */
        const char *addr_str = json_get_string(w, "address");
        if (addr_str && *addr_str) {
            win.id = axctl_strdup(addr_str);
        } else {
            win.id = axctl_sprintf("0x%llx", (long long)json_get_int(w, "address", 0));
        }

        win.title = axctl_strdup(json_get_string(w, "title"));
        /* Hyprland uses "class" for app_id */
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

        /* Go sets IsFocused = false for all windows (not from focusHistoryID) */
        win.is_focused = false;
        win.is_floating = json_get_bool(w, "floating", false);
        /* Go: IsFullscreen = c.Fullscreen != 0  (it's an int, not bool) */
        win.is_fullscreen = (json_get_int(w, "fullscreen", 0) != 0);
        win.is_hidden = false; /* Go sets IsHidden = false always */

        /* Metadata with full Hyprland-specific fields */
        win.metadata = json_object_new_object();
        char *mon_str = axctl_sprintf("%d", json_get_int(w, "monitor", -1));
        json_object_object_add(win.metadata, "monitor_id",
            json_object_new_string(mon_str));
        free(mon_str);
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

/* FocusWindow: versioned dispatch with address target */
static int hypr_focus_window(void *priv, const char *id) {
    hyprland_data_t *d = (hyprland_data_t *)priv;
    char *target = hypr_target(id);
    char *legacy = axctl_sprintf("focuswindow %s", target);
    char *lua = axctl_sprintf("hl.dsp.focus({ window = \"%s\" })", target);
    int rc = hypr_dispatch_versioned(d, legacy, lua, NULL);
    free(target); free(legacy); free(lua);
    return rc;
}

/* FocusDir: versioned dispatch */
static int hypr_focus_dir(void *priv, const char *direction) {
    hyprland_data_t *d = (hyprland_data_t *)priv;
    char *legacy = axctl_sprintf("movefocus %s", direction);
    char *lua = axctl_sprintf("hl.dsp.focus({ direction = \"%s\" })", lua_direction(direction));
    int rc = hypr_dispatch_versioned(d, legacy, lua, NULL);
    free(legacy); free(lua);
    return rc;
}

/* CloseWindow: versioned dispatch, handles empty id */
static int hypr_close_window(void *priv, const char *id) {
    hyprland_data_t *d = (hyprland_data_t *)priv;
    char *target = hypr_target(id);
    char *legacy = axctl_sprintf("closewindow %s", target);
    char *lua;
    if (target[0] != '\0') {
        lua = axctl_sprintf("hl.dsp.window.close({ window = \"%s\" })", target);
    } else {
        lua = axctl_strdup("hl.dsp.window.close()");
    }
    int rc = hypr_dispatch_versioned(d, legacy, lua, NULL);
    free(target); free(legacy); free(lua);
    return rc;
}

/* MoveWindow: versioned dispatch with direction and optional target */
static int hypr_move_window(void *priv, const char *id, const char *direction) {
    hyprland_data_t *d = (hyprland_data_t *)priv;
    char *legacy;
    if (id && *id) {
        char *target = hypr_target(id);
        legacy = axctl_sprintf("movewindow %s,%s", direction, target);
        free(target);
    } else {
        legacy = axctl_sprintf("movewindow %s", direction);
    }
    char *ltf = lua_target_field(id);
    char *lua = axctl_sprintf("hl.dsp.window.move({ direction = \"%s\"%s })",
                              lua_direction(direction), ltf);
    int rc = hypr_dispatch_versioned(d, legacy, lua, NULL);
    free(legacy); free(ltf); free(lua);
    return rc;
}

/* ResizeWindow: versioned dispatch */
static int hypr_resize_window(void *priv, const char *id, int w, int h) {
    hyprland_data_t *d = (hyprland_data_t *)priv;
    char *target = hypr_target(id);
    char *ltf = lua_target_field(id);
    char *legacy = axctl_sprintf("resizewindowpixel exact %d %d,%s", w, h, target);
    char *lua = axctl_sprintf("hl.dsp.window.resize({ x = %d, y = %d%s })", w, h, ltf);
    int rc = hypr_dispatch_versioned(d, legacy, lua, NULL);
    free(target); free(ltf); free(legacy); free(lua);
    return rc;
}

/* ToggleFloating: versioned dispatch */
static int hypr_toggle_floating(void *priv, const char *id) {
    hyprland_data_t *d = (hyprland_data_t *)priv;
    char *target = hypr_target(id);
    char *ltf = lua_target_field(id);
    char *legacy = axctl_sprintf("togglefloating %s", target);
    char *lua = axctl_sprintf("hl.dsp.window.float({ action = \"toggle\"%s })", ltf);
    int rc = hypr_dispatch_versioned(d, legacy, lua, NULL);
    free(target); free(ltf); free(legacy); free(lua);
    return rc;
}

/* SetFullscreen: must check current state first (matches Go logic) */
static int hypr_set_fullscreen(void *priv, const char *id, int state) {
    hyprland_data_t *d = (hyprland_data_t *)priv;

    /* Get list of windows to check current fullscreen state */
    axctl_window_array_t windows;
    int rc = hypr_list_windows(priv, &windows);
    if (rc != AXCTL_OK) return rc;

    /* Determine target ID */
    char *target_id = NULL;
    if (id && *id) {
        target_id = axctl_strdup(id);
    } else {
        rc = hypr_active_window(priv, &target_id);
        if (rc != AXCTL_OK) {
            axctl_window_array_free(&windows);
            return rc;
        }
    }

    /* Check if the target window is currently fullscreen */
    bool is_fs = false;
    for (size_t i = 0; i < windows.count; i++) {
        if (windows.items[i].id && target_id &&
            strcmp(windows.items[i].id, target_id) == 0) {
            is_fs = windows.items[i].is_fullscreen;
            break;
        }
    }
    axctl_window_array_free(&windows);
    free(target_id);

    /* Only toggle if current state differs from desired state */
    if (is_fs != (bool)state) {
        return hypr_dispatch_versioned(d,
            "fullscreen 0",
            "hl.dsp.window.fullscreen({ mode = \"fullscreen\", action = \"toggle\" })",
            NULL);
    }
    return AXCTL_OK;
}

/* SetMaximized: versioned dispatch with proper fullscreen mode
 * Go: fullscreen 0 (unset) / fullscreen 1 (set), NOT the same as fullscreen toggle */
static int hypr_set_maximized(void *priv, const char *id, int state) {
    hyprland_data_t *d = (hyprland_data_t *)priv;
    (void)id;
    const char *val = state ? "1" : "0";
    const char *action = state ? "set" : "unset";
    char *legacy = axctl_sprintf("fullscreen %s", val);
    char *lua = axctl_sprintf("hl.dsp.window.fullscreen({ mode = \"maximized\", action = \"%s\" })", action);
    int rc = hypr_dispatch_versioned(d, legacy, lua, NULL);
    free(legacy); free(lua);
    return rc;
}

/* PinWindow: versioned dispatch */
static int hypr_pin_window(void *priv, const char *id, int state) {
    hyprland_data_t *d = (hyprland_data_t *)priv;
    (void)state;
    char *target = hypr_target(id);
    char *ltf = lua_target_field(id);
    char *legacy = axctl_sprintf("pin %s", target);
    char *lua = axctl_sprintf("hl.dsp.window.pin({ action = \"toggle\"%s })", ltf);
    int rc = hypr_dispatch_versioned(d, legacy, lua, NULL);
    free(target); free(ltf); free(legacy); free(lua);
    return rc;
}

/* ToggleGroup: versioned dispatch */
static int hypr_toggle_group(void *priv, const char *id) {
    hyprland_data_t *d = (hyprland_data_t *)priv;
    (void)id;
    return hypr_dispatch_versioned(d, "togglegroup", "hl.dsp.group.toggle()", NULL);
}

/* GroupNav: maps direction to b/f, then versioned dispatch
 * Go: l/u/b -> "b" (backward), everything else -> "f" (forward)
 * Lua: "b" -> prev(), "f" -> next() */
static int hypr_group_nav(void *priv, const char *direction) {
    hyprland_data_t *d = (hyprland_data_t *)priv;

    const char *dir = "f";
    if (direction && (strcmp(direction, "l") == 0 ||
                      strcmp(direction, "u") == 0 ||
                      strcmp(direction, "b") == 0)) {
        dir = "b";
    }

    const char *lua_dir = (strcmp(dir, "b") == 0) ? "prev" : "next";

    char *legacy = axctl_sprintf("changegroupactive %s", dir);
    char *lua = axctl_sprintf("hl.dsp.group.%s()", lua_dir);
    int rc = hypr_dispatch_versioned(d, legacy, lua, NULL);
    free(legacy); free(lua);
    return rc;
}

static int hypr_set_layout_property(void *priv, const char *id,
                                      const char *key, const char *value) {
    (void)priv; (void)id; (void)key; (void)value;
    return AXCTL_ERR_NOT_SUPPORTED;
}

/* MoveWindowPixel: versioned dispatch */
static int hypr_move_window_pixel(void *priv, const char *id, int x, int y) {
    hyprland_data_t *d = (hyprland_data_t *)priv;
    char *target = hypr_target(id);
    char *ltf = lua_target_field(id);
    char *legacy = axctl_sprintf("movewindowpixel exact %d %d,%s", x, y, target);
    char *lua = axctl_sprintf("hl.dsp.window.move({ x = %d, y = %d%s })", x, y, ltf);
    int rc = hypr_dispatch_versioned(d, legacy, lua, NULL);
    free(target); free(ltf); free(legacy); free(lua);
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
        ws.monitor_id = axctl_strdup(json_get_string(w, "monitor"));
        ws.is_active = (ws_id == active_id);
        /* Go: IsEmpty = false always (not parsing windows count) */
        ws.is_empty = false;
        /* Add metadata with focused field (matches Go original) */
        ws.metadata = json_object_new_object();
        json_object_object_add(ws.metadata, "focused",
            json_object_new_boolean(ws.is_active));
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
    /* Go: IsEmpty = false */
    out->is_empty = false;
    /* Add metadata with focused field (matches Go original) */
    out->metadata = json_object_new_object();
    json_object_object_add(out->metadata, "focused",
        json_object_new_boolean(1));

    json_object_put(obj);
    return AXCTL_OK;
}

/* SwitchWorkspace: versioned dispatch */
static int hypr_switch_workspace(void *priv, const char *id) {
    hyprland_data_t *d = (hyprland_data_t *)priv;
    char *legacy = axctl_sprintf("workspace %s", id);
    char *lua = axctl_sprintf("hl.dsp.focus({ workspace = \"%s\" })", id);
    int rc = hypr_dispatch_versioned(d, legacy, lua, NULL);
    free(legacy); free(lua);
    return rc;
}

/* MoveToWorkspace: versioned dispatch */
static int hypr_move_to_workspace(void *priv, const char *win_id, const char *ws_id) {
    hyprland_data_t *d = (hyprland_data_t *)priv;
    char *target = hypr_target(win_id);
    char *ltf = lua_target_field(win_id);
    char *legacy = axctl_sprintf("movetoworkspace %s,%s", ws_id, target);
    char *lua = axctl_sprintf("hl.dsp.window.move({ workspace = \"%s\"%s })", ws_id, ltf);
    int rc = hypr_dispatch_versioned(d, legacy, lua, NULL);
    free(target); free(ltf); free(legacy); free(lua);
    return rc;
}

/* MoveToWorkspaceSilent: versioned dispatch, adds follow=false in lua */
static int hypr_move_to_workspace_silent(void *priv, const char *win_id, const char *ws_id) {
    hyprland_data_t *d = (hyprland_data_t *)priv;
    char *target = hypr_target(win_id);
    char *ltf = lua_target_field(win_id);
    char *legacy = axctl_sprintf("movetoworkspacesilent %s,%s", ws_id, target);
    char *lua = axctl_sprintf("hl.dsp.window.move({ workspace = \"%s\", follow = false%s })", ws_id, ltf);
    int rc = hypr_dispatch_versioned(d, legacy, lua, NULL);
    free(target); free(ltf); free(legacy); free(lua);
    return rc;
}

/* ToggleSpecialWorkspace: versioned dispatch */
static int hypr_toggle_special(void *priv, const char *name) {
    hyprland_data_t *d = (hyprland_data_t *)priv;
    char *legacy, *lua;
    if (name && *name) {
        legacy = axctl_sprintf("togglespecialworkspace %s", name);
        lua = axctl_sprintf("hl.dsp.workspace.toggle_special(\"%s\")", name);
    } else {
        legacy = axctl_strdup("togglespecialworkspace");
        lua = axctl_strdup("hl.dsp.workspace.toggle_special()");
    }
    int rc = hypr_dispatch_versioned(d, legacy, lua, NULL);
    free(legacy); free(lua);
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
        /* Go: Description = "" */
        mon.description = axctl_strdup("");
        mon.width = json_get_int(m, "width", 0);
        mon.height = json_get_int(m, "height", 0);
        mon.refresh_rate = json_get_double(m, "refreshRate", 0.0);
        mon.scale = json_get_double(m, "scale", 1.0);
        mon.is_focused = json_get_bool(m, "focused", false);

        struct json_object *meta = json_object_new_object();
        json_object_object_add(meta, "x", json_object_new_int(json_get_int(m, "x", 0)));
        json_object_object_add(meta, "y", json_object_new_int(json_get_int(m, "y", 0)));
        json_object_object_add(meta, "transform", json_object_new_int(json_get_int(m, "transform", 0)));
        
        struct json_object *aw = NULL;
        if (json_object_object_get_ex(m, "activeWorkspace", &aw)) {
            struct json_object *aw_name = NULL;
            if (json_object_object_get_ex(aw, "name", &aw_name)) {
                json_object_object_add(meta, "active_workspace", json_object_get(aw_name));
            } else {
                json_object_object_add(meta, "active_workspace", json_object_new_string(""));
            }
        } else {
            json_object_object_add(meta, "active_workspace", json_object_new_string(""));
        }
        mon.metadata = meta;

        axctl_monitor_array_push(out, mon);
    }

    json_object_put(arr);
    return AXCTL_OK;
}

/* FocusMonitor: versioned dispatch */
static int hypr_focus_monitor(void *priv, const char *id) {
    hyprland_data_t *d = (hyprland_data_t *)priv;
    char *legacy = axctl_sprintf("focusmonitor %s", id);
    char *lua = axctl_sprintf("hl.dsp.focus({ monitor = \"%s\" })", id);
    int rc = hypr_dispatch_versioned(d, legacy, lua, NULL);
    free(legacy); free(lua);
    return rc;
}

/* MoveToMonitor: Go uses "movewindowmon MON,TARGET" (NOT "movewindow mon:MON") */
static int hypr_move_to_monitor(void *priv, const char *win_id, const char *mon_id) {
    hyprland_data_t *d = (hyprland_data_t *)priv;
    char *target = hypr_target(win_id);
    char *ltf = lua_target_field(win_id);
    char *legacy = axctl_sprintf("movewindowmon %s,%s", mon_id, target);
    char *lua = axctl_sprintf("hl.dsp.window.move({ monitor = \"%s\"%s })", mon_id, ltf);
    int rc = hypr_dispatch_versioned(d, legacy, lua, NULL);
    free(target); free(ltf); free(legacy); free(lua);
    return rc;
}

/* SetDpms: versioned dispatch, handles empty monitorID separately (matches Go) */
static int hypr_set_dpms(void *priv, const char *mon_id, int on) {
    hyprland_data_t *d = (hyprland_data_t *)priv;
    const char *state = on ? "on" : "off";

    if (mon_id && *mon_id) {
        char *legacy = axctl_sprintf("dpms %s %s", state, mon_id);
        char *lua = axctl_sprintf("hl.dsp.dpms({ action = \"%s\", monitor = \"%s\" })", state, mon_id);
        int rc = hypr_dispatch_versioned(d, legacy, lua, NULL);
        free(legacy); free(lua);
        return rc;
    }
    char *legacy = axctl_sprintf("dpms %s", state);
    char *lua = axctl_sprintf("hl.dsp.dpms({ action = \"%s\" })", state);
    int rc = hypr_dispatch_versioned(d, legacy, lua, NULL);
    free(legacy); free(lua);
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

/* SetConfig: applies config key mapping before dispatching (matches Go) */
static int hypr_set_config(void *priv, const char *key, const char *value) {
    hyprland_data_t *d = (hyprland_data_t *)priv;
    const char *hypr_key = hypr_map_config_key(key);
    char *cmd = axctl_sprintf("keyword %s %s", hypr_key, value ? value : "");
    int rc = hypr_dispatch_locked(d, cmd, NULL);
    free(cmd);
    return rc;
}

/* BatchConfig: applies config key mapping and uses [[BATCH]] (matches Go) */
static int hypr_batch_config(void *priv, struct json_object *configs) {
    hyprland_data_t *d = (hyprland_data_t *)priv;
    /* Build batch command: Go uses "[[BATCH]]" + join(cmds, ";") */
    char *batch = axctl_strdup("");
    bool first = true;

    json_object_object_foreach(configs, key, val) {
        const char *hypr_key = hypr_map_config_key(key);
        const char *v = json_object_get_string(val);
        char *part;
        if (first) {
            part = axctl_sprintf("keyword %s %s", hypr_key, v ? v : "");
            first = false;
        } else {
            part = axctl_sprintf(";keyword %s %s", hypr_key, v ? v : "");
        }
        axctl_str_append(&batch, part);
        free(part);
    }

    char *cmd = axctl_sprintf("[[BATCH]]%s", batch);
    free(batch);
    int rc = hypr_dispatch_locked(d, cmd, NULL);
    free(cmd);
    return rc;
}

/* BatchKeybinds: matches Go logic exactly
 * Go uses "bind" + flags (e.g. "bindm", "bindl"), NOT just the flags string.
 * Go uses commas as separator (no spaces), NOT "; keyword bind MODS, KEY, ...".
 * Go uses [[BATCH]] prefix with semicolon-separated commands. */
static int hypr_batch_keybinds(void *priv, const char *json_payload) {
    hyprland_data_t *d = (hyprland_data_t *)priv;
    if (!json_payload || !*json_payload) {
        LOG_WARN("batch_keybinds: empty payload");
        return AXCTL_ERR_PARSE;
    }
    struct json_object *root = json_tokener_parse(json_payload);
    if (!root) {
        LOG_ERROR("batch_keybinds: failed to parse JSON payload");
        return AXCTL_ERR_PARSE;
    }

    /* Collect commands into a dynamic array, then join with ";" */
    char *batch_parts = axctl_strdup("");
    bool has_cmds = false;

    /* Process unbinds first (matches Go order) */
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

            /* Go: fmt.Sprintf("keyword unbind %s,%s", mods, u.Key) */
            char *part;
            if (has_cmds) {
                part = axctl_sprintf(";keyword unbind %s,%s", mods_str, key);
            } else {
                part = axctl_sprintf("keyword unbind %s,%s", mods_str, key);
                has_cmds = true;
            }
            axctl_str_append(&batch_parts, part);
            free(part); free(mods_str);
        }
    }

    /* Process binds */
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

            /* Go: bindKeyword = "bind" + flags (e.g. "bindm", "bindl")
             * Strip 'r' (repeat) flag from modifier key binds (Super_L,
             * Super_R, Alt_L, Alt_R, Ctrl_L, Ctrl_R, Shift_L, Shift_R)
             * to prevent the bind from firing 10+ times per press due
             * to the key repeat rate of modifier keys. */
            static const char *mod_keys[] = {
                "Super_L", "Super_R", "Alt_L", "Alt_R",
                "Ctrl_L", "Ctrl_R", "Shift_L", "Shift_R", NULL
            };
            int is_mod = 0;
            if (key) {
                for (int mk = 0; mod_keys[mk]; mk++) {
                    if (strcmp(key, mod_keys[mk]) == 0) { is_mod = 1; break; }
                }
            }
            char *bind_kw;
            if (flags && *flags && is_mod && strchr(flags, 'r')) {
                /* Copy flags without 'r' */
                char clean[16];
                int ci = 0;
                for (int fi = 0; flags[fi] && ci < 15; fi++)
                    if (flags[fi] != 'r') clean[ci++] = flags[fi];
                clean[ci] = '\0';
                bind_kw = clean[0] ? axctl_sprintf("bind%s", clean) : axctl_strdup("bind");
            } else if (flags && *flags) {
                bind_kw = axctl_sprintf("bind%s", flags);
            } else {
                bind_kw = axctl_strdup("bind");
            }

            /* Go: if flags == "m" && argument == "" -> no argument field
             * Otherwise: keyword bindX mods,key,dispatcher,argument */
            char *part;
            const char *sep = has_cmds ? ";" : "";
            if (flags && strcmp(flags, "m") == 0 && (!argument || !*argument)) {
                part = axctl_sprintf("%skeyword %s %s,%s,%s",
                    sep, bind_kw, mods_str, key, dispatcher);
            } else {
                part = axctl_sprintf("%skeyword %s %s,%s,%s,%s",
                    sep, bind_kw, mods_str, key, dispatcher,
                    argument ? argument : "");
            }
            if (!has_cmds) has_cmds = true;
            axctl_str_append(&batch_parts, part);
            free(part); free(mods_str); free(bind_kw);
        }
    }

    json_object_put(root);

    if (!has_cmds) {
        free(batch_parts);
        return AXCTL_OK;
    }

    /* Go: h.dispatch("[[BATCH]]" + strings.Join(cmds, ";")) */
    char *cmd = axctl_sprintf("[[BATCH]]%s", batch_parts);
    free(batch_parts);
    LOG_DEBUG("batch_keybinds: dispatching %zu chars", strlen(cmd));
    int rc = hypr_dispatch_locked(d, cmd, NULL);
    if (rc != 0)
        LOG_ERROR("batch_keybinds: dispatch failed with rc=%d", rc);
    free(cmd);
    return rc;
}

/* RawBatch: Go prepends [[BATCH]] to the command */
static int hypr_raw_batch(void *priv, const char *command) {
    hyprland_data_t *d = (hyprland_data_t *)priv;
    char *cmd = axctl_sprintf("[[BATCH]]%s", command);
    int rc = hypr_dispatch_locked(d, cmd, NULL);
    free(cmd);
    return rc;
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

/* BindKey: Go uses commas without spaces: "keyword bind mods,key,command" */
static int hypr_bind_key(void *priv, const char *mods, const char *key, const char *cmd) {
    hyprland_data_t *d = (hyprland_data_t *)priv;
    char *command = axctl_sprintf("keyword bind %s,%s,%s", mods, key, cmd);
    int rc = hypr_dispatch_locked(d, command, NULL);
    free(command);
    return rc;
}

/* UnbindKey: Go uses commas without spaces: "keyword unbind mods,key" */
static int hypr_unbind_key(void *priv, const char *mods, const char *key) {
    hyprland_data_t *d = (hyprland_data_t *)priv;
    char *cmd = axctl_sprintf("keyword unbind %s,%s", mods, key);
    int rc = hypr_dispatch_locked(d, cmd, NULL);
    free(cmd);
    return rc;
}

/* Helper: escape a string for embedding in a Lua double-quoted literal.
 * Matches Go's fmt.Sprintf(%q, ...) behavior for safe Lua strings.
 * Caller must free the returned string. */
static char *lua_escape_for_cmd(const char *s) {
    if (!s || !*s) return axctl_strdup("\"\"");
    size_t len = strlen(s);
    char *buf = malloc(len * 2 + 3);
    if (!buf) return axctl_strdup("\"\"");
    char *p = buf;
    *p++ = '"';
    for (size_t i = 0; i < len; i++) {
        if (s[i] == '"' || s[i] == '\\') *p++ = '\\';
        else if (s[i] == '\n') { *p++ = '\\'; *p++ = 'n'; continue; }
        else if (s[i] == '\t') { *p++ = '\\'; *p++ = 't'; continue; }
        *p++ = s[i];
    }
    *p++ = '"';
    *p = '\0';
    return buf;
}

/* Execute: versioned dispatch */
static int hypr_execute(void *priv, const char *command) {
    hyprland_data_t *d = (hyprland_data_t *)priv;
    char *legacy = axctl_sprintf("exec %s", command);
    char *qcmd = lua_escape_for_cmd(command);
    char *lua = axctl_sprintf("hl.dsp.exec_cmd(%s)", qcmd);
    int rc = hypr_dispatch_versioned(d, legacy, lua, NULL);
    free(legacy); free(lua); free(qcmd);
    return rc;
}

/* Exit: versioned dispatch */
static int hypr_exit(void *priv) {
    hyprland_data_t *d = (hyprland_data_t *)priv;
    return hypr_dispatch_versioned(d, "exit", "hl.dsp.exit()", NULL);
}

/* SwitchKeyboardLayout: Go uses "current" NOT "all" */
static int hypr_switch_keyboard_layout(void *priv, const char *action) {
    hyprland_data_t *d = (hyprland_data_t *)priv;
    char *cmd = axctl_sprintf("switchxkblayout current %s", action);
    int rc = hypr_dispatch_locked(d, cmd, NULL);
    free(cmd);
    return rc;
}

/* SetKeyboardLayouts: Go sends two separate dispatches, not a batch.
 * Also clears variant if variants is empty. */
static int hypr_set_keyboard_layouts(void *priv, const char *layouts, const char *variants) {
    hyprland_data_t *d = (hyprland_data_t *)priv;

    char *cmd1 = axctl_sprintf("keyword input:kb_layout %s", layouts);
    int rc = hypr_dispatch_locked(d, cmd1, NULL);
    free(cmd1);
    if (rc != AXCTL_OK) return rc;

    /* Go: if variants != "" send it, else send empty to clear */
    char *cmd2 = axctl_sprintf("keyword input:kb_variant %s", (variants && *variants) ? variants : "");
    rc = hypr_dispatch_locked(d, cmd2, NULL);
    free(cmd2);
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
