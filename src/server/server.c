/* src/server/server.c -- JSON-RPC server over Unix socket
 *
 * Listens on /tmp/axctl-<uid>.sock for JSON-RPC requests.
 * Routes 60+ methods across Window, Workspace, Monitor, Layout,
 * Config, and System categories.
 *
 * Maintains a state cache (windows, workspaces, monitors) and
 * broadcasts events to subscribed clients.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <pthread.h>
#include <stdarg.h>
#include <time.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <json-c/json.h>

#include "server/server.h"
#include "server/idle.h"
#include "server/config_handler.h"
#include "ipc/compositor.h"
#include "ipc/cache.h"
#include "ipc/types.h"
#include "ipc/errors.h"
#include "ipc/config_types.h"
#include "utils/log.h"
#include "utils/strutil.h"
#include "utils/json_helpers.h"

/* ------------------------------------------------------------------ */
/* Debug logging to /tmp/axctl_debug.log                               */
/* ------------------------------------------------------------------ */

static FILE *g_debug_log = NULL;

static void debug_log(const char *fmt, ...) {
    if (!g_debug_log) {
        g_debug_log = fopen("/tmp/axctl_debug.log", "a");
        if (!g_debug_log) return;
        setvbuf(g_debug_log, NULL, _IOLBF, 0); /* line-buffered */
    }
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    struct tm tm;
    localtime_r(&ts.tv_sec, &tm);
    fprintf(g_debug_log, "[%02d:%02d:%02d.%03ld] ",
            tm.tm_hour, tm.tm_min, tm.tm_sec, ts.tv_nsec / 1000000);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(g_debug_log, fmt, ap);
    va_end(ap);
    fprintf(g_debug_log, "\n");
}

/* ------------------------------------------------------------------ */
/* Server context                                                      */
/* ------------------------------------------------------------------ */

#define MAX_CLIENTS 64
#define READ_BUF_SIZE 65536

struct axctl_server {
    axctl_compositor_t    *compositor;
    char                  *socket_path;
    axctl_state_cache_t   *cache;
    axctl_idle_manager_t  *idle_mgr;

    int                    listen_fd;

    /* Subscribed clients */
    int        client_fds[MAX_CLIENTS];
    int        client_count;
    pthread_mutex_t clients_mu;
};

/* Forward declarations */
static void server_init_cache(axctl_server_t *s);
static void *server_event_watcher(void *arg);
static void handle_connection(axctl_server_t *s, int fd);
static json_object *dispatch_method(axctl_server_t *s, const char *method,
                                    json_object *params, int client_fd);
static void broadcast_event(axctl_server_t *s, const char *method,
                            json_object *params);
static void idle_monitor_changed_cb(uint32_t id, int is_idle, void *userdata);

/* ------------------------------------------------------------------ */
/* Helpers                                                             */
/* ------------------------------------------------------------------ */

static char *resolve_id(axctl_server_t *s, const char *id)
{
    if (id && *id) return axctl_strdup(id);
    char *active = NULL;
    s->compositor->active_window(s->compositor->priv, &active);
    return active ? active : axctl_strdup("");
}

/* Build a state dump JSON for event broadcasts */
static json_object *build_state_dump(axctl_server_t *s)
{
    json_object *dump = json_object_new_object();

    /* Windows */
    axctl_window_array_t windows;
    windows = axctl_cache_get_windows(s->cache);
    json_object *w_arr = json_object_new_array();
    for (size_t i = 0; i < windows.count; i++) {
        json_object *wj = axctl_window_to_json(&windows.items[i]);
        json_object_array_add(w_arr, wj);
    }
    json_object_object_add(dump, "windows", w_arr);
    debug_log("BUILD_STATE_DUMP: windows=%zu", windows.count);
    axctl_window_array_free(&windows);

    /* Workspaces */
    axctl_workspace_array_t workspaces;
    workspaces = axctl_cache_get_workspaces(s->cache);
    json_object *ws_arr = json_object_new_array();
    for (size_t i = 0; i < workspaces.count; i++) {
        json_object *wsj = axctl_workspace_to_json(&workspaces.items[i]);
        json_object_array_add(ws_arr, wsj);
    }
    json_object_object_add(dump, "workspaces", ws_arr);
    debug_log("BUILD_STATE_DUMP: workspaces=%zu", workspaces.count);
    axctl_workspace_array_free(&workspaces);

    /* Monitors */
    axctl_monitor_array_t monitors;
    monitors = axctl_cache_get_monitors(s->cache);
    json_object *m_arr = json_object_new_array();
    for (size_t i = 0; i < monitors.count; i++) {
        json_object *mj = axctl_monitor_to_json(&monitors.items[i]);
        json_object_array_add(m_arr, mj);
        /* Log each monitor's active_workspace for multi-monitor debugging */
        const char *aw = "";
        if (monitors.items[i].metadata) {
            json_object *jaw = NULL;
            if (json_object_object_get_ex(monitors.items[i].metadata, "active_workspace", &jaw))
                aw = json_object_get_string(jaw);
        }
        debug_log("  STATE_MON[%zu]: id=%s name=%s active_ws=%s",
                  i, monitors.items[i].id ? monitors.items[i].id : "(null)",
                  monitors.items[i].name ? monitors.items[i].name : "(null)", aw);
    }
    json_object_object_add(dump, "monitors", m_arr);
    debug_log("BUILD_STATE_DUMP: monitors=%zu", monitors.count);
    axctl_monitor_array_free(&monitors);

    return dump;
}

/* ------------------------------------------------------------------ */
/* Server lifecycle                                                    */
/* ------------------------------------------------------------------ */

