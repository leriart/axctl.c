/* src/server/idle.c -- Idle management
 *
 * Provides idle detection, inhibition, and monitoring via:
 *   - ext_idle_notifier_v1 (Wayland protocol for idle/resume notifications)
 *   - zwp_idle_inhibit_manager_v1 (Wayland protocol for idle inhibition)
 *   - systemd-inhibit (system-wide idle/sleep inhibition)
 *   - PulseAudio/PipeWire sink-input checks (media inhibitor)
 *   - /proc scanning (app inhibitor)
 *
 * Thread safety: all public functions are mutex-protected.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <pthread.h>
#include <dirent.h>
#include <ctype.h>
#include <sys/wait.h>

#include "protocols/ext-idle-notify-v1-client-protocol.h"
#include "protocols/idle-inhibit-unstable-v1-client-protocol.h"
#include "server/idle.h"
#include "ipc/wayland/wayland_client.h"
#include "utils/log.h"
#include "utils/strutil.h"

/* ------------------------------------------------------------------ */
/* Internal types                                                      */
/* ------------------------------------------------------------------ */

typedef struct idle_monitor {
    uint32_t id;
    uint32_t timeout_ms;
    int      respect_inhibitors;
    int      enabled;
    int      is_idle;
    int      deleted;

    /* Real Wayland notification proxy */
    struct ext_idle_notification_v1 *notification;

    /* Condition variable for blocking waits */
    pthread_cond_t  cond;
    int             signaled;
} idle_monitor_t;

typedef struct idle_inhibitor {
    uint32_t id;
    int      enabled;
    int      deleted;

    /* Real Wayland inhibitor proxy + surface */
    struct wl_surface          *surface;
    struct zwp_idle_inhibitor_v1 *inhibitor;
} idle_inhibitor_t;

#define MAX_MONITORS   64
#define MAX_INHIBITORS 64

struct axctl_idle_manager {
    axctl_wayland_ctx_t *wl_ctx;
    pthread_mutex_t      mu;

    uint32_t legacy_inhibitor_id;

    idle_monitor_t  monitors[MAX_MONITORS];
    int             monitor_count;
    uint32_t        next_monitor_id;

    idle_inhibitor_t inhibitors[MAX_INHIBITORS];
    int              inhibitor_count;
    uint32_t         next_inhibitor_id;

    /* System-wide inhibitor via systemd-inhibit */
    int      system_inhibited;
    pid_t    system_inhibit_pid;

    /* Callback for idle monitor state changes */
    axctl_idle_monitor_cb_t on_changed;
    void                   *cb_userdata;
};

/* ------------------------------------------------------------------ */
/* Helpers                                                             */
/* ------------------------------------------------------------------ */

static idle_monitor_t *find_monitor(axctl_idle_manager_t *im, uint32_t id)
{
    for (int i = 0; i < im->monitor_count; i++) {
        if (im->monitors[i].id == id && !im->monitors[i].deleted)
            return &im->monitors[i];
    }
    return NULL;
}

static idle_inhibitor_t *find_inhibitor(axctl_idle_manager_t *im, uint32_t id)
{
    for (int i = 0; i < im->inhibitor_count; i++) {
        if (im->inhibitors[i].id == id && !im->inhibitors[i].deleted)
            return &im->inhibitors[i];
    }
    return NULL;
}

