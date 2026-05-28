/*
 * axctl - Thread-safe state cache implementation
 */
#include "ipc/cache.h"
#include "utils/strutil.h"
#include <stdlib.h>
#include <string.h>

axctl_state_cache_t *axctl_cache_new(void) {
    axctl_state_cache_t *c = calloc(1, sizeof(axctl_state_cache_t));
    if (!c) return NULL;
    pthread_rwlock_init(&c->lock, NULL);
    axctl_window_array_init(&c->windows);
    axctl_workspace_array_init(&c->workspaces);
    axctl_monitor_array_init(&c->monitors);
    return c;
}

void axctl_cache_free(axctl_state_cache_t *c) {
    if (!c) return;
    axctl_window_array_free(&c->windows);
    axctl_workspace_array_free(&c->workspaces);
    axctl_monitor_array_free(&c->monitors);
    pthread_rwlock_destroy(&c->lock);
    free(c);
}

/* Helper: deep copy an array for returning to caller */
static axctl_window_array_t dup_windows(const axctl_window_array_t *src) {
    axctl_window_array_t dst;
    axctl_window_array_init(&dst);
    for (size_t i = 0; i < src->count; i++) {
        axctl_window_array_push(&dst, axctl_window_dup(&src->items[i]));
    }
    return dst;
}

static axctl_workspace_t workspace_dup(const axctl_workspace_t *src) {
    axctl_workspace_t w = {0};
    w.id = axctl_strdup(src->id);
    w.name = axctl_strdup(src->name);
    w.monitor_id = axctl_strdup(src->monitor_id);
    w.is_active = src->is_active;
    w.is_empty = src->is_empty;
    if (src->metadata) w.metadata = json_object_get(src->metadata);
    return w;
}

static axctl_workspace_array_t dup_workspaces(const axctl_workspace_array_t *src) {
    axctl_workspace_array_t dst;
    axctl_workspace_array_init(&dst);
    for (size_t i = 0; i < src->count; i++) {
        axctl_workspace_array_push(&dst, workspace_dup(&src->items[i]));
    }
    return dst;
}

static axctl_monitor_t monitor_dup(const axctl_monitor_t *src) {
    axctl_monitor_t m = {0};
    m.id = axctl_strdup(src->id);
    m.name = axctl_strdup(src->name);
    m.description = axctl_strdup(src->description);
    m.width = src->width;
    m.height = src->height;
    m.refresh_rate = src->refresh_rate;
    m.scale = src->scale;
    m.is_focused = src->is_focused;
    if (src->metadata) m.metadata = json_object_get(src->metadata);
    return m;
}

static axctl_monitor_array_t dup_monitors(const axctl_monitor_array_t *src) {
    axctl_monitor_array_t dst;
    axctl_monitor_array_init(&dst);
    for (size_t i = 0; i < src->count; i++) {
        axctl_monitor_array_push(&dst, monitor_dup(&src->items[i]));
    }
    return dst;
}

/* ── Window operations ──────────────────────────────────────────────── */
void axctl_cache_add_window(axctl_state_cache_t *c, axctl_window_t w) {
    pthread_rwlock_wrlock(&c->lock);
    /* Deep copy: the caller owns the original window and may free it.
     * axctl_window_array_push does a shallow copy of the struct, so we
     * must dup the contents before the original is destroyed. */
    axctl_window_t copy = axctl_window_dup(&w);
    axctl_window_array_push(&c->windows, copy);
    pthread_rwlock_unlock(&c->lock);
}

void axctl_cache_remove_window(axctl_state_cache_t *c, const char *id) {
    pthread_rwlock_wrlock(&c->lock);
    for (size_t i = 0; i < c->windows.count; i++) {
        if (c->windows.items[i].id && strcmp(c->windows.items[i].id, id) == 0) {
            axctl_window_free(&c->windows.items[i]);
            /* Shift remaining items */
            memmove(&c->windows.items[i], &c->windows.items[i+1],
                    (c->windows.count - i - 1) * sizeof(axctl_window_t));
            c->windows.count--;
            break;
        }
    }
    pthread_rwlock_unlock(&c->lock);
}

void axctl_cache_update_window_title(axctl_state_cache_t *c, const char *id, const char *title) {
    pthread_rwlock_wrlock(&c->lock);
    for (size_t i = 0; i < c->windows.count; i++) {
        if (c->windows.items[i].id && strcmp(c->windows.items[i].id, id) == 0) {
            free(c->windows.items[i].title);
            c->windows.items[i].title = axctl_strdup(title);
            break;
        }
    }
    pthread_rwlock_unlock(&c->lock);
}