axctl_server_t *axctl_server_create(axctl_compositor_t *comp, const char *path)
{
    axctl_server_t *s = calloc(1, sizeof(*s));
    if (!s) return NULL;

    s->compositor = comp;
    s->socket_path = axctl_strdup(path);
    s->cache = axctl_cache_new();
    s->listen_fd = -1;
    pthread_mutex_init(&s->clients_mu, NULL);

    /* Create idle manager (non-fatal if it fails) */
    s->idle_mgr = axctl_idle_manager_create();
    if (s->idle_mgr) {
        axctl_idle_manager_set_callback(s->idle_mgr,
                                         idle_monitor_changed_cb, s);
    } else {
        LOG_WARN("Failed to initialize idle manager");
    }

    /* Initialize cache from compositor */
    server_init_cache(s);

    /* Start event watcher thread */
    pthread_t tid;
    pthread_create(&tid, NULL, server_event_watcher, s);
    pthread_detach(tid);

    return s;
}

static void server_init_cache(axctl_server_t *s)
{
    debug_log("INIT_CACHE: refreshing all cache data");

    axctl_window_array_t windows;
    if (s->compositor->list_windows(s->compositor->priv, &windows) == 0) {
        debug_log("INIT_CACHE: got %zu windows", windows.count);
        for (size_t i = 0; i < windows.count; i++) {
            debug_log("  WIN[%zu]: id=%s app=%s ws=%s focused=%d floating=%d",
                      i, windows.items[i].id ? windows.items[i].id : "(null)",
                      windows.items[i].app_id ? windows.items[i].app_id : "(null)",
                      windows.items[i].workspace_id ? windows.items[i].workspace_id : "(null)",
                      windows.items[i].is_focused, windows.items[i].is_floating);
        }
        axctl_cache_set_windows(s->cache, &windows);
        axctl_window_array_free(&windows);
    } else {
        debug_log("INIT_CACHE: list_windows FAILED");
    }

    /* Mark active window */
    char *active_id = NULL;
    if (s->compositor->active_window(s->compositor->priv, &active_id) == 0 &&
        active_id && *active_id) {
        debug_log("INIT_CACHE: active_window=%s", active_id);
        axctl_cache_mark_window_focused(s->cache, active_id);
    }
    free(active_id);

    axctl_workspace_array_t workspaces;
    if (s->compositor->list_workspaces(s->compositor->priv, &workspaces) == 0) {
        debug_log("INIT_CACHE: got %zu workspaces", workspaces.count);
        for (size_t i = 0; i < workspaces.count; i++) {
            debug_log("  WS[%zu]: id=%s name=%s mon=%s active=%d",
                      i, workspaces.items[i].id ? workspaces.items[i].id : "(null)",
                      workspaces.items[i].name ? workspaces.items[i].name : "(null)",
                      workspaces.items[i].monitor_id ? workspaces.items[i].monitor_id : "(null)",
                      workspaces.items[i].is_active);
        }
        axctl_cache_set_workspaces(s->cache, &workspaces);
        axctl_workspace_array_free(&workspaces);
    } else {
        debug_log("INIT_CACHE: list_workspaces FAILED");
    }

    axctl_monitor_array_t monitors;
    if (s->compositor->list_monitors(s->compositor->priv, &monitors) == 0) {
        debug_log("INIT_CACHE: got %zu monitors", monitors.count);
        for (size_t i = 0; i < monitors.count; i++) {
            const char *aw = "";
            if (monitors.items[i].metadata) {
                json_object *jaw = NULL;
                if (json_object_object_get_ex(monitors.items[i].metadata, "active_workspace", &jaw))
                    aw = json_object_get_string(jaw);
            }
            debug_log("  MON[%zu]: id=%s name=%s focused=%d active_ws=%s",
                      i, monitors.items[i].id ? monitors.items[i].id : "(null)",
                      monitors.items[i].name ? monitors.items[i].name : "(null)",
                      monitors.items[i].is_focused, aw);
        }
        axctl_cache_set_monitors(s->cache, &monitors);
        axctl_monitor_array_free(&monitors);
    } else {
        debug_log("INIT_CACHE: list_monitors FAILED");
    }
}

/* ------------------------------------------------------------------ */
/* Event watcher: subscribes to compositor events, updates cache,      */
/* broadcasts to subscribed clients.                                   */
/* ------------------------------------------------------------------ */

