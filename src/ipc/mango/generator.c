/*
 * axctl - Mango config generator (stub)
 *
 * Mango uses IPC-based config dispatch rather than file-based configuration.
 * These generators return empty strings since Mango applies config changes
 * directly via the compositor IPC protocol.
 */

#include "ipc/mango/mango.h"
#include "ipc/config_types.h"
#include "utils/strutil.h"
#include <stdlib.h>

char *mango_generate_appearance(const axctl_appearance_t *cfg)
{
    (void)cfg;
    return axctl_strdup("");
}

char *mango_generate_keybinds(const axctl_keybind_t *binds, int count)
{
    (void)binds; (void)count;
    return axctl_strdup("");
}

char *mango_generate_window_rules(const axctl_window_rule_t *rules, int count)
{
    (void)rules; (void)count;
    return axctl_strdup("");
}

char *mango_generate_layer_rules(const axctl_layer_rule_t *rules, int count)
{
    (void)rules; (void)count;
    return axctl_strdup("");
}

char *mango_generate_startup(char **exec, int exec_count,
                             char **exec_once, int exec_once_count)
{
    (void)exec; (void)exec_count;
    (void)exec_once; (void)exec_once_count;
    return axctl_strdup("");
}