void axctl_cache_update_window_workspace(axctl_state_cache_t *c, const char *id,
                                          const char *workspace_id, const char *monitor_id) {
    pthread_rwlock_wrlock(&c->lock);
    for (size_t i = 0; i < c->windows.count; i++) {
        if (c->windows.items[i].id && strcmp(c->windows.items[i].id, id) == 0) {
            free(c->windows.items[i].workspace_id);
            c->windows.items[i].workspace_id = axctl_strdup(workspace_id);
            if (monitor_id && *monitor_id) {
                if (!c->windows.items[i].metadata) {
                    c->windows.items[i].metadata = json_object_new_object();
                }
                json_object_object_add(c->windows.items[i].metadata, "monitor_id",
                                       json_object_new_string(monitor_id));
            }
            break;
        }
    }
    pthread_rwlock_unlock(&c->lock);
}

void axctl_cache_update_window_state(axctl_state_cache_t *c, const char *id, bool is_fullscreen) {
    pthread_rwlock_wrlock(&c->lock);
    for (size_t i = 0; i < c->windows.count; i++) {
        if (c->windows.items[i].id && strcmp(c->windows.items[i].id, id) == 0) {
            c->windows.items[i].is_fullscreen = is_fullscreen;
            break;
        }
    }
    pthread_rwlock_unlock(&c->lock);
}

void axctl_cache_update_window_floating(axctl_state_cache_t *c, const char *id, bool floating) {
    pthread_rwlock_wrlock(&c->lock);
    for (size_t i = 0; i < c->windows.count; i++) {
        if (c->windows.items[i].id && strcmp(c->windows.items[i].id, id) == 0) {
            c->windows.items[i].is_floating = floating;
            break;
        }
    }
    pthread_rwlock_unlock(&c->lock);
}

void axctl_cache_mark_window_focused(axctl_state_cache_t *c, const char *id) {
    pthread_rwlock_wrlock(&c->lock);
    for (size_t i = 0; i < c->windows.count; i++) {
        c->windows.items[i].is_focused = (c->windows.items[i].id && id &&
                                            strcmp(c->windows.items[i].id, id) == 0);
    }
    pthread_rwlock_unlock(&c->lock);
}

void axctl_cache_set_windows(axctl_state_cache_t *c, axctl_window_array_t *arr) {
    pthread_rwlock_wrlock(&c->lock);
    axctl_window_array_free(&c->windows);
    c->windows = *arr;
    /* Zero out the source so caller doesn't double-free */
    axctl_window_array_init(arr);
    pthread_rwlock_unlock(&c->lock);
}

axctl_window_array_t axctl_cache_get_windows(axctl_state_cache_t *c) {
    pthread_rwlock_rdlock(&c->lock);
    axctl_window_array_t copy = dup_windows(&c->windows);
    pthread_rwlock_unlock(&c->lock);
    return copy;
}

void axctl_cache_set_workspaces(axctl_state_cache_t *c, axctl_workspace_array_t *arr) {
    pthread_rwlock_wrlock(&c->lock);
    axctl_workspace_array_free(&c->workspaces);
    c->workspaces = *arr;
    axctl_workspace_array_init(arr);
    pthread_rwlock_unlock(&c->lock);
}

axctl_workspace_array_t axctl_cache_get_workspaces(axctl_state_cache_t *c) {
    pthread_rwlock_rdlock(&c->lock);
    axctl_workspace_array_t copy = dup_workspaces(&c->workspaces);
    pthread_rwlock_unlock(&c->lock);
    return copy;
}

void axctl_cache_set_monitors(axctl_state_cache_t *c, axctl_monitor_array_t *arr) {
    pthread_rwlock_wrlock(&c->lock);
    axctl_monitor_array_free(&c->monitors);
    c->monitors = *arr;
    axctl_monitor_array_init(arr);
    pthread_rwlock_unlock(&c->lock);
}

axctl_monitor_array_t axctl_cache_get_monitors(axctl_state_cache_t *c) {
    pthread_rwlock_rdlock(&c->lock);
    axctl_monitor_array_t copy = dup_monitors(&c->monitors);
    pthread_rwlock_unlock(&c->lock);
    return copy;
}