static void event_callback(const axctl_event_t *event, void *userdata)
{
    axctl_server_t *s = (axctl_server_t *)userdata;

    debug_log("EVENT: type=%d (%s) payload=%s",
              event->type, axctl_event_type_str(event->type),
              event->payload ? json_object_to_json_string_ext(event->payload, JSON_C_TO_STRING_PLAIN) : "(null)");

    switch (event->type) {
    case EVENT_WINDOW_CREATED:
        if (event->window) {
            axctl_cache_add_window(s->cache, *event->window);
        }
        broadcast_event(s, "Event.WindowCreated",
                        event->payload ? json_object_get(event->payload) : NULL);
        break;

    case EVENT_WINDOW_CLOSED: {
        const char *id = NULL;
        if (event->payload) {
            json_object *jid = NULL;
            if (json_object_object_get_ex(event->payload, "address", &jid) ||
                json_object_object_get_ex(event->payload, "id", &jid)) {
                id = json_object_get_string(jid);
            }
        }
        if (id) axctl_cache_remove_window(s->cache, id);
        json_object *p = json_object_new_object();
        json_object_object_add(p, "ID",
            json_object_new_string(id ? id : ""));
        broadcast_event(s, "Event.WindowClosed", p);
        break;
    }

    case EVENT_WINDOW_FOCUSED:
        if (event->payload) {
            json_object *jaddr = NULL;
            if (json_object_object_get_ex(event->payload, "address", &jaddr)) {
                axctl_cache_mark_window_focused(s->cache,
                    json_object_get_string(jaddr));
            }
        }
        if (event->window && event->window->id) {
            axctl_cache_add_window(s->cache, *event->window);
        }
        broadcast_event(s, "Event.WindowFocused",
                        event->payload ? json_object_get(event->payload) : NULL);
        break;

    case EVENT_WINDOW_TITLE_CHANGED: {
        const char *id = NULL, *title = NULL;
        if (event->payload) {
            json_object *jid = NULL, *jt = NULL;
            if (json_object_object_get_ex(event->payload, "address", &jid) ||
                json_object_object_get_ex(event->payload, "id", &jid))
                id = json_object_get_string(jid);
            if (json_object_object_get_ex(event->payload, "title", &jt))
                title = json_object_get_string(jt);
        }
        if (id && title)
            axctl_cache_update_window_title(s->cache, id, title);
        broadcast_event(s, "Event.WindowTitleChanged",
                        event->payload ? json_object_get(event->payload) : NULL);
        break;
    }

    case EVENT_WORKSPACE_CHANGED: {
        server_init_cache(s);
        /* Go normalises params to {"Name": name} when name is available */
        json_object *ws_params = NULL;
        if (event->payload) {
            json_object *jname = NULL;
            if (json_object_object_get_ex(event->payload, "name", &jname)) {
                ws_params = json_object_new_object();
                json_object_object_add(ws_params, "Name",
                    json_object_new_string(json_object_get_string(jname)));
            } else {
                ws_params = json_object_get(event->payload);
            }
        }
        broadcast_event(s, "Event.WorkspaceChanged", ws_params);
        break;
    }

    case EVENT_WINDOW_MOVED: {
        const char *id = NULL, *ws = NULL, *mon = NULL;
        if (event->payload) {
            json_object *jid = NULL, *jws = NULL, *jm = NULL;
            if (json_object_object_get_ex(event->payload, "address", &jid) ||
                json_object_object_get_ex(event->payload, "id", &jid))
                id = json_object_get_string(jid);
            if (json_object_object_get_ex(event->payload, "workspace", &jws))
                ws = json_object_get_string(jws);
            if (json_object_object_get_ex(event->payload, "monitor", &jm))
                mon = json_object_get_string(jm);
        }
        if (id && ws)
            axctl_cache_update_window_workspace(s->cache, id, ws, mon);
        /* Go normalises params to {"ID": id, "WorkspaceID": ws} */
        json_object *mv_params = NULL;
        if (id && ws) {
            mv_params = json_object_new_object();
            json_object_object_add(mv_params, "ID",
                json_object_new_string(id));
            json_object_object_add(mv_params, "WorkspaceID",
                json_object_new_string(ws));
        } else if (event->payload) {
            mv_params = json_object_get(event->payload);
        }
        broadcast_event(s, "Event.WindowMoved", mv_params);
        break;
    }

    case EVENT_MONITOR_CHANGED:
        server_init_cache(s);
        broadcast_event(s, "Event.MonitorChanged",
                        event->payload ? json_object_get(event->payload) : NULL);
        break;

    case EVENT_CONFIG_RELOADED:
        server_init_cache(s);
        broadcast_event(s, "Event.ConfigReloaded", NULL);
        break;

    case EVENT_FULLSCREEN_CHANGED: {
        const char *id = NULL;
        int fs = 0;
        if (event->payload) {
            json_object *jid = NULL, *jfs = NULL;
            if (json_object_object_get_ex(event->payload, "address", &jid) ||
                json_object_object_get_ex(event->payload, "id", &jid))
                id = json_object_get_string(jid);
            if (json_object_object_get_ex(event->payload, "fullscreen", &jfs))
                fs = json_object_get_boolean(jfs);
        }
        if (id)
            axctl_cache_update_window_state(s->cache, id, fs);
        broadcast_event(s, "Event.FullscreenChanged",
                        event->payload ? json_object_get(event->payload) : NULL);
        break;
    }

    case EVENT_FOCUSED_MONITOR_CHANGED:
        server_init_cache(s);
        broadcast_event(s, "Event.FocusedMonitorChanged",
                        event->payload ? json_object_get(event->payload) : NULL);
        break;

    case EVENT_FLOATING_CHANGED: {
        /* Update floating state in cache (matches Go behavior) */
        const char *fid = NULL;
        int floating = 0;
        if (event->payload) {
            json_object *jaddr = NULL, *jfloat = NULL;
            if (json_object_object_get_ex(event->payload, "address", &jaddr) ||
                json_object_object_get_ex(event->payload, "id", &jaddr))
                fid = json_object_get_string(jaddr);
            if (json_object_object_get_ex(event->payload, "floating", &jfloat))
                floating = json_object_get_boolean(jfloat);
        }
        if (fid)
            axctl_cache_update_window_floating(s->cache, fid, floating);
        broadcast_event(s, "Event.FloatingChanged",
                        event->payload ? json_object_get(event->payload) : NULL);
        break;
    }

    default:
        server_init_cache(s);
        broadcast_event(s, "Event.CacheRefreshed", NULL);
        break;
    }
}

static void *server_event_watcher(void *arg)
{
    axctl_server_t *s = (axctl_server_t *)arg;
    s->compositor->subscribe(s->compositor->priv, event_callback, s);
    return NULL;
}

/* ------------------------------------------------------------------ */
/* Broadcast                                                           */
/* ------------------------------------------------------------------ */

static void broadcast_event(axctl_server_t *s, const char *method,
                            json_object *params)
{
    pthread_mutex_lock(&s->clients_mu);
    debug_log("BROADCAST: method=%s clients=%d params=%s",
              method, s->client_count,
              params ? json_object_to_json_string_ext(params, JSON_C_TO_STRING_PLAIN) : "(null)");

    if (s->client_count == 0) {
        debug_log("BROADCAST: no subscribers, skipping");
        pthread_mutex_unlock(&s->clients_mu);
        if (params) json_object_put(params);
        return;
    }

    json_object *notif = json_object_new_object();
    json_object_object_add(notif, "jsonrpc", json_object_new_string("2.0"));
    json_object_object_add(notif, "method", json_object_new_string(method));
    if (params)
        json_object_object_add(notif, "params", params);
    json_object_object_add(notif, "state", build_state_dump(s));

    /* Use compact format matching Go's json.Marshal (no extra spaces) */
    const char *data = json_object_to_json_string_ext(notif,
                                                       JSON_C_TO_STRING_PLAIN);
    size_t dlen = strlen(data);
    debug_log("BROADCAST: sending %zu bytes to %d client(s)", dlen, s->client_count);

    for (int i = 0; i < s->client_count; i++) {
        /* Non-blocking write; drop if client is slow */
        ssize_t w1 = write(s->client_fds[i], data, dlen);
        ssize_t w2 = write(s->client_fds[i], "\n", 1);
        if (w1 < 0 || w2 < 0) {
            debug_log("BROADCAST: write error to client fd=%d: %s",
                      s->client_fds[i], strerror(errno));
        }
    }

    json_object_put(notif);
    pthread_mutex_unlock(&s->clients_mu);
}

