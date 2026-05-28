/* src/ipc/compositor.c -- Compositor auto-detection
 *
 * Tries to connect to compositors in order:
 *   1. Hyprland (checks HYPRLAND_INSTANCE_SIGNATURE env)
 *   2. Niri     (checks NIRI_SOCKET env)
 *   3. Mango    (fallback: any Wayland display with zdwl_ipc_manager_v2)
 *
 * Returns the first compositor that successfully connects.
 */

#include <stdio.h>
#include <stdlib.h>

#include "ipc/compositor.h"
#include "ipc/hyprland/hyprland.h"
#include "ipc/niri/niri.h"
#include "ipc/mango/mango.h"
#include "utils/log.h"

axctl_compositor_t *axctl_detect_compositor(void)
{
    axctl_compositor_t *comp = NULL;

    /* Try Hyprland first */
    comp = hyprland_compositor_create();
    if (comp) {
        LOG_INFO("Detected compositor: Hyprland");
        return comp;
    }

    /* Try Niri */
    comp = niri_compositor_create();
    if (comp) {
        LOG_INFO("Detected compositor: Niri");
        return comp;
    }

    /* Try Mango */
    comp = mango_compositor_create();
    if (comp) {
        LOG_INFO("Detected compositor: Mango");
        return comp;
    }

    LOG_ERROR("No supported compositor detected");
    return NULL;
}

void axctl_compositor_destroy(axctl_compositor_t *comp)
{
    if (!comp) return;
    if (comp->destroy)
        comp->destroy(comp->priv);
    free(comp);
}
