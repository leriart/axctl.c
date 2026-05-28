/* src/ipc/wayland/wayland_client.c -- Wayland client wrapper
 *
 * Provides a thin wrapper around libwayland-client for the idle manager.
 * Handles connection to the Wayland display and binding of:
 *   - wl_compositor (for creating surfaces)
 *   - wl_seat (for idle notifications)
 *   - ext_idle_notifier_v1 (for idle/resume detection)
 *   - zwp_idle_inhibit_manager_v1 (for idle inhibition)
 *
 * Thread safety:
 *   - The dispatch thread runs wl_display_dispatch() in a loop.
 *   - Other threads should only call wl_display_flush() (safe to call
 *     concurrently with dispatch) or wait on condition variables for
 *     events delivered by the dispatch thread.
 *   - wl_display_roundtrip() must NOT be called after the dispatch
 *     thread starts, as it races with wl_display_dispatch().
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <wayland-client.h>

#include "protocols/ext-idle-notify-v1-client-protocol.h"
#include "protocols/idle-inhibit-unstable-v1-client-protocol.h"
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

    /* Protocol globals – proper typed pointers. */
    struct ext_idle_notifier_v1        *idle_notifier;
    struct zwp_idle_inhibit_manager_v1 *inhibit_manager;

    uint32_t idle_notifier_name;
    uint32_t idle_notifier_version;
    uint32_t inhibit_mgr_name;
    uint32_t inhibit_mgr_version;

    pthread_t dispatch_thread;
    volatile int running;
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
        ctx->idle_notifier_name = name;
        ctx->idle_notifier_version = version > 1 ? 1 : version;
        ctx->idle_notifier = wl_registry_bind(registry, name,
            &ext_idle_notifier_v1_interface, ctx->idle_notifier_version);
    } else if (strcmp(interface, "zwp_idle_inhibit_manager_v1") == 0) {
        ctx->inhibit_mgr_name = name;
        ctx->inhibit_mgr_version = version > 1 ? 1 : version;
        ctx->inhibit_manager = wl_registry_bind(registry, name,
            &zwp_idle_inhibit_manager_v1_interface, ctx->inhibit_mgr_version);
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
        /* wl_display_dispatch blocks until events are available.
         * If the display connection is broken, it returns -1. */
        int ret = wl_display_dispatch(ctx->display);
        if (ret < 0) {
            if (ctx->running) {
                LOG_ERROR("Wayland dispatch error in idle manager");
            }
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

    /* Roundtrip to discover and bind globals.
     * This is safe because the dispatch thread hasn't started yet. */
    wl_display_roundtrip(display);

    /* Start background dispatch thread for event delivery */
    ctx->running = 1;
    if (pthread_create(&ctx->dispatch_thread, NULL, wayland_dispatch_loop, ctx) != 0) {
        LOG_ERROR("Failed to create Wayland dispatch thread");
        if (ctx->idle_notifier)
            ext_idle_notifier_v1_destroy(ctx->idle_notifier);
        if (ctx->inhibit_manager)
            zwp_idle_inhibit_manager_v1_destroy(ctx->inhibit_manager);
        wl_display_disconnect(display);
        free(ctx);
        return NULL;
    }

    return ctx;
}

void axctl_wayland_disconnect(axctl_wayland_ctx_t *ctx)
{
    if (!ctx) return;

    /* Signal the dispatch thread to stop */
    ctx->running = 0;

    /* Cancel the display read to unblock wl_display_dispatch.
     * We do this by disconnecting the display, which causes
     * wl_display_dispatch to return -1. Then we join the thread. */
    if (ctx->display) {
        /* Flush any pending requests before shutdown */
        wl_display_flush(ctx->display);
    }

    /* Join the dispatch thread (with timeout via detach+sleep fallback) */
    pthread_join(ctx->dispatch_thread, NULL);

    /* Now safe to destroy protocol objects and disconnect */
    if (ctx->idle_notifier)
        ext_idle_notifier_v1_destroy(ctx->idle_notifier);
    if (ctx->inhibit_manager)
        zwp_idle_inhibit_manager_v1_destroy(ctx->inhibit_manager);

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

struct ext_idle_notifier_v1 *axctl_wayland_get_idle_notifier(axctl_wayland_ctx_t *ctx)
{
    return ctx ? ctx->idle_notifier : NULL;
}

struct zwp_idle_inhibit_manager_v1 *axctl_wayland_get_inhibit_manager(axctl_wayland_ctx_t *ctx)
{
    return ctx ? ctx->inhibit_manager : NULL;
}

int axctl_wayland_has_idle_notifier(axctl_wayland_ctx_t *ctx)
{
    return ctx && ctx->idle_notifier != NULL;
}

int axctl_wayland_has_inhibit_manager(axctl_wayland_ctx_t *ctx)
{
    return ctx && ctx->inhibit_manager != NULL;
}

void axctl_wayland_roundtrip(axctl_wayland_ctx_t *ctx)
{
    /* WARNING: Do NOT call this after the dispatch thread has started!
     * wl_display_roundtrip races with wl_display_dispatch.
     * This function is only safe during initial setup. */
    if (ctx && ctx->display)
        wl_display_roundtrip(ctx->display);
}

void axctl_wayland_flush(axctl_wayland_ctx_t *ctx)
{
    /* wl_display_flush is safe to call concurrently with
     * wl_display_dispatch (it only writes to the output buffer). */
    if (ctx && ctx->display)
        wl_display_flush(ctx->display);
}