static void idle_monitor_changed_cb(uint32_t id, int is_idle, void *userdata)
{
    axctl_server_t *s = (axctl_server_t *)userdata;
    json_object *params = json_object_new_object();
    json_object_object_add(params, "id", json_object_new_int(id));
    json_object_object_add(params, "is_idle", json_object_new_boolean(is_idle));
    broadcast_event(s, "Event.IdleMonitorChanged", params);
}

/* ------------------------------------------------------------------ */
/* Method dispatch                                                     */
/* ------------------------------------------------------------------ */

/* Helper: extract string from JSON params */
static const char *param_str(json_object *params, const char *key)
{
    json_object *val = NULL;
    if (!params || !json_object_object_get_ex(params, key, &val))
        return NULL;
    return json_object_get_string(val);
}

/* Safe version that returns "" instead of NULL for use in printf/sprintf */
static const char *param_str_safe(json_object *params, const char *key)
{
    const char *s = param_str(params, key);
    return s ? s : "";
}

static int param_int(json_object *params, const char *key, int def)
{
    json_object *val = NULL;
    if (!params || !json_object_object_get_ex(params, key, &val))
        return def;
    return json_object_get_int(val);
}

static int param_bool(json_object *params, const char *key, int def)
{
    json_object *val = NULL;
    if (!params || !json_object_object_get_ex(params, key, &val))
        return def;
    return json_object_get_boolean(val);
}

