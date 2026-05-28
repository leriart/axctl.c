/* src/ipc/wayland/wayland_client.c -- Wayland client wrapper
 *
 * Provides a thin wrapper around libwayland-client for the idle manager.
 * Handles connection to the Wayland display and binding of:
 *   - wl_compositor (for creating surfaces)
 *   - wl_seat (for idle notifications)
 *   - ext_idle_notifier_v1 (for idle/resume detection)
 *   - zwp_idle_inhibit_manager_v1 (for idle inhibition)
 *
 * NOTE: The ext_idle_notify_v1 and zwp_idle_inhibit_v1 protocols require
 * wayland-scanner generated headers. For this migration, we use
 * wl_proxy-based manual binding as a compatible approach.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <wayland-client.h>

#include "ipc/wayland/wayland_client.h"
#include "utils/log.h"

/* ------------------------------------------------------------------ */
/* Wayland context                                                     */
/* ------------------------------------------------------------------ */

struct axctl_wayland_ctx {
    struct wl_display    *display;
    struct wl_registry   *registry;
    struct wl_compositor *compositor;
    struct wl_seat       *seat;

    /* Protocol globals – stored as wl_proxy since we don't have
     * scanner-generated types. Null if not available. */
    struct wl_proxy      *idle_notifier;     /* ext_idle_notifier_v1 */
    struct wl_proxy      *inhibit_manager;   /* zwp_idle_inhibit_manager_v1 */

    uint32_t idle_notifier_name;
    uint32_t idle_notifier_version;
    uint32_t inhibit_mgr_name;

    pthread_t dispatch_thread;
    int       running;
    pthread_mutex_t mu;
};

/* ------------------------------------------------------------------ */
/* Registry listener                                                   */
/* ------------------------------------------------------------------ */

static void registry_global(void *data, struct wl_registry *registry,
                            uint32_t name, const char *interface,
                            uint32_t version)
{
    struct axctl_wayland_ctx *ctx = (struct axctl_wayland_ctx *)data;

    if (strcmp(interface, "wl_compositor") == 0) {
        ctx->compositor = wl_registry_bind(registry, name,
            &wl_compositor_interface, 1);
    } else if (strcmp(interface, "wl_seat") == 0) {
        ctx->seat = wl_registry_bind(registry, name,
            &wl_seat_interface, 1);
    } else if (strcmp(interface, "ext_idle_notifier_v1") == 0) {
        /* Store info for later manual binding */
        ctx->idle_notifier_name = name;
        ctx->idle_notifier_version = version > 2 ? 2 : version;
    } else if (strcmp(interface, "zwp_idle_inhibit_manager_v1") == 0) {
        ctx->inhibit_mgr_name = name;
    }
}

static void registry_global_remove(void *data, struct wl_registry *registry,
                                   uint32_t name)
{
    (void)data; (void)registry; (void)name;
}

static const struct wl_registry_listener registry_listener = {
    .global = registry_global,
    .global_remove = registry_global_remove,
};

/* ------------------------------------------------------------------ */
/* Background dispatch thread                                          */
/* ------------------------------------------------------------------ */

static void *wayland_dispatch_loop(void *arg)
{
    struct axctl_wayland_ctx *ctx = (struct axctl_wayland_ctx *)arg;
    while (ctx->running) {
        if (wl_display_dispatch(ctx->display) < 0) {
            LOG_ERROR("Wayland dispatch error in idle manager");
            break;
        }
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */

axctl_wayland_ctx_t *axctl_wayland_connect(void)
{
    struct wl_display *display = wl_display_connect(NULL);
    if (!display) {
        LOG_ERROR("Cannot connect to Wayland display for idle management");
        return NULL;
    }

    struct axctl_wayland_ctx *ctx = calloc(1, sizeof(*ctx));
    if (!ctx) {
        wl_display_disconnect(display);
        return NULL;
    }

    ctx->display = display;
    pthread_mutex_init(&ctx->mu, NULL);

    ctx->registry = wl_display_get_registry(display);
    if (!ctx->registry) {
        wl_display_disconnect(display);
        free(ctx);
        return NULL;
    }

    wl_registry_add_listener(ctx->registry, &registry_listener, ctx);

    /* Roundtrip to discover globals */
    wl_display_roundtrip(display);

    /* Start background dispatch thread */
    ctx->running = 1;
    pthread_create(&ctx->dispatch_thread, NULL, wayland_dispatch_loop, ctx);
    pthread_detach(ctx->dispatch_thread);

    return ctx;
}

void axctl_wayland_disconnect(axctl_wayland_ctx_t *ctx)
{
    if (!ctx) return;
    ctx->running = 0;
    if (ctx->display)
        wl_display_disconnect(ctx->display);
    pthread_mutex_destroy(&ctx->mu);
    free(ctx);
}

struct wl_display *axctl_wayland_get_display(axctl_wayland_ctx_t *ctx)
{
    return ctx ? ctx->display : NULL;
}

struct wl_compositor *axctl_wayland_get_compositor(axctl_wayland_ctx_t *ctx)
{
    return ctx ? ctx->compositor : NULL;
}

struct wl_seat *axctl_wayland_get_seat(axctl_wayland_ctx_t *ctx)
{
    return ctx ? ctx->seat : NULL;
}

int axctl_wayland_has_idle_notifier(axctl_wayland_ctx_t *ctx)
{
    return ctx && ctx->idle_notifier_name != 0;
}

int axctl_wayland_has_inhibit_manager(axctl_wayland_ctx_t *ctx)
{
    return ctx && ctx->inhibit_mgr_name != 0;
}

void axctl_wayland_roundtrip(axctl_wayland_ctx_t *ctx)
{
    if (ctx && ctx->display)
        wl_display_roundtrip(ctx->display);
}

void axctl_wayland_flush(axctl_wayland_ctx_t *ctx)
{
    if (ctx && ctx->display)
        wl_display_flush(ctx->display);
}
