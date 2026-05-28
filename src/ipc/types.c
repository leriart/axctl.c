/*
 * axctl - IPC type implementations
 */
#include "ipc/types.h"
#include "utils/strutil.h"
#include <stdlib.h>
#include <string.h>

/* ── Event type names ───────────────────────────────────────────────── */
static const char *event_type_names[] = {
    "window_created",
    "window_closed",
    "window_focused",
    "window_title_changed",
    "window_moved",
    "workspace_changed",
    "monitor_changed",
    "config_reloaded",
    "fullscreen_changed",
    "focused_monitor_changed",
    "floating_changed",
    "unknown"
};

const char *axctl_event_type_str(axctl_event_type_t type) {
    if (type >= 0 && type <= EVENT_UNKNOWN) return event_type_names[type];
    return "unknown";
}

/* ── Dynamic array operations ───────────────────────────────────────── */
#define ARRAY_INIT_CAP 8

void axctl_window_array_init(axctl_window_array_t *arr) {
    arr->items = NULL;
    arr->count = 0;
    arr->capacity = 0;
}

void axctl_window_array_push(axctl_window_array_t *arr, axctl_window_t w) {
    if (arr->count >= arr->capacity) {
        size_t new_cap = arr->capacity ? arr->capacity * 2 : ARRAY_INIT_CAP;
        axctl_window_t *new_items = realloc(arr->items, new_cap * sizeof(axctl_window_t));
        if (!new_items) return;
        arr->items = new_items;
        arr->capacity = new_cap;
    }
    arr->items[arr->count++] = w;
}

void axctl_window_array_free(axctl_window_array_t *arr) {
    for (size_t i = 0; i < arr->count; i++) {
        axctl_window_free(&arr->items[i]);
    }
    free(arr->items);
    arr->items = NULL;
    arr->count = 0;
    arr->capacity = 0;
}

void axctl_workspace_array_init(axctl_workspace_array_t *arr) {
    arr->items = NULL; arr->count = 0; arr->capacity = 0;
}

void axctl_workspace_array_push(axctl_workspace_array_t *arr, axctl_workspace_t w) {
    if (arr->count >= arr->capacity) {
        size_t new_cap = arr->capacity ? arr->capacity * 2 : ARRAY_INIT_CAP;
        axctl_workspace_t *new_items = realloc(arr->items, new_cap * sizeof(axctl_workspace_t));
        if (!new_items) return;
        arr->items = new_items;
        arr->capacity = new_cap;
    }
    arr->items[arr->count++] = w;
}

void axctl_workspace_array_free(axctl_workspace_array_t *arr) {
    for (size_t i = 0; i < arr->count; i++) axctl_workspace_free(&arr->items[i]);
    free(arr->items);
    arr->items = NULL; arr->count = 0; arr->capacity = 0;
}

void axctl_monitor_array_init(axctl_monitor_array_t *arr) {
    arr->items = NULL; arr->count = 0; arr->capacity = 0;
}

void axctl_monitor_array_push(axctl_monitor_array_t *arr, axctl_monitor_t m) {
    if (arr->count >= arr->capacity) {
        size_t new_cap = arr->capacity ? arr->capacity * 2 : ARRAY_INIT_CAP;
        axctl_monitor_t *new_items = realloc(arr->items, new_cap * sizeof(axctl_monitor_t));
        if (!new_items) return;
        arr->items = new_items;
        arr->capacity = new_cap;
    }
    arr->items[arr->count++] = m;
}

void axctl_monitor_array_free(axctl_monitor_array_t *arr) {
    for (size_t i = 0; i < arr->count; i++) axctl_monitor_free(&arr->items[i]);
    free(arr->items);
    arr->items = NULL; arr->count = 0; arr->capacity = 0;
}

/* ── Deep copy ──────────────────────────────────────────────────────── */
axctl_window_t axctl_window_dup(const axctl_window_t *src) {
    axctl_window_t w = {0};
    if (!src) return w;
    w.id = axctl_strdup(src->id);
    w.title = axctl_strdup(src->title);
    w.app_id = axctl_strdup(src->app_id);
    w.workspace_id = axctl_strdup(src->workspace_id);
    w.is_focused = src->is_focused;
    w.is_floating = src->is_floating;
    w.is_fullscreen = src->is_fullscreen;
    w.is_hidden = src->is_hidden;
    if (src->metadata) {
        w.metadata = json_object_get(src->metadata);
    }
    return w;
}

/* ── Free functions ─────────────────────────────────────────────────── */
void axctl_window_free(axctl_window_t *w) {
    if (!w) return;
    free(w->id); free(w->title); free(w->app_id); free(w->workspace_id);
    if (w->metadata) json_object_put(w->metadata);
    memset(w, 0, sizeof(*w));
}

void axctl_workspace_free(axctl_workspace_t *w) {
    if (!w) return;
    free(w->id); free(w->name); free(w->monitor_id);
    if (w->metadata) json_object_put(w->metadata);
    memset(w, 0, sizeof(*w));
}

void axctl_monitor_free(axctl_monitor_t *m) {
    if (!m) return;
    free(m->id); free(m->name); free(m->description);
    if (m->metadata) json_object_put(m->metadata);
    memset(m, 0, sizeof(*m));
}

void axctl_event_free(axctl_event_t *e) {
    if (!e) return;
    if (e->window) { axctl_window_free(e->window); free(e->window); }
    if (e->workspace) { axctl_workspace_free(e->workspace); free(e->workspace); }
    if (e->payload) json_object_put(e->payload);
    memset(e, 0, sizeof(*e));
}

