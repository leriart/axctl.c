/* ipc/compositor.h -- Compositor interface (vtable)
 *
 * Core abstraction layer enabling axctl to work with multiple Wayland compositors.
 * Each backend implements all function pointers in axctl_compositor_t.
 */
#ifndef AXCTL_IPC_COMPOSITOR_H
#define AXCTL_IPC_COMPOSITOR_H

#include "ipc/types.h"
#include <json-c/json.h>

/* Event callback type */
typedef void (*axctl_event_callback_t)(const axctl_event_t *event, void *userdata);

/* Compositor instance with vtable function pointers and private data. */
typedef struct axctl_compositor {
    const char *name;
    void *priv;  /* Backend-specific private data */

    /* Lifecycle */
    void (*destroy)(void *priv);

    /* Window operations */
    int (*list_windows)(void *priv, axctl_window_array_t *out);
    int (*active_window)(void *priv, char **out_id);
    int (*focus_window)(void *priv, const char *id);
    int (*focus_dir)(void *priv, const char *direction);
    int (*close_window)(void *priv, const char *id);
    int (*move_window)(void *priv, const char *id, const char *direction);
    int (*resize_window)(void *priv, const char *id, int width, int height);
    int (*toggle_floating)(void *priv, const char *id);
    int (*set_fullscreen)(void *priv, const char *id, int state);
    int (*set_maximized)(void *priv, const char *id, int state);
    int (*pin_window)(void *priv, const char *id, int state);
    int (*toggle_group)(void *priv, const char *id);
    int (*group_nav)(void *priv, const char *direction);
    int (*set_layout_property)(void *priv, const char *id,
                               const char *key, const char *value);
    int (*move_window_pixel)(void *priv, const char *id, int x, int y);

    /* Workspace operations */
    int (*list_workspaces)(void *priv, axctl_workspace_array_t *out);
    int (*active_workspace)(void *priv, axctl_workspace_t *out);
    int (*switch_workspace)(void *priv, const char *id);
    int (*move_to_workspace)(void *priv, const char *win_id, const char *ws_id);
    int (*move_to_workspace_silent)(void *priv, const char *win_id, const char *ws_id);
    int (*toggle_special_workspace)(void *priv, const char *name);

    /* Monitor operations */
    int (*list_monitors)(void *priv, axctl_monitor_array_t *out);
    int (*focus_monitor)(void *priv, const char *id);
    int (*move_to_monitor)(void *priv, const char *win_id, const char *mon_id);
    int (*set_dpms)(void *priv, const char *mon_id, int on);

    /* Layout */
    int (*set_layout)(void *priv, const char *name);

    /* Config operations */
    int (*get_config)(void *priv, const char *key, json_object **out);
    int (*set_config)(void *priv, const char *key, const char *value);
    int (*batch_config)(void *priv, json_object *configs);
    int (*batch_keybinds)(void *priv, const char *json_payload);
    int (*raw_batch)(void *priv, const char *command);
    int (*reload_config)(void *priv);
    int (*get_animations)(void *priv, json_object **out);
    int (*get_cursor_position)(void *priv, int *x, int *y);
    int (*bind_key)(void *priv, const char *mods, const char *key, const char *cmd);
    int (*unbind_key)(void *priv, const char *mods, const char *key);

    /* System operations */
    int (*execute)(void *priv, const char *command);
    int (*compositor_exit)(void *priv);
    int (*switch_keyboard_layout)(void *priv, const char *action);
    int (*set_keyboard_layouts)(void *priv, const char *layouts, const char *variants);

    /* Event subscription */
    int (*subscribe)(void *priv, axctl_event_callback_t cb, void *userdata);

    /* Capabilities */
    int (*get_capabilities)(void *priv, axctl_capabilities_t *out);
} axctl_compositor_t;

/* Auto-detect and create the active compositor */
axctl_compositor_t *axctl_detect_compositor(void);

/* Free a compositor */
void axctl_compositor_destroy(axctl_compositor_t *c);

#endif /* AXCTL_IPC_COMPOSITOR_H */