static json_object *dispatch_method(axctl_server_t *s, const char *method,
                                    json_object *params, int client_fd)
{
    axctl_compositor_t *c = s->compositor;
    json_object *result = NULL;
    int rc = 0;

    /* ---- Window methods ---- */
    if (strcmp(method, "Window.List") == 0) {
        axctl_window_array_t windows;
        windows = axctl_cache_get_windows(s->cache);
        result = json_object_new_array();
        for (size_t i = 0; i < windows.count; i++)
            json_object_array_add(result, axctl_window_to_json(&windows.items[i]));
        axctl_window_array_free(&windows);
    }
    else if (strcmp(method, "Window.Active") == 0) {
        char *id = NULL;
        rc = c->active_window(c->priv, &id);
        if (rc == 0) {
            result = json_object_new_object();
            json_object_object_add(result, "id",
                json_object_new_string(id ? id : ""));
        }
        free(id);
    }
    else if (strcmp(method, "Window.Focus") == 0) {
        rc = c->focus_window(c->priv, param_str(params, "id"));
    }
    else if (strcmp(method, "Window.FocusDir") == 0) {
        rc = c->focus_dir(c->priv, param_str_safe(params, "direction"));
    }
    else if (strcmp(method, "Window.Close") == 0) {
        char *id = resolve_id(s, param_str(params, "id"));
        rc = c->close_window(c->priv, id);
        free(id);
    }
    else if (strcmp(method, "Window.Move") == 0) {
        char *id = resolve_id(s, param_str(params, "id"));
        rc = c->move_window(c->priv, id, param_str_safe(params, "direction"));
        free(id);
    }
    else if (strcmp(method, "Window.Resize") == 0) {
        char *id = resolve_id(s, param_str(params, "id"));
        rc = c->resize_window(c->priv, id,
                              param_int(params, "width", 0),
                              param_int(params, "height", 0));
        free(id);
    }
    else if (strcmp(method, "Window.ToggleFloating") == 0) {
        char *id = resolve_id(s, param_str(params, "id"));
        rc = c->toggle_floating(c->priv, id);
        free(id);
    }
    else if (strcmp(method, "Window.Fullscreen") == 0) {
        char *id = resolve_id(s, param_str(params, "id"));
        rc = c->set_fullscreen(c->priv, id, param_bool(params, "state", 0));
        free(id);
    }
    else if (strcmp(method, "Window.Maximize") == 0) {
        char *id = resolve_id(s, param_str(params, "id"));
        rc = c->set_maximized(c->priv, id, param_bool(params, "state", 0));
        free(id);
    }
    else if (strcmp(method, "Window.Pin") == 0) {
        char *id = resolve_id(s, param_str(params, "id"));
        rc = c->pin_window(c->priv, id, param_bool(params, "state", 0));
        free(id);
    }
    else if (strcmp(method, "Window.ToggleGroup") == 0) {
        char *id = resolve_id(s, param_str(params, "id"));
        rc = c->toggle_group(c->priv, id);
        free(id);
    }
    else if (strcmp(method, "Window.GroupNav") == 0) {
        rc = c->group_nav(c->priv, param_str_safe(params, "direction"));
    }
    else if (strcmp(method, "Window.LayoutProp") == 0) {
        char *id = resolve_id(s, param_str(params, "id"));
        rc = c->set_layout_property(c->priv, id,
                                    param_str(params, "key"),
                                    param_str(params, "value"));
        free(id);
    }
    else if (strcmp(method, "Window.MovePixel") == 0) {
        char *id = resolve_id(s, param_str(params, "id"));
        rc = c->move_window_pixel(c->priv, id,
                                  param_int(params, "x", 0),
                                  param_int(params, "y", 0));
        free(id);
    }
    else if (strcmp(method, "Window.MoveToWorkspaceSilent") == 0) {
        char *id = resolve_id(s, param_str(params, "window_id"));
        rc = c->move_to_workspace_silent(c->priv, id,
                                         param_str(params, "workspace_id"));
        free(id);
    }

    /* ---- Workspace methods ---- */
    else if (strcmp(method, "Workspace.List") == 0) {
        axctl_workspace_array_t ws;
        ws = axctl_cache_get_workspaces(s->cache);
        result = json_object_new_array();
        for (size_t i = 0; i < ws.count; i++)
            json_object_array_add(result, axctl_workspace_to_json(&ws.items[i]));
        axctl_workspace_array_free(&ws);
    }
    else if (strcmp(method, "Workspace.Active") == 0) {
        axctl_workspace_t ws = {0};
        rc = c->active_workspace(c->priv, &ws);
        if (rc == 0) {
            result = axctl_workspace_to_json(&ws);
            axctl_workspace_free(&ws);
        }
    }
    else if (strcmp(method, "Workspace.Switch") == 0) {
        rc = c->switch_workspace(c->priv, param_str(params, "id"));
    }
    else if (strcmp(method, "Workspace.MoveTo") == 0) {
        char *id = resolve_id(s, param_str(params, "window_id"));
        rc = c->move_to_workspace(c->priv, id,
                                  param_str(params, "workspace_id"));
        free(id);
    }
    else if (strcmp(method, "Workspace.ToggleSpecial") == 0) {
        rc = c->toggle_special_workspace(c->priv, param_str(params, "name"));
    }

    /* ---- Monitor methods ---- */
    else if (strcmp(method, "Monitor.List") == 0) {
        axctl_monitor_array_t mons;
        mons = axctl_cache_get_monitors(s->cache);
        result = json_object_new_array();
        for (size_t i = 0; i < mons.count; i++)
            json_object_array_add(result, axctl_monitor_to_json(&mons.items[i]));
        axctl_monitor_array_free(&mons);
    }
    else if (strcmp(method, "Monitor.Focus") == 0) {
        rc = c->focus_monitor(c->priv, param_str(params, "id"));
    }
    else if (strcmp(method, "Monitor.MoveTo") == 0) {
        char *id = resolve_id(s, param_str(params, "window_id"));
        rc = c->move_to_monitor(c->priv, id, param_str(params, "monitor_id"));
        free(id);
    }
    else if (strcmp(method, "Monitor.SetDpms") == 0) {
        rc = c->set_dpms(c->priv, param_str(params, "monitor_id"),
                         param_bool(params, "on", 1));
    }

    /* ---- Layout methods ---- */
    else if (strcmp(method, "Layout.Set") == 0) {
        rc = c->set_layout(c->priv, param_str(params, "name"));
    }

    /* ---- Config methods ---- */
    else if (strcmp(method, "Config.Get") == 0) {
        rc = c->get_config(c->priv, param_str(params, "key"), &result);
    }
    else if (strcmp(method, "Config.Set") == 0) {
        const char *key = param_str(params, "key");
        json_object *val_obj = NULL;
        json_object_object_get_ex(params, "value", &val_obj);
        const char *val = val_obj ? json_object_get_string(val_obj) : "";
        rc = c->set_config(c->priv, key, val);
    }
    else if (strcmp(method, "Config.Apply") == 0) {
        const char *payload = param_str(params, "payload");
        if (payload) {
            axctl_config_universal_t ucfg = {0};
            rc = axctl_config_universal_from_json(payload, &ucfg);
            if (rc == AXCTL_OK) {
                rc = axctl_config_handler_apply(c, &ucfg, NULL);
                axctl_config_universal_free(&ucfg);
            }
        } else {
            rc = -1;
            axctl_set_error("missing payload");
        }
    }
    else if (strcmp(method, "Config.Batch") == 0) {
        json_object *configs = NULL;
        json_object_object_get_ex(params, "configs", &configs);
        if (configs) {
            rc = c->batch_config(c->priv, configs);
        }
    }
    else if (strcmp(method, "Config.KeybindsBatch") == 0) {
        rc = c->batch_keybinds(c->priv, param_str(params, "payload"));
    }
    else if (strcmp(method, "Config.RawBatch") == 0) {
        rc = c->raw_batch(c->priv, param_str(params, "command"));
    }
    else if (strcmp(method, "Config.Reload") == 0) {
        rc = c->reload_config(c->priv);
    }
    else if (strcmp(method, "Config.GetAnimations") == 0) {
        rc = c->get_animations(c->priv, &result);
    }
    else if (strcmp(method, "Config.BindKey") == 0) {
        rc = c->bind_key(c->priv, param_str(params, "mods"),
                         param_str(params, "key"),
                         param_str(params, "command"));
    }
    else if (strcmp(method, "Config.UnbindKey") == 0) {
        rc = c->unbind_key(c->priv, param_str(params, "mods"),
                           param_str(params, "key"));
    }

    /* ---- System methods ---- */
    else if (strcmp(method, "System.Execute") == 0) {
        rc = c->execute(c->priv, param_str(params, "command"));
    }
    else if (strcmp(method, "System.GetCursorPosition") == 0) {
        int x = 0, y = 0;
        rc = c->get_cursor_position(c->priv, &x, &y);
        if (rc == 0) {
            result = json_object_new_object();
            json_object_object_add(result, "x", json_object_new_int(x));
            json_object_object_add(result, "y", json_object_new_int(y));
        }
    }
    else if (strcmp(method, "System.Exit") == 0) {
        rc = c->compositor_exit(c->priv);
    }
    else if (strcmp(method, "System.SwitchKeyboardLayout") == 0) {
        const char *action = param_str(params, "action");
        if (!action) action = "next";
        rc = c->switch_keyboard_layout(c->priv, action);
    }
    else if (strcmp(method, "System.SetKeyboardLayouts") == 0) {
        rc = c->set_keyboard_layouts(c->priv,
                                     param_str(params, "layouts"),
                                     param_str(params, "variants"));
    }

    /* ---- Idle management methods ---- */
    else if (strcmp(method, "System.IdleInhibit") == 0) {
        if (!s->idle_mgr) { axctl_set_error("idle not supported"); rc = -1; }
        else rc = axctl_idle_inhibit(s->idle_mgr, param_bool(params, "on", 0));
    }
    else if (strcmp(method, "System.IdleWait") == 0) {
        if (!s->idle_mgr) { axctl_set_error("idle not supported"); rc = -1; }
        else rc = axctl_idle_wait(s->idle_mgr, (uint32_t)param_int(params, "timeout_ms", 0));
    }
    else if (strcmp(method, "System.ResumeWait") == 0) {
        if (!s->idle_mgr) { axctl_set_error("idle not supported"); rc = -1; }
        else rc = axctl_idle_wait_resume(s->idle_mgr, (uint32_t)param_int(params, "timeout_ms", 0));
    }
    else if (strcmp(method, "System.InputIdleWait") == 0) {
        if (!s->idle_mgr) { axctl_set_error("idle not supported"); rc = -1; }
        else rc = axctl_idle_wait_input(s->idle_mgr, (uint32_t)param_int(params, "timeout_ms", 0));
    }
    else if (strcmp(method, "System.InputResumeWait") == 0) {
        if (!s->idle_mgr) { axctl_set_error("idle not supported"); rc = -1; }
        else rc = axctl_idle_wait_input_resume(s->idle_mgr, (uint32_t)param_int(params, "timeout_ms", 0));
    }
    else if (strcmp(method, "System.IsIdle") == 0) {
        if (!s->idle_mgr) { axctl_set_error("idle not supported"); rc = -1; }
        else {
            int is_idle = 0;
            rc = axctl_idle_is_idle(s->idle_mgr, (uint32_t)param_int(params, "timeout_ms", 0), &is_idle);
            if (rc == 0) result = json_object_new_string(is_idle ? "true" : "false");
        }
    }
    else if (strcmp(method, "System.IsInputIdle") == 0) {
        if (!s->idle_mgr) { axctl_set_error("idle not supported"); rc = -1; }
        else {
            int is_idle = 0;
            rc = axctl_idle_is_input_idle(s->idle_mgr, (uint32_t)param_int(params, "timeout_ms", 0), &is_idle);
            if (rc == 0) result = json_object_new_string(is_idle ? "true" : "false");
        }
    }
    else if (strcmp(method, "System.IsInhibited") == 0) {
        if (!s->idle_mgr) { axctl_set_error("idle not supported"); rc = -1; }
        else result = json_object_new_string(axctl_idle_is_inhibited(s->idle_mgr) ? "true" : "false");
    }
    else if (strcmp(method, "System.IdleMonitorCreate") == 0) {
        if (!s->idle_mgr) { axctl_set_error("idle not supported"); rc = -1; }
        else {
            axctl_idle_monitor_state_t st;
            rc = axctl_idle_monitor_create(s->idle_mgr,
                (uint32_t)param_int(params, "timeout_ms", 0),
                param_bool(params, "respect_inhibitors", 1),
                param_bool(params, "enabled", 1), &st);
            if (rc == 0) {
                result = json_object_new_object();
                json_object_object_add(result, "id", json_object_new_int(st.id));
                json_object_object_add(result, "enabled", json_object_new_boolean(st.enabled));
                json_object_object_add(result, "timeout_ms", json_object_new_int(st.timeout_ms));
                json_object_object_add(result, "respect_inhibitors", json_object_new_boolean(st.respect_inhibitors));
                json_object_object_add(result, "is_idle", json_object_new_boolean(st.is_idle));
            }
        }
    }
    else if (strcmp(method, "System.IdleMonitorUpdate") == 0) {
        if (!s->idle_mgr) { axctl_set_error("idle not supported"); rc = -1; }
        else {
            axctl_idle_monitor_state_t current;
            uint32_t mid = (uint32_t)param_int(params, "id", 0);
            rc = axctl_idle_monitor_get(s->idle_mgr, mid, &current);
            if (rc == 0) {
                json_object *j_tms = NULL, *j_ri = NULL, *j_en = NULL;
                uint32_t tms = current.timeout_ms;
                int ri = current.respect_inhibitors, en = current.enabled;
                if (json_object_object_get_ex(params, "timeout_ms", &j_tms))
                    tms = (uint32_t)json_object_get_int(j_tms);
                if (json_object_object_get_ex(params, "respect_inhibitors", &j_ri))
                    ri = json_object_get_boolean(j_ri);
                if (json_object_object_get_ex(params, "enabled", &j_en))
                    en = json_object_get_boolean(j_en);
                axctl_idle_monitor_state_t st;
                rc = axctl_idle_monitor_update(s->idle_mgr, mid, tms, ri, en, &st);
                if (rc == 0) {
                    result = json_object_new_object();
                    json_object_object_add(result, "id", json_object_new_int(st.id));
                    json_object_object_add(result, "enabled", json_object_new_boolean(st.enabled));
                    json_object_object_add(result, "timeout_ms", json_object_new_int(st.timeout_ms));
                    json_object_object_add(result, "respect_inhibitors", json_object_new_boolean(st.respect_inhibitors));
                    json_object_object_add(result, "is_idle", json_object_new_boolean(st.is_idle));
                }
            }
        }
    }
    else if (strcmp(method, "System.IdleMonitorGet") == 0) {
        if (!s->idle_mgr) { axctl_set_error("idle not supported"); rc = -1; }
        else {
            axctl_idle_monitor_state_t st;
            rc = axctl_idle_monitor_get(s->idle_mgr, (uint32_t)param_int(params, "id", 0), &st);
            if (rc == 0) {
                result = json_object_new_object();
                json_object_object_add(result, "id", json_object_new_int(st.id));
                json_object_object_add(result, "enabled", json_object_new_boolean(st.enabled));
                json_object_object_add(result, "timeout_ms", json_object_new_int(st.timeout_ms));
                json_object_object_add(result, "respect_inhibitors", json_object_new_boolean(st.respect_inhibitors));
                json_object_object_add(result, "is_idle", json_object_new_boolean(st.is_idle));
            }
        }
    }
    else if (strcmp(method, "System.IdleMonitorDestroy") == 0) {
        if (!s->idle_mgr) { axctl_set_error("idle not supported"); rc = -1; }
        else rc = axctl_idle_monitor_destroy(s->idle_mgr, (uint32_t)param_int(params, "id", 0));
    }
    else if (strcmp(method, "System.IdleInhibitorCreate") == 0) {
        if (!s->idle_mgr) { axctl_set_error("idle not supported"); rc = -1; }
        else {
            axctl_idle_inhibitor_state_t st;
            rc = axctl_idle_inhibitor_create(s->idle_mgr,
                param_bool(params, "enabled", 0), &st);
            if (rc == 0) {
                result = json_object_new_object();
                json_object_object_add(result, "id", json_object_new_int(st.id));
                json_object_object_add(result, "enabled", json_object_new_boolean(st.enabled));
            }
        }
    }
    else if (strcmp(method, "System.IdleInhibitorSet") == 0) {
        if (!s->idle_mgr) { axctl_set_error("idle not supported"); rc = -1; }
        else {
            axctl_idle_inhibitor_state_t st;
            rc = axctl_idle_inhibitor_set(s->idle_mgr,
                (uint32_t)param_int(params, "id", 0),
                param_bool(params, "enabled", 0), &st);
            if (rc == 0) {
                result = json_object_new_object();
                json_object_object_add(result, "id", json_object_new_int(st.id));
                json_object_object_add(result, "enabled", json_object_new_boolean(st.enabled));
            }
        }
    }
    else if (strcmp(method, "System.IdleInhibitorGet") == 0) {
        if (!s->idle_mgr) { axctl_set_error("idle not supported"); rc = -1; }
        else {
            axctl_idle_inhibitor_state_t st;
            rc = axctl_idle_inhibitor_get(s->idle_mgr,
                (uint32_t)param_int(params, "id", 0), &st);
            if (rc == 0) {
                result = json_object_new_object();
                json_object_object_add(result, "id", json_object_new_int(st.id));
                json_object_object_add(result, "enabled", json_object_new_boolean(st.enabled));
            }
        }
    }
    else if (strcmp(method, "System.IdleInhibitorDestroy") == 0) {
        if (!s->idle_mgr) { axctl_set_error("idle not supported"); rc = -1; }
        else rc = axctl_idle_inhibitor_destroy(s->idle_mgr, (uint32_t)param_int(params, "id", 0));
    }
    else if (strcmp(method, "System.InhibitSystem") == 0) {
        if (!s->idle_mgr) { axctl_set_error("idle not supported"); rc = -1; }
        else rc = axctl_idle_inhibit_system(s->idle_mgr, param_bool(params, "on", 0));
    }
    else if (strcmp(method, "System.IsSystemInhibited") == 0) {
        if (!s->idle_mgr) { axctl_set_error("idle not supported"); rc = -1; }
        else result = json_object_new_string(
            axctl_idle_is_system_inhibited(s->idle_mgr) ? "true" : "false");
    }
    else if (strcmp(method, "System.AppInhibitCheck") == 0) {
        if (!s->idle_mgr) { axctl_set_error("idle not supported"); rc = -1; }
        else {
            json_object *pats = NULL;
            json_object_object_get_ex(params, "patterns", &pats);
            int pcount = 0;
            char **parr = NULL;
            if (pats && json_object_is_type(pats, json_type_array)) {
                pcount = json_object_array_length(pats);
                parr = calloc(pcount, sizeof(char*));
                for (int i = 0; i < pcount; i++)
                    parr[i] = (char *)json_object_get_string(json_object_array_get_idx(pats, i));
            }
            rc = axctl_idle_app_check(s->idle_mgr, parr, pcount, &result);
            free(parr);
        }
    }
    else if (strcmp(method, "System.MediaInhibitCheck") == 0) {
        if (!s->idle_mgr) { axctl_set_error("idle not supported"); rc = -1; }
        else rc = axctl_idle_media_check(s->idle_mgr, &result);
    }
    else if (strcmp(method, "System.GetCapabilities") == 0) {
        axctl_capabilities_t caps = {0};
        rc = c->get_capabilities(c->priv, &caps);
        if (rc == 0) {
            result = json_object_new_object();
            json_object_object_add(result, "blur", json_object_new_boolean(caps.blur));
            json_object_object_add(result, "shadows", json_object_new_boolean(caps.shadows));
            json_object_object_add(result, "animations", json_object_new_boolean(caps.animations));
            json_object_object_add(result, "rounded_corners", json_object_new_boolean(caps.rounded_corners));
            json_object_object_add(result, "workspaces_supported", json_object_new_boolean(caps.workspaces_supported));
            json_object_object_add(result, "windows_supported", json_object_new_boolean(caps.windows_supported));
        }
    }
    else if (strcmp(method, "System.Subscribe") == 0) {
        debug_log("SUBSCRIBE: new subscriber fd=%d", client_fd);

        /* Add this client to the subscription list */
        pthread_mutex_lock(&s->clients_mu);
        if (s->client_count < MAX_CLIENTS) {
            s->client_fds[s->client_count++] = client_fd;
            debug_log("SUBSCRIBE: added client, total=%d", s->client_count);
        } else {
            debug_log("SUBSCRIBE: MAX_CLIENTS reached, rejecting fd=%d", client_fd);
        }
        pthread_mutex_unlock(&s->clients_mu);

        /* Build state dump once, use in both messages */
        json_object *initial_state = build_state_dump(s);

        /* 1. State.Dump notification (backward compat) */
        {
            json_object *notif = json_object_new_object();
            json_object_object_add(notif, "jsonrpc", json_object_new_string("2.0"));
            json_object_object_add(notif, "method", json_object_new_string("State.Dump"));
            json_object_object_add(notif, "state", json_object_get(initial_state));
            const char *data = json_object_to_json_string_ext(notif,
                                                               JSON_C_TO_STRING_PLAIN);
            debug_log("SUBSCRIBE: initial state dump %zu bytes", strlen(data));
            write(client_fd, data, strlen(data));
            write(client_fd, "\n", 1);
            json_object_put(notif);
        }

        /* 2. Subscribe response with state at top level.
         * The QML checks parsedJson.state — this ensures the state
         * is available even if the notification arrives mid-parse. */
        {
            json_object *resp = json_object_new_object();
            json_object_object_add(resp, "jsonrpc", json_object_new_string("2.0"));
            json_object_object_add(resp, "result", json_object_new_string("subscribed"));
            json_object_object_add(resp, "state", initial_state);
            const char *data = json_object_to_json_string_ext(resp,
                                                               JSON_C_TO_STRING_PLAIN);
            write(client_fd, data, strlen(data));
            write(client_fd, "\n", 1);
            json_object_put(resp);
        }

        /* Return a string result so the generic handler sends
         * {"id":1,"result":"subscribed"} too — redundant but harmless */
        result = json_object_new_string("subscribed");
    }
    else {
        axctl_set_error("method not found");
        rc = -1;
    }

    if (rc != 0 && !result) {
        /* Return error */
        result = json_object_new_string("error");
    }

    return result;
}

