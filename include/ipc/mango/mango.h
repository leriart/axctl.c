/*
 * axctl - Mango compositor backend
 *
 * Communicates with Mango via Wayland protocols:
 *   - zdwl_ipc_manager_v2 for workspace/layout control
 *   - zwlr_foreign_toplevel_manager_v1 for window tracking
 *   - wl_output for monitor information
 */
#ifndef AXCTL_IPC_MANGO_H
#define AXCTL_IPC_MANGO_H

#include "ipc/compositor.h"
#include "ipc/config_types.h"

/* Create a new Mango compositor backend. Returns NULL on failure. */
axctl_compositor_t *mango_compositor_create(void);

/* Config generators for Mango (stub - uses IPC dispatch) */
char *mango_generate_appearance(const axctl_appearance_t *cfg);
char *mango_generate_keybinds(const axctl_keybind_t *binds, int count);
char *mango_generate_window_rules(const axctl_window_rule_t *rules, int count);
char *mango_generate_layer_rules(const axctl_layer_rule_t *rules, int count);
char *mango_generate_startup(char **exec, int exec_count,
                             char **exec_once, int exec_once_count);

#endif /* AXCTL_IPC_MANGO_H */
