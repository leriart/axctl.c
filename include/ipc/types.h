/*
 * axctl - Universal IPC types
 *
 * These structures represent compositor-agnostic data types used across
 * all compositor backends (Hyprland, Niri, Mango).
 */
#ifndef AXCTL_IPC_TYPES_H
#define AXCTL_IPC_TYPES_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <json-c/json.h>

/* ── Window ─────────────────────────────────────────────────────────── */
typedef struct {
    char *id;
    char *title;
    char *app_id;
    char *workspace_id;
    bool is_focused;
    bool is_floating;
    bool is_fullscreen;
    bool is_hidden;
    struct json_object *metadata;   /* json object, may be NULL */
} axctl_window_t;

/* ── Workspace ──────────────────────────────────────────────────────── */
typedef struct {
    char *id;
    char *name;
    char *monitor_id;
    bool is_active;
    bool is_empty;
    struct json_object *metadata;
} axctl_workspace_t;

/* ── Monitor ────────────────────────────────────────────────────────── */
typedef struct {
    char *id;
    char *name;
    char *description;
    int width;
    int height;
    double refresh_rate;
    double scale;
    bool is_focused;
    struct json_object *metadata;
} axctl_monitor_t;

/* ── Capabilities ───────────────────────────────────────────────────── */
typedef struct {
    int blur;
    int shadows;
    int animations;
    int rounded_corners;
    int workspaces_supported;
    int windows_supported;
} axctl_capabilities_t;

/* ── Event Types ────────────────────────────────────────────────────── */
typedef enum {
    EVENT_WINDOW_CREATED = 0,
    EVENT_WINDOW_CLOSED,
    EVENT_WINDOW_FOCUSED,
    EVENT_WINDOW_TITLE_CHANGED,
    EVENT_WINDOW_MOVED,
    EVENT_WORKSPACE_CHANGED,
    EVENT_MONITOR_CHANGED,
    EVENT_CONFIG_RELOADED,
    EVENT_FULLSCREEN_CHANGED,
    EVENT_FOCUSED_MONITOR_CHANGED,
    EVENT_FLOATING_CHANGED,
    EVENT_UNKNOWN
} axctl_event_type_t;

/* Get the string name for an event type */
const char *axctl_event_type_str(axctl_event_type_t type);

/* ── Event ──────────────────────────────────────────────────────────── */
typedef struct {
    axctl_event_type_t type;
    int64_t timestamp;
    axctl_window_t *window;         /* may be NULL */
    axctl_workspace_t *workspace;   /* may be NULL */
    struct json_object *payload;    /* additional event data, may be NULL */
} axctl_event_t;

/* ── Dynamic arrays ─────────────────────────────────────────────────── */
typedef struct {
    axctl_window_t *items;
    size_t count;
    size_t capacity;
} axctl_window_array_t;

typedef struct {
    axctl_workspace_t *items;
    size_t count;
    size_t capacity;
} axctl_workspace_array_t;

typedef struct {
    axctl_monitor_t *items;
    size_t count;
    size_t capacity;
} axctl_monitor_array_t;

/* Array operations */
void axctl_window_array_init(axctl_window_array_t *arr);
void axctl_window_array_push(axctl_window_array_t *arr, axctl_window_t w);
void axctl_window_array_free(axctl_window_array_t *arr);

void axctl_workspace_array_init(axctl_workspace_array_t *arr);
void axctl_workspace_array_push(axctl_workspace_array_t *arr, axctl_workspace_t w);
void axctl_workspace_array_free(axctl_workspace_array_t *arr);

void axctl_monitor_array_init(axctl_monitor_array_t *arr);
void axctl_monitor_array_push(axctl_monitor_array_t *arr, axctl_monitor_t m);
void axctl_monitor_array_free(axctl_monitor_array_t *arr);

/* Deep-copy and free helpers */
axctl_window_t axctl_window_dup(const axctl_window_t *src);
void axctl_window_free(axctl_window_t *w);
void axctl_workspace_free(axctl_workspace_t *w);
void axctl_monitor_free(axctl_monitor_t *m);
void axctl_event_free(axctl_event_t *e);

/* JSON serialisation */
struct json_object *axctl_window_to_json(const axctl_window_t *w);
struct json_object *axctl_workspace_to_json(const axctl_workspace_t *w);
struct json_object *axctl_monitor_to_json(const axctl_monitor_t *m);
struct json_object *axctl_capabilities_to_json(const axctl_capabilities_t *c);
struct json_object *axctl_window_array_to_json(const axctl_window_array_t *arr);
struct json_object *axctl_workspace_array_to_json(const axctl_workspace_array_t *arr);
struct json_object *axctl_monitor_array_to_json(const axctl_monitor_array_t *arr);

#endif /* AXCTL_IPC_TYPES_H */