/* ── JSON serialisation ─────────────────────────────────────────────── */
struct json_object *axctl_window_to_json(const axctl_window_t *w) {
    struct json_object *obj = json_object_new_object();
    json_object_object_add(obj, "id", json_object_new_string(w->id ? w->id : ""));
    json_object_object_add(obj, "title", json_object_new_string(w->title ? w->title : ""));
    json_object_object_add(obj, "app_id", json_object_new_string(w->app_id ? w->app_id : ""));
    json_object_object_add(obj, "workspace_id", json_object_new_string(w->workspace_id ? w->workspace_id : ""));
    json_object_object_add(obj, "is_focused", json_object_new_boolean(w->is_focused));
    json_object_object_add(obj, "is_floating", json_object_new_boolean(w->is_floating));
    json_object_object_add(obj, "is_fullscreen", json_object_new_boolean(w->is_fullscreen));
    json_object_object_add(obj, "is_hidden", json_object_new_boolean(w->is_hidden));

    /* Always include metadata — QML clients expect it even if empty */
    if (w->metadata) {
        json_object_object_add(obj, "metadata", json_object_get(w->metadata));
    } else {
        json_object_object_add(obj, "metadata", json_object_new_object());
    }

    return obj;
}

struct json_object *axctl_workspace_to_json(const axctl_workspace_t *w) {
    struct json_object *obj = json_object_new_object();
    json_object_object_add(obj, "id", json_object_new_string(w->id ? w->id : ""));
    json_object_object_add(obj, "name", json_object_new_string(w->name ? w->name : ""));
    json_object_object_add(obj, "monitor_id", json_object_new_string(w->monitor_id ? w->monitor_id : ""));
    json_object_object_add(obj, "is_active", json_object_new_boolean(w->is_active));
    json_object_object_add(obj, "is_empty", json_object_new_boolean(w->is_empty));

    /* Always include metadata with at least a "focused" field.
     * The Go original always serialises metadata.focused; QML clients
     * (Ambxst) crash with TypeError if the field is missing. */
    if (w->metadata) {
        /* If the backend set metadata but forgot "focused", add it */
        struct json_object *meta = json_object_get(w->metadata);
        json_object *existing = NULL;
        if (!json_object_object_get_ex(meta, "focused", &existing)) {
            json_object_object_add(meta, "focused",
                                   json_object_new_boolean(w->is_active));
        }
        json_object_object_add(obj, "metadata", meta);
    } else {
        /* No metadata at all — create minimal {focused: is_active} */
        struct json_object *meta = json_object_new_object();
        json_object_object_add(meta, "focused",
                               json_object_new_boolean(w->is_active));
        json_object_object_add(obj, "metadata", meta);
    }

    return obj;
}

struct json_object *axctl_monitor_to_json(const axctl_monitor_t *m) {
    struct json_object *obj = json_object_new_object();
    json_object_object_add(obj, "id", json_object_new_string(m->id ? m->id : ""));
    json_object_object_add(obj, "name", json_object_new_string(m->name ? m->name : ""));
    if (m->description)
        json_object_object_add(obj, "description", json_object_new_string(m->description));
    json_object_object_add(obj, "width", json_object_new_int(m->width));
    json_object_object_add(obj, "height", json_object_new_int(m->height));
    json_object_object_add(obj, "refresh_rate", json_object_new_double(m->refresh_rate));
    json_object_object_add(obj, "scale", json_object_new_double(m->scale));
    json_object_object_add(obj, "is_focused", json_object_new_boolean(m->is_focused));

    /* Always include metadata — QML clients expect it even if empty */
    if (m->metadata) {
        json_object_object_add(obj, "metadata", json_object_get(m->metadata));
    } else {
        json_object_object_add(obj, "metadata", json_object_new_object());
    }

    return obj;
}

struct json_object *axctl_capabilities_to_json(const axctl_capabilities_t *c) {
    struct json_object *obj = json_object_new_object();
    json_object_object_add(obj, "blur", json_object_new_boolean(c->blur));
    json_object_object_add(obj, "shadows", json_object_new_boolean(c->shadows));
    json_object_object_add(obj, "animations", json_object_new_boolean(c->animations));
    json_object_object_add(obj, "rounded_corners", json_object_new_boolean(c->rounded_corners));
    json_object_object_add(obj, "workspaces_supported", json_object_new_boolean(c->workspaces_supported));
    json_object_object_add(obj, "windows_supported", json_object_new_boolean(c->windows_supported));
    return obj;
}

struct json_object *axctl_window_array_to_json(const axctl_window_array_t *arr) {
    struct json_object *ja = json_object_new_array();
    for (size_t i = 0; i < arr->count; i++)
        json_object_array_add(ja, axctl_window_to_json(&arr->items[i]));
    return ja;
}

struct json_object *axctl_workspace_array_to_json(const axctl_workspace_array_t *arr) {
    struct json_object *ja = json_object_new_array();
    for (size_t i = 0; i < arr->count; i++)
        json_object_array_add(ja, axctl_workspace_to_json(&arr->items[i]));
    return ja;
}

struct json_object *axctl_monitor_array_to_json(const axctl_monitor_array_t *arr) {
    struct json_object *ja = json_object_new_array();
    for (size_t i = 0; i < arr->count; i++)
        json_object_array_add(ja, axctl_monitor_to_json(&arr->items[i]));
    return ja;
}
