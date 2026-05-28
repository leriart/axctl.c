/*
 * axctl - Hyprland compositor backend
 *
 * Communicates with Hyprland via Unix sockets:
 *   .socket.sock  - command/query socket
 *   .socket2.sock - event stream socket
 *
 * Supports both legacy and Lua dispatcher syntax (Hyprland >= 0.55).
 */
#ifndef AXCTL_IPC_HYPRLAND_H
#define AXCTL_IPC_HYPRLAND_H

#include "ipc/compositor.h"
#include "ipc/config_types.h"

/* Create a new Hyprland compositor backend. Returns NULL on failure. */
axctl_compositor_t *hyprland_compositor_create(void);

/* Config generators for Hyprland (hyprlang syntax) */
char *hyprland_generate_appearance(const axctl_appearance_t *cfg);
char *hyprland_generate_keybinds(const axctl_keybind_t *binds, int count);
char *hyprland_generate_window_rules(const axctl_window_rule_t *rules, int count);
char *hyprland_generate_layer_rules(const axctl_layer_rule_t *rules, int count);
char *hyprland_generate_startup(char **exec, int exec_count,
                                char **exec_once, int exec_once_count);

/* Lua config generators for Hyprland */
char *hyprland_generate_appearance_lua(const axctl_appearance_t *cfg);
char *hyprland_generate_keybinds_lua(const axctl_keybind_t *binds, int count);
char *hyprland_generate_window_rules_lua(const axctl_window_rule_t *rules, int count);
char *hyprland_generate_layer_rules_lua(const axctl_layer_rule_t *rules, int count);
char *hyprland_generate_startup_lua(char **exec, int exec_count,
                                    char **exec_once, int exec_once_count);

#endif /* AXCTL_IPC_HYPRLAND_H */
