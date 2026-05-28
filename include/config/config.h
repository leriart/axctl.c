/*
 * axctl - TOML configuration loader and watcher
 *
 * Loads TOML configuration files with:
 *   - Recursive include resolution with glob patterns
 *   - Circular import detection (maxImportDepth=10)
 *   - Config merging (includes loaded first, main file overrides)
 *   - Hot-reload via inotify with debounce
 */
#ifndef AXCTL_CONFIG_H
#define AXCTL_CONFIG_H

#include "ipc/compositor.h"
#include "ipc/config_types.h"
#include <json-c/json.h>

/* Opaque config types */
typedef struct axctl_toml_config axctl_toml_config_t;
typedef struct axctl_config_watcher axctl_config_watcher_t;

/* Config reload callback */
typedef void (*axctl_config_reload_cb_t)(axctl_toml_config_t *new_cfg, void *userdata);

/* Get the default config path (~/.config/axctl/config.toml) */
char *axctl_default_config_path(void);

/* Load and merge a TOML configuration file. Caller must free with axctl_free_config. */
axctl_toml_config_t *axctl_load_config(const char *path);

/* Free a loaded TOML config */
void axctl_free_config(axctl_toml_config_t *cfg);

/* Convert TOML config to JSON string. Caller must free result. */
char *axctl_config_to_json(axctl_toml_config_t *cfg);

/* Apply the config to a compositor (generate + reload) */
int axctl_apply_config(axctl_toml_config_t *cfg, axctl_compositor_t *comp);

/* Watcher */
axctl_config_watcher_t *axctl_config_watcher_create(void);
int axctl_config_watcher_start(axctl_config_watcher_t *w, const char *path,
                                axctl_config_reload_cb_t callback, void *userdata);
void axctl_config_watcher_stop(axctl_config_watcher_t *w);

#endif /* AXCTL_CONFIG_H */
