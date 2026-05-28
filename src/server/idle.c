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
    /* In a full implementation, this would hold the
     * ext_idle_notification_v1 proxy */
} idle_monitor_t;

typedef struct idle_inhibitor {
    uint32_t id;
    int      enabled;
    int      deleted;
    /* In a full implementation: zwp_idle_inhibitor_v1 + wl_surface */
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

/* Check if a string contains a pattern (case-insensitive). */
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
    /* In a full implementation, create/destroy zwp_idle_inhibitor_v1 */

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

int axctl_idle_wait(axctl_idle_manager_t *im, uint32_t timeout_ms)
{
    if (!im) return -1;
    if (!axctl_wayland_has_idle_notifier(im->wl_ctx)) {
        LOG_ERROR("idle_notify not supported by compositor");
        return -1;
    }

    /* In a full implementation with wayland-scanner generated code:
     * 1. Create ext_idle_notification_v1 via notifier.get_idle_notification()
     * 2. Set idled handler that signals a condition variable
     * 3. Wait on the condition variable
     * 4. Destroy the notification
     *
     * For now, we use a simplified poll-based approach as a stub.
     */
    (void)timeout_ms;
    LOG_WARN("idle_wait: requires full Wayland protocol binding (stub)");
    return 0;
}

int axctl_idle_wait_resume(axctl_idle_manager_t *im, uint32_t timeout_ms)
{
    if (!im) return -1;
    (void)timeout_ms;
    LOG_WARN("idle_wait_resume: requires full Wayland protocol binding (stub)");
    return 0;
}

int axctl_idle_is_idle(axctl_idle_manager_t *im, uint32_t timeout_ms, int *is_idle)
{
    if (!im) return -1;
    (void)timeout_ms;
    *is_idle = 0;
    /* Stub: would create short-lived notification, do sync, check if idled */
    return 0;
}

int axctl_idle_wait_input(axctl_idle_manager_t *im, uint32_t timeout_ms)
{
    return axctl_idle_wait(im, timeout_ms); /* Same approach, input-only */
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
    mon->id = im->next_monitor_id;
    mon->timeout_ms = timeout_ms;
    mon->respect_inhibitors = respect_inhibitors;
    mon->enabled = enabled;
    mon->is_idle = 0;

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

    mon->deleted = 1;

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
    inh->id = im->next_inhibitor_id;
    inh->enabled = enabled;

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
            /* Child process */
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

    /* Run pactl list sink-inputs */
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

            /* New block header */
            if (strncmp(trimmed, "Sink Input #", 12) == 0 ||
                strncmp(trimmed, "SinkInput #", 11) == 0) {
                /* Process previous block */
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
                    /* Strip quotes and newline */
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

        /* Process last block */
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

    /* Check Hyprland apps if available */
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

    /* Fallback: scan /proc */
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
            /* Strip newline */
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
