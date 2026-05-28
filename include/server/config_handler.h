/*
 * axctl - Config handler
 *
 * Converts universal configuration to compositor-specific syntax,
 * writes config files, and triggers compositor reload.
 */
#ifndef AXCTL_SERVER_CONFIG_HANDLER_H
#define AXCTL_SERVER_CONFIG_HANDLER_H

#include "ipc/compositor.h"
#include "ipc/config_types.h"

/* Apply a universal config to a compositor (generate files and reload) */
int axctl_config_handler_apply(axctl_compositor_t *comp,
                                const axctl_config_universal_t *cfg,
                                const char *output_path);

/* Get the default output path */
char *axctl_config_default_output_path(void);

#endif /* AXCTL_SERVER_CONFIG_HANDLER_H */