/* Case-insensitive substring search */
static int str_contains_ci(const char *haystack, const char *needle)
{
    if (!haystack || !needle) return 0;
    size_t hlen = strlen(haystack), nlen = strlen(needle);
    if (nlen > hlen) return 0;
    for (size_t i = 0; i <= hlen - nlen; i++) {
        int match = 1;
        for (size_t j = 0; j < nlen; j++) {
            if (tolower((unsigned char)haystack[i+j]) !=
                tolower((unsigned char)needle[j])) {
                match = 0;
                break;
            }
        }
        if (match) return 1;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Wayland idle notification listeners                                 */
/* ------------------------------------------------------------------ */

/* Set monitor idle state and fire callback (caller must hold im->mu) */
static void set_monitor_idle(axctl_idle_manager_t *im,
                             idle_monitor_t *mon, int idle)
{
    if (mon->is_idle == idle)
        return;

    mon->is_idle = idle;
    mon->signaled = 1;
    pthread_cond_signal(&mon->cond);

    if (im->on_changed)
        im->on_changed(mon->id, idle, im->cb_userdata);
}

/* Listener callbacks for ext_idle_notification_v1 */
static void notification_idled(void *data,
                               struct ext_idle_notification_v1 *notif)
{
    idle_monitor_t *mon = (idle_monitor_t *)data;
    (void)notif;

    /* We need the manager pointer to fire the callback. Store it via
     * user_data on the notification proxy. */
    axctl_idle_manager_t *im = ext_idle_notification_v1_get_user_data(notif);
    if (!im) return;

    pthread_mutex_lock(&im->mu);
    set_monitor_idle(im, mon, 1);
    pthread_mutex_unlock(&im->mu);
}

static void notification_resumed(void *data,
                                 struct ext_idle_notification_v1 *notif)
{
    idle_monitor_t *mon = (idle_monitor_t *)data;
    (void)notif;

    axctl_idle_manager_t *im = ext_idle_notification_v1_get_user_data(notif);
    if (!im) return;

    pthread_mutex_lock(&im->mu);
    set_monitor_idle(im, mon, 0);
    pthread_mutex_unlock(&im->mu);
}

static const struct ext_idle_notification_v1_listener idle_notif_listener = {
    .idled   = notification_idled,
    .resumed = notification_resumed,
};

/* ------------------------------------------------------------------ */
/* Refresh monitor: (re)create the Wayland notification                */
/* ------------------------------------------------------------------ */

/* Must be called with im->mu held */
static void refresh_monitor(axctl_idle_manager_t *im, idle_monitor_t *mon)
{
    /* Destroy previous notification if any */
    if (mon->notification) {
        ext_idle_notification_v1_destroy(mon->notification);
        mon->notification = NULL;
    }

    if (!mon->enabled)
        return;

    struct ext_idle_notifier_v1 *notifier =
        axctl_wayland_get_idle_notifier(im->wl_ctx);
    struct wl_seat *seat = axctl_wayland_get_seat(im->wl_ctx);

    if (!notifier || !seat) {
        LOG_WARN("Cannot create idle notification: notifier=%p seat=%p",
                 (void *)notifier, (void *)seat);
        return;
    }

    /* Create the notification: timeout in milliseconds */
    mon->notification = ext_idle_notifier_v1_get_idle_notification(
        notifier, mon->timeout_ms, seat);

    if (!mon->notification) {
        LOG_ERROR("Failed to create ext_idle_notification_v1");
        return;
    }

    /* Store the manager pointer so callbacks can access it */
    ext_idle_notification_v1_set_user_data(mon->notification, im);

    /* Set the idled/resumed listeners, passing the monitor as data */
    ext_idle_notification_v1_add_listener(
        mon->notification, &idle_notif_listener, mon);

    /* Flush to ensure the compositor receives the request */
    axctl_wayland_flush(im->wl_ctx);
}

/* ------------------------------------------------------------------ */
/* Inhibitor helpers                                                   */
/* ------------------------------------------------------------------ */

/* Create/destroy the real Wayland idle inhibitor.
 * Must be called with im->mu held. */
static void refresh_inhibitor(axctl_idle_manager_t *im, idle_inhibitor_t *inh)
{
    if (inh->enabled && !inh->inhibitor) {
        struct zwp_idle_inhibit_manager_v1 *mgr =
            axctl_wayland_get_inhibit_manager(im->wl_ctx);
        struct wl_compositor *comp =
            axctl_wayland_get_compositor(im->wl_ctx);

        if (!mgr || !comp) return;

        /* Create a dummy surface for the inhibitor */
        if (!inh->surface)
            inh->surface = wl_compositor_create_surface(comp);

        if (!inh->surface) return;

        inh->inhibitor = zwp_idle_inhibit_manager_v1_create_inhibitor(
            mgr, inh->surface);

        axctl_wayland_flush(im->wl_ctx);
    } else if (!inh->enabled && inh->inhibitor) {
        zwp_idle_inhibitor_v1_destroy(inh->inhibitor);
        inh->inhibitor = NULL;
        if (inh->surface) {
            wl_surface_destroy(inh->surface);
            inh->surface = NULL;
        }
        axctl_wayland_flush(im->wl_ctx);
    }
}

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */

axctl_idle_manager_t *axctl_idle_manager_create(void)
{
    axctl_wayland_ctx_t *wl = axctl_wayland_connect();
    if (!wl) {
        LOG_WARN("Failed to connect Wayland for idle management");
        return NULL;
    }

    axctl_idle_manager_t *im = calloc(1, sizeof(*im));
    if (!im) {
        axctl_wayland_disconnect(wl);
        return NULL;
    }

    im->wl_ctx = wl;
    pthread_mutex_init(&im->mu, NULL);

    return im;
}

void axctl_idle_manager_destroy(axctl_idle_manager_t *im)
{
    if (!im) return;

    /* Destroy all active notifications */
    for (int i = 0; i < im->monitor_count; i++) {
        if (im->monitors[i].notification) {
            ext_idle_notification_v1_destroy(im->monitors[i].notification);
            im->monitors[i].notification = NULL;
        }
        pthread_cond_destroy(&im->monitors[i].cond);
    }

    /* Destroy all active inhibitors */
    for (int i = 0; i < im->inhibitor_count; i++) {
        if (im->inhibitors[i].inhibitor)
            zwp_idle_inhibitor_v1_destroy(im->inhibitors[i].inhibitor);
        if (im->inhibitors[i].surface)
            wl_surface_destroy(im->inhibitors[i].surface);
    }

    /* Kill system inhibitor if running */
    if (im->system_inhibit_pid > 0) {
        kill(im->system_inhibit_pid, SIGTERM);
        waitpid(im->system_inhibit_pid, NULL, 0);
    }

    axctl_wayland_disconnect(im->wl_ctx);
    pthread_mutex_destroy(&im->mu);
    free(im);
}

void axctl_idle_manager_set_callback(axctl_idle_manager_t *im,
                                     axctl_idle_monitor_cb_t cb,
                                     void *userdata)
{
    if (!im) return;
    pthread_mutex_lock(&im->mu);
    im->on_changed = cb;
    im->cb_userdata = userdata;
    pthread_mutex_unlock(&im->mu);
}

/* ------------------------------------------------------------------ */
/* Inhibit (legacy toggle)                                             */
/* ------------------------------------------------------------------ */

int axctl_idle_inhibit(axctl_idle_manager_t *im, int on)
{
    if (!im) return -1;
    pthread_mutex_lock(&im->mu);

    if (im->legacy_inhibitor_id == 0) {
        im->next_inhibitor_id++;
        im->legacy_inhibitor_id = im->next_inhibitor_id;
        if (im->inhibitor_count < MAX_INHIBITORS) {
            idle_inhibitor_t *inh = &im->inhibitors[im->inhibitor_count++];
            memset(inh, 0, sizeof(*inh));
            inh->id = im->legacy_inhibitor_id;
            inh->enabled = 0;
        }
    }

    idle_inhibitor_t *inh = find_inhibitor(im, im->legacy_inhibitor_id);
    if (!inh) {
        pthread_mutex_unlock(&im->mu);
        return -1;
    }

    if (inh->enabled == on) {
        pthread_mutex_unlock(&im->mu);
        return 0;
    }

    inh->enabled = on;
    refresh_inhibitor(im, inh);

    pthread_mutex_unlock(&im->mu);
    return 0;
}

int axctl_idle_is_inhibited(axctl_idle_manager_t *im)
{
    if (!im) return 0;
    pthread_mutex_lock(&im->mu);
    int result = 0;
    if (im->legacy_inhibitor_id) {
        idle_inhibitor_t *inh = find_inhibitor(im, im->legacy_inhibitor_id);
        if (inh) result = inh->enabled;
    }
    pthread_mutex_unlock(&im->mu);
    return result;
}

/* ------------------------------------------------------------------ */
/* Wait for idle/resume                                                */
/* ------------------------------------------------------------------ */

/* Internal: create a short-lived notification, wait for idle event,
 * then destroy it. Used by axctl_idle_wait / axctl_idle_is_idle. */
typedef struct {
    int              fired;
    pthread_mutex_t  mu;
    pthread_cond_t   cond;
} oneshot_ctx_t;

static void oneshot_idled(void *data,
                          struct ext_idle_notification_v1 *notif)
{
    (void)notif;
    oneshot_ctx_t *ctx = (oneshot_ctx_t *)data;
    pthread_mutex_lock(&ctx->mu);
    ctx->fired = 1;
    pthread_cond_signal(&ctx->cond);
    pthread_mutex_unlock(&ctx->mu);
}

static void oneshot_resumed(void *data,
                            struct ext_idle_notification_v1 *notif)
{
    (void)data; (void)notif;
    /* Not used for one-shot idle check, but required by listener struct */
}

static const struct ext_idle_notification_v1_listener oneshot_listener = {
    .idled   = oneshot_idled,
    .resumed = oneshot_resumed,
};

int axctl_idle_wait(axctl_idle_manager_t *im, uint32_t timeout_ms)
{
    if (!im) return -1;
    if (!axctl_wayland_has_idle_notifier(im->wl_ctx)) {
        LOG_ERROR("idle_notify not supported by compositor");
        return -1;
    }

    struct ext_idle_notifier_v1 *notifier =
        axctl_wayland_get_idle_notifier(im->wl_ctx);
    struct wl_seat *seat = axctl_wayland_get_seat(im->wl_ctx);
    if (!notifier || !seat) return -1;

    /* Create a one-shot notification to wait for idle */
    struct ext_idle_notification_v1 *notif =
        ext_idle_notifier_v1_get_idle_notification(notifier, timeout_ms, seat);
    if (!notif) return -1;

    oneshot_ctx_t ctx = { .fired = 0 };
    pthread_mutex_init(&ctx.mu, NULL);
    pthread_cond_init(&ctx.cond, NULL);

    ext_idle_notification_v1_add_listener(notif, &oneshot_listener, &ctx);
    axctl_wayland_flush(im->wl_ctx);

    /* Block until the idled event fires (dispatched by background thread) */
    pthread_mutex_lock(&ctx.mu);
    while (!ctx.fired)
        pthread_cond_wait(&ctx.cond, &ctx.mu);
    pthread_mutex_unlock(&ctx.mu);

    ext_idle_notification_v1_destroy(notif);
    axctl_wayland_flush(im->wl_ctx);
    pthread_mutex_destroy(&ctx.mu);
    pthread_cond_destroy(&ctx.cond);

    return 0;
}

/* Context for wait-resume one-shot */
typedef struct {
    int              idled;
    int              resumed;
    pthread_mutex_t  mu;
    pthread_cond_t   cond;
} resume_ctx_t;

static void resume_idled_cb(void *data, struct ext_idle_notification_v1 *n)
{
    (void)n;
    resume_ctx_t *r = (resume_ctx_t *)data;
    pthread_mutex_lock(&r->mu);
    r->idled = 1;
    pthread_mutex_unlock(&r->mu);
}

static void resume_resumed_cb(void *data, struct ext_idle_notification_v1 *n)
{
    (void)n;
    resume_ctx_t *r = (resume_ctx_t *)data;
    pthread_mutex_lock(&r->mu);
    r->resumed = 1;
    pthread_cond_signal(&r->cond);
    pthread_mutex_unlock(&r->mu);
}

static const struct ext_idle_notification_v1_listener resume_listener = {
    .idled   = resume_idled_cb,
    .resumed = resume_resumed_cb,
};

int axctl_idle_wait_resume(axctl_idle_manager_t *im, uint32_t timeout_ms)
{
    if (!im) return -1;
    if (!axctl_wayland_has_idle_notifier(im->wl_ctx)) {
        LOG_ERROR("idle_notify not supported by compositor");
        return -1;
    }

    struct ext_idle_notifier_v1 *notifier =
        axctl_wayland_get_idle_notifier(im->wl_ctx);
    struct wl_seat *seat = axctl_wayland_get_seat(im->wl_ctx);
    if (!notifier || !seat) return -1;

    struct ext_idle_notification_v1 *notif =
        ext_idle_notifier_v1_get_idle_notification(notifier, timeout_ms, seat);
    if (!notif) return -1;

    resume_ctx_t *rctx = calloc(1, sizeof(*rctx));
    pthread_mutex_init(&rctx->mu, NULL);
    pthread_cond_init(&rctx->cond, NULL);

    ext_idle_notification_v1_add_listener(notif, &resume_listener, rctx);
    axctl_wayland_flush(im->wl_ctx);

    /* Block until resumed event fires */
    pthread_mutex_lock(&rctx->mu);
    while (!rctx->resumed)
        pthread_cond_wait(&rctx->cond, &rctx->mu);
    pthread_mutex_unlock(&rctx->mu);

    ext_idle_notification_v1_destroy(notif);
    axctl_wayland_flush(im->wl_ctx);
    pthread_mutex_destroy(&rctx->mu);
    pthread_cond_destroy(&rctx->cond);
    free(rctx);

    return 0;
}

int axctl_idle_is_idle(axctl_idle_manager_t *im, uint32_t timeout_ms, int *is_idle)
{
    if (!im) return -1;
    *is_idle = 0;

    if (!axctl_wayland_has_idle_notifier(im->wl_ctx))
        return -1;

    struct ext_idle_notifier_v1 *notifier =
        axctl_wayland_get_idle_notifier(im->wl_ctx);
    struct wl_seat *seat = axctl_wayland_get_seat(im->wl_ctx);
    if (!notifier || !seat) return -1;

    /* Create a short-lived notification and immediately sync to see
     * if the idled event fires during the roundtrip. */
    struct ext_idle_notification_v1 *notif =
        ext_idle_notifier_v1_get_idle_notification(notifier, timeout_ms, seat);
    if (!notif) return -1;

    oneshot_ctx_t ctx = { .fired = 0 };
    pthread_mutex_init(&ctx.mu, NULL);
    pthread_cond_init(&ctx.cond, NULL);

    ext_idle_notification_v1_add_listener(notif, &oneshot_listener, &ctx);
    axctl_wayland_flush(im->wl_ctx);

    /* Do a sync roundtrip – if already idle, the event fires during this */
    axctl_wayland_roundtrip(im->wl_ctx);

    pthread_mutex_lock(&ctx.mu);
    *is_idle = ctx.fired;
    pthread_mutex_unlock(&ctx.mu);

    ext_idle_notification_v1_destroy(notif);
    axctl_wayland_flush(im->wl_ctx);
    pthread_mutex_destroy(&ctx.mu);
    pthread_cond_destroy(&ctx.cond);

    return 0;
}

int axctl_idle_wait_input(axctl_idle_manager_t *im, uint32_t timeout_ms)
{
    /* ext_idle_notifier_v1 is input-based by default */
    return axctl_idle_wait(im, timeout_ms);
}

int axctl_idle_wait_input_resume(axctl_idle_manager_t *im, uint32_t timeout_ms)
{
    return axctl_idle_wait_resume(im, timeout_ms);
}

int axctl_idle_is_input_idle(axctl_idle_manager_t *im, uint32_t timeout_ms,
                             int *is_idle)
{
    return axctl_idle_is_idle(im, timeout_ms, is_idle);
}

/* ------------------------------------------------------------------ */
/* Idle monitors (persistent watchers)                                 */
/* ------------------------------------------------------------------ */

int axctl_idle_monitor_create(axctl_idle_manager_t *im,
                              uint32_t timeout_ms,
                              int respect_inhibitors,
                              int enabled,
                              axctl_idle_monitor_state_t *out)
{
    if (!im) return -1;
    pthread_mutex_lock(&im->mu);

    if (im->monitor_count >= MAX_MONITORS) {
        pthread_mutex_unlock(&im->mu);
        return -1;
    }

    im->next_monitor_id++;
    idle_monitor_t *mon = &im->monitors[im->monitor_count++];
    memset(mon, 0, sizeof(*mon));
    mon->id = im->next_monitor_id;
    mon->timeout_ms = timeout_ms;
    mon->respect_inhibitors = respect_inhibitors;
    mon->enabled = enabled;
    mon->is_idle = 0;
    pthread_cond_init(&mon->cond, NULL);

    /* Create real Wayland notification if enabled */
    refresh_monitor(im, mon);

    out->id = mon->id;
    out->timeout_ms = mon->timeout_ms;
    out->respect_inhibitors = mon->respect_inhibitors;
    out->enabled = mon->enabled;
    out->is_idle = mon->is_idle;

    pthread_mutex_unlock(&im->mu);
    return 0;
}

int axctl_idle_monitor_update(axctl_idle_manager_t *im, uint32_t id,
                              uint32_t timeout_ms, int respect_inhibitors,
                              int enabled, axctl_idle_monitor_state_t *out)
{
    if (!im) return -1;
    pthread_mutex_lock(&im->mu);

    idle_monitor_t *mon = find_monitor(im, id);
    if (!mon) {
        pthread_mutex_unlock(&im->mu);
        return -1;
    }

    mon->timeout_ms = timeout_ms;
    mon->respect_inhibitors = respect_inhibitors;
    mon->enabled = enabled;

    /* Recreate the notification with updated parameters */
    refresh_monitor(im, mon);

    out->id = mon->id;
    out->timeout_ms = mon->timeout_ms;
    out->respect_inhibitors = mon->respect_inhibitors;
    out->enabled = mon->enabled;
    out->is_idle = mon->is_idle;

    pthread_mutex_unlock(&im->mu);
    return 0;
}

int axctl_idle_monitor_get(axctl_idle_manager_t *im, uint32_t id,
                           axctl_idle_monitor_state_t *out)
{
    if (!im) return -1;
    pthread_mutex_lock(&im->mu);

    idle_monitor_t *mon = find_monitor(im, id);
    if (!mon) {
        pthread_mutex_unlock(&im->mu);
        return -1;
    }

    out->id = mon->id;
    out->timeout_ms = mon->timeout_ms;
    out->respect_inhibitors = mon->respect_inhibitors;
    out->enabled = mon->enabled;
    out->is_idle = mon->is_idle;

    pthread_mutex_unlock(&im->mu);
    return 0;
}

int axctl_idle_monitor_destroy(axctl_idle_manager_t *im, uint32_t id)
{
    if (!im) return -1;
    pthread_mutex_lock(&im->mu);

    idle_monitor_t *mon = find_monitor(im, id);
    if (!mon) {
        pthread_mutex_unlock(&im->mu);
        return -1;
    }

    /* Destroy the Wayland notification */
    if (mon->notification) {
        ext_idle_notification_v1_destroy(mon->notification);
        mon->notification = NULL;
        axctl_wayland_flush(im->wl_ctx);
    }

    mon->deleted = 1;
    pthread_cond_destroy(&mon->cond);

    pthread_mutex_unlock(&im->mu);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Idle inhibitors (persistent)                                        */
/* ------------------------------------------------------------------ */

int axctl_idle_inhibitor_create(axctl_idle_manager_t *im, int enabled,
                                axctl_idle_inhibitor_state_t *out)
{
    if (!im) return -1;
    pthread_mutex_lock(&im->mu);

    if (im->inhibitor_count >= MAX_INHIBITORS) {
        pthread_mutex_unlock(&im->mu);
        return -1;
    }

    im->next_inhibitor_id++;
    idle_inhibitor_t *inh = &im->inhibitors[im->inhibitor_count++];
    memset(inh, 0, sizeof(*inh));
    inh->id = im->next_inhibitor_id;
    inh->enabled = enabled;

    /* Create real Wayland inhibitor if enabled */
    refresh_inhibitor(im, inh);

    out->id = inh->id;
    out->enabled = inh->enabled;

    pthread_mutex_unlock(&im->mu);
    return 0;
}

int axctl_idle_inhibitor_set(axctl_idle_manager_t *im, uint32_t id,
                             int enabled, axctl_idle_inhibitor_state_t *out)
{
    if (!im) return -1;
    pthread_mutex_lock(&im->mu);

    idle_inhibitor_t *inh = find_inhibitor(im, id);
    if (!inh) {
        pthread_mutex_unlock(&im->mu);
        return -1;
    }

    inh->enabled = enabled;
    refresh_inhibitor(im, inh);

    out->id = inh->id;
    out->enabled = inh->enabled;

    pthread_mutex_unlock(&im->mu);
    return 0;
}

int axctl_idle_inhibitor_get(axctl_idle_manager_t *im, uint32_t id,
                             axctl_idle_inhibitor_state_t *out)
{
    if (!im) return -1;
    pthread_mutex_lock(&im->mu);

    idle_inhibitor_t *inh = find_inhibitor(im, id);
    if (!inh) {
        pthread_mutex_unlock(&im->mu);
        return -1;
    }

    out->id = inh->id;
    out->enabled = inh->enabled;

    pthread_mutex_unlock(&im->mu);
    return 0;
}

int axctl_idle_inhibitor_destroy(axctl_idle_manager_t *im, uint32_t id)
{
    if (!im) return -1;
    pthread_mutex_lock(&im->mu);

    idle_inhibitor_t *inh = find_inhibitor(im, id);
    if (!inh) {
        pthread_mutex_unlock(&im->mu);
        return -1;
    }

    /* Destroy the real Wayland inhibitor */
    if (inh->inhibitor) {
        zwp_idle_inhibitor_v1_destroy(inh->inhibitor);
        inh->inhibitor = NULL;
    }
    if (inh->surface) {
        wl_surface_destroy(inh->surface);
        inh->surface = NULL;
    }
    axctl_wayland_flush(im->wl_ctx);

    inh->deleted = 1;
    if (im->legacy_inhibitor_id == id)
        im->legacy_inhibitor_id = 0;

    pthread_mutex_unlock(&im->mu);
    return 0;
}

/* ------------------------------------------------------------------ */
/* System-wide inhibition (via systemd-inhibit)                        */
/* ------------------------------------------------------------------ */

int axctl_idle_inhibit_system(axctl_idle_manager_t *im, int on)
{
    if (!im) return -1;
    pthread_mutex_lock(&im->mu);

    if (im->system_inhibited == on) {
        pthread_mutex_unlock(&im->mu);
        return 0;
    }

    if (on) {
        pid_t pid = fork();
        if (pid < 0) {
            pthread_mutex_unlock(&im->mu);
            return -1;
        }
        if (pid == 0) {
            execlp("systemd-inhibit",
                   "systemd-inhibit",
                   "--what=sleep:idle",
                   "--who=axctl",
                   "--why=IPC daemon running",
                   "--mode=block",
                   "sleep", "infinity",
                   (char *)NULL);
            _exit(1);
        }
        im->system_inhibit_pid = pid;
        im->system_inhibited = 1;
    } else {
        if (im->system_inhibit_pid > 0) {
            kill(im->system_inhibit_pid, SIGTERM);
            waitpid(im->system_inhibit_pid, NULL, 0);
            im->system_inhibit_pid = 0;
        }
        im->system_inhibited = 0;
    }

    pthread_mutex_unlock(&im->mu);
    return 0;
}

int axctl_idle_is_system_inhibited(axctl_idle_manager_t *im)
{
    if (!im) return 0;
    pthread_mutex_lock(&im->mu);
    int r = im->system_inhibited;
    pthread_mutex_unlock(&im->mu);
    return r;
}

/* ------------------------------------------------------------------ */
/* Media inhibitor check (PulseAudio/PipeWire sink-inputs)             */
/* ------------------------------------------------------------------ */

static const char *media_blacklist[] = {
    "speech-dispatcher", "speech-dispatcher-dummy",
    "sndio", "pipewire", "wireplumber", "galene", NULL
};

static int is_media_blacklisted(const char *app)
{
    for (int i = 0; media_blacklist[i]; i++) {
        if (str_contains_ci(app, media_blacklist[i]))
            return 1;
    }
    return 0;
}

int axctl_idle_media_check(axctl_idle_manager_t *im, json_object **result)
{
    (void)im;

    *result = json_object_new_object();
    json_object *apps_arr = json_object_new_array();
    int count = 0;

    FILE *fp = popen("pactl list sink-inputs 2>/dev/null", "r");
    if (fp) {
        char line[1024];
        int in_block = 0;
        int corked = 0, muted = 0;
        char app_name[256] = {0};
        int in_props = 0;

        while (fgets(line, sizeof(line), fp)) {
            char *trimmed = line;
            while (*trimmed == ' ' || *trimmed == '\t') trimmed++;

            if (strncmp(trimmed, "Sink Input #", 12) == 0 ||
                strncmp(trimmed, "SinkInput #", 11) == 0) {
                if (in_block && !corked && !muted && app_name[0] &&
                    !is_media_blacklisted(app_name)) {
                    json_object_array_add(apps_arr,
                        json_object_new_string(app_name));
                    count++;
                }
                in_block = 1;
                corked = muted = 0;
                app_name[0] = 0;
                in_props = 0;
                continue;
            }

            if (strcasecmp(trimmed, "Corked: yes\n") == 0 ||
                strcasecmp(trimmed, "Corked: yes\r\n") == 0)
                corked = 1;
            if (strcasecmp(trimmed, "Mute: yes\n") == 0 ||
                strcasecmp(trimmed, "Mute: yes\r\n") == 0)
                muted = 1;

            if (strcmp(trimmed, "Properties:\n") == 0 ||
                strcmp(trimmed, "Properties:\r\n") == 0) {
                in_props = 1;
                continue;
            }

            if (in_props && !app_name[0]) {
                if (strncmp(trimmed, "application.name = ", 19) == 0) {
                    char *val = trimmed + 19;
                    char *p = val;
                    while (*p == '"') p++;
                    char *end = p + strlen(p) - 1;
                    while (end > p && (*end == '\n' || *end == '\r' || *end == '"'))
                        *end-- = 0;
                    snprintf(app_name, sizeof(app_name), "%s", p);
                } else if (strncmp(trimmed, "application.process.binary = ", 29) == 0 && !app_name[0]) {
                    char *val = trimmed + 29;
                    char *p = val;
                    while (*p == '"') p++;
                    char *end = p + strlen(p) - 1;
                    while (end > p && (*end == '\n' || *end == '\r' || *end == '"'))
                        *end-- = 0;
                    snprintf(app_name, sizeof(app_name), "%s", p);
                }
            }
        }

        if (in_block && !corked && !muted && app_name[0] &&
            !is_media_blacklisted(app_name)) {
            json_object_array_add(apps_arr,
                json_object_new_string(app_name));
            count++;
        }

        pclose(fp);
    }

    json_object_object_add(*result, "count", json_object_new_int(count));
    json_object_object_add(*result, "apps", apps_arr);
    return 0;
}

/* ------------------------------------------------------------------ */
/* App inhibitor check (/proc scanning)                                */
/* ------------------------------------------------------------------ */

static const char *default_patterns[] = {
    "vlc", "mpv", "firefox", "chromium", "chrome",
    "brave", "vivaldi", "steam", NULL
};

int axctl_idle_app_check(axctl_idle_manager_t *im,
                         char **patterns, int pattern_count,
                         json_object **result)
{
    (void)im;
    *result = json_object_new_object();

    const char **pats = (const char **)patterns;
    int pcount = pattern_count;
    if (!pats || pcount <= 0) {
        pats = default_patterns;
        pcount = 0;
        while (default_patterns[pcount]) pcount++;
    }

    if (getenv("HYPRLAND_INSTANCE_SIGNATURE")) {
        FILE *fp = popen("hyprctl clients -j 2>/dev/null", "r");
        if (fp) {
            char buf[65536];
            size_t n = fread(buf, 1, sizeof(buf)-1, fp);
            buf[n] = 0;
            pclose(fp);

            json_object *arr = json_tokener_parse(buf);
            if (arr && json_object_is_type(arr, json_type_array)) {
                int len = json_object_array_length(arr);
                for (int i = 0; i < len; i++) {
                    json_object *w = json_object_array_get_idx(arr, i);
                    json_object *cls = NULL;
                    if (json_object_object_get_ex(w, "class", &cls)) {
                        const char *class_str = json_object_get_string(cls);
                        for (int p = 0; p < pcount; p++) {
                            if (str_contains_ci(class_str, pats[p])) {
                                json_object_object_add(*result, class_str,
                                    json_object_new_boolean(1));
                                break;
                            }
                        }
                    }
                }
            }
            if (arr) json_object_put(arr);
            return 0;
        }
    }

    DIR *proc = opendir("/proc");
    if (!proc) return 0;

    struct dirent *ent;
    while ((ent = readdir(proc)) != NULL) {
        if (!isdigit((unsigned char)ent->d_name[0])) continue;

        char comm_path[512];
        snprintf(comm_path, sizeof(comm_path), "/proc/%s/comm", ent->d_name);

        FILE *f = fopen(comm_path, "r");
        if (!f) continue;

        char comm[256];
        if (fgets(comm, sizeof(comm), f)) {
            char *nl = strchr(comm, '\n');
            if (nl) *nl = 0;

            for (int p = 0; p < pcount; p++) {
                if (str_contains_ci(comm, pats[p])) {
                    json_object_object_add(*result, comm,
                        json_object_new_boolean(1));
                    break;
                }
            }
        }
        fclose(f);
    }

    closedir(proc);
    return 0;
}