/* ------------------------------------------------------------------ */
/* Connection handler                                                  */
/* ------------------------------------------------------------------ */

static void handle_connection(axctl_server_t *s, int fd)
{
    char buf[READ_BUF_SIZE];
    struct json_tokener *tok = json_tokener_new();

    /* Send state dump immediately on connection.
     * This lets the QML client receive initial data before it even
     * sends System.Subscribe, eliminating the race window where
     * CompositorData properties are accessed before being populated. */
    {
        json_object *state = build_state_dump(s);
        json_object *greeting = json_object_new_object();
        json_object_object_add(greeting, "jsonrpc", json_object_new_string("2.0"));
        json_object_object_add(greeting, "method", json_object_new_string("State.Dump"));
        json_object_object_add(greeting, "state", state);
        const char *data = json_object_to_json_string_ext(greeting,
                                                           JSON_C_TO_STRING_PLAIN);
        write(fd, data, strlen(data));
        write(fd, "\n", 1);
        json_object_put(greeting);
    }

    while (1) {
        ssize_t n = read(fd, buf, sizeof(buf) - 1);
        if (n <= 0) break;
        buf[n] = 0;

        /* Parse JSON-RPC request(s) from the buffer */
        json_object *req_obj = json_tokener_parse_ex(tok, buf, (int)n);
        enum json_tokener_error jerr = json_tokener_get_error(tok);

        if (jerr == json_tokener_success && req_obj) {
            /* Extract method and params */
            json_object *j_id = NULL, *j_method = NULL, *j_params = NULL;
            json_object_object_get_ex(req_obj, "id", &j_id);
            json_object_object_get_ex(req_obj, "method", &j_method);
            json_object_object_get_ex(req_obj, "params", &j_params);

            const char *method = j_method ? json_object_get_string(j_method) : "";

            /* Dispatch */
            json_object *result = dispatch_method(s, method, j_params, fd);

            /* Build response */
            json_object *resp = json_object_new_object();
            if (j_id)
                json_object_object_add(resp, "id", json_object_get(j_id));

            const char *err = axctl_get_error();
            if (err && *err) {
                json_object_object_add(resp, "error",
                    json_object_new_string(err));
                axctl_clear_error();
            } else if (result) {
                json_object_object_add(resp, "result", result);
            } else {
                json_object_object_add(resp, "result",
                    json_object_new_string("ok"));
            }

            const char *resp_str = json_object_to_json_string_ext(resp,
                                                JSON_C_TO_STRING_PLAIN);
            write(fd, resp_str, strlen(resp_str));
            write(fd, "\n", 1);

            json_object_put(resp);
            json_object_put(req_obj);
            json_tokener_reset(tok);
        } else if (jerr != json_tokener_continue) {
            /* Parse error */
            json_tokener_reset(tok);
        }
    }

    /* Remove from subscribed clients */
    pthread_mutex_lock(&s->clients_mu);
    for (int i = 0; i < s->client_count; i++) {
        if (s->client_fds[i] == fd) {
            s->client_fds[i] = s->client_fds[--s->client_count];
            break;
        }
    }
    pthread_mutex_unlock(&s->clients_mu);

    json_tokener_free(tok);
    close(fd);
}

