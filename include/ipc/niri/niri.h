/*
 * axctl - Niri compositor backend
 *
 * Communicates with Niri via JSON-RPC over Unix socket (NIRI_SOCKET).
 */
#ifndef AXCTL_IPC_NIRI_H
#define AXCTL_IPC_NIRI_H

#include "ipc/compositor.h"
#include "ipc/config_types.h"

/* Create a new Niri compositor backend. Returns NULL on failure. */
axctl_compositor_t *niri_compositor_create(void);

/* Config generators for Niri (KDL syntax) */
char *niri_generate_appearance(const axctl_appearance_t *cfg);
char *niri_generate_keybinds(const axctl_keybind_t *binds, int count);
char *niri_generate_window_rules(const axctl_window_rule_t *rules, int count);
char *niri_generate_layer_rules(const axctl_layer_rule_t *rules, int count);
char *niri_generate_startup(char **exec, int exec_count,
                            char **exec_once, int exec_once_count);

#endif /* AXCTL_IPC_NIRI_H */
