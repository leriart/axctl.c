/*
 * axctl - Wayland client wrapper
 *
 * Thin wrapper around libwayland-client providing:
 *   - Display connection management
 *   - Registry and global binding
 *   - Roundtrip/sync helpers
 *   - Protocol object lifecycle
 *
 * Used by IdleManager for idle protocol interaction.
 */
#ifndef AXCTL_WAYLAND_CLIENT_H
#define AXCTL_WAYLAND_CLIENT_H

#include <wayland-client.h>

/* Opaque Wayland context */
typedef struct axctl_wayland_ctx axctl_wayland_ctx_t;

/* Connect to the Wayland display. Returns NULL on failure. */
axctl_wayland_ctx_t *axctl_wayland_connect(void);

/* Disconnect and free all resources */
void axctl_wayland_disconnect(axctl_wayland_ctx_t *ctx);

/* Get the raw wl_display pointer */
struct wl_display *axctl_wayland_get_display(axctl_wayland_ctx_t *ctx);

/* Get bound globals */
struct wl_compositor *axctl_wayland_get_compositor(axctl_wayland_ctx_t *ctx);
struct wl_seat *axctl_wayland_get_seat(axctl_wayland_ctx_t *ctx);

/* Get typed protocol objects */
struct ext_idle_notifier_v1 *axctl_wayland_get_idle_notifier(axctl_wayland_ctx_t *ctx);
struct zwp_idle_inhibit_manager_v1 *axctl_wayland_get_inhibit_manager(axctl_wayland_ctx_t *ctx);

/* Check protocol availability */
int axctl_wayland_has_idle_notifier(axctl_wayland_ctx_t *ctx);
int axctl_wayland_has_inhibit_manager(axctl_wayland_ctx_t *ctx);

/* Display operations */
void axctl_wayland_roundtrip(axctl_wayland_ctx_t *ctx);
void axctl_wayland_flush(axctl_wayland_ctx_t *ctx);

#endif /* AXCTL_WAYLAND_CLIENT_H */