/* Connection handler thread */
static void *connection_thread(void *arg)
{
    void **args = (void **)arg;
    axctl_server_t *s = (axctl_server_t *)args[0];
    int fd = (int)(intptr_t)args[1];
    free(args);
    handle_connection(s, fd);
    return NULL;
}

/* ------------------------------------------------------------------ */
/* Server start (blocking)                                             */
/* ------------------------------------------------------------------ */

int axctl_server_start(axctl_server_t *s)
{
    unlink(s->socket_path);

    s->listen_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (s->listen_fd < 0) {
        LOG_ERROR("Failed to create socket: %s", strerror(errno));
        return -1;
    }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, s->socket_path, sizeof(addr.sun_path) - 1);

    if (bind(s->listen_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        LOG_ERROR("Failed to bind socket: %s", strerror(errno));
        close(s->listen_fd);
        return -1;
    }

    if (listen(s->listen_fd, 16) < 0) {
        LOG_ERROR("Failed to listen: %s", strerror(errno));
        close(s->listen_fd);
        return -1;
    }

    LOG_INFO("Server listening on %s", s->socket_path);

    while (1) {
        int fd = accept(s->listen_fd, NULL, NULL);
        if (fd < 0) {
            if (errno == EINTR) continue;
            LOG_ERROR("Accept error: %s", strerror(errno));
            continue;
        }

        /* Spawn thread for each connection */
        void **args = malloc(sizeof(void *) * 2);
        args[0] = s;
        args[1] = (void *)(intptr_t)fd;

        pthread_t tid;
        pthread_create(&tid, NULL, connection_thread, args);
        pthread_detach(tid);
    }

    return 0;
}

void axctl_server_destroy(axctl_server_t *s)
{
    if (!s) return;
    if (s->listen_fd >= 0) close(s->listen_fd);
    if (s->socket_path) {
        unlink(s->socket_path);
        free(s->socket_path);
    }
    if (s->idle_mgr) axctl_idle_manager_destroy(s->idle_mgr);
    if (s->cache) axctl_cache_free(s->cache);
    pthread_mutex_destroy(&s->clients_mu);
    free(s);
}
