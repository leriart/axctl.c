/* src/config/config.c -- TOML config loader with include support and inotify watcher
 *
 * Implements a minimal TOML parser (sufficient for axctl's config format):
 *   - [section] headers
 *   - key = value (string, int, float, bool)
 *   - key = "quoted string"
 *   - [[array_of_tables]]
 *   - # comments
 *   - include = ["path1.toml", "path2.toml"]
 *
 * Supports hot-reload via inotify file watching.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/stat.h>
#include <sys/inotify.h>
#include <limits.h>
#include <errno.h>
#include <json-c/json.h>

#include "config/config.h"
#include "ipc/compositor.h"
#include "ipc/config_types.h"
#include "server/config_handler.h"
#include "utils/log.h"
#include "utils/strutil.h"

/* ------------------------------------------------------------------ */
/* Internal types                                                      */
/* ------------------------------------------------------------------ */

#define MAX_IMPORT_DEPTH 10
#define MAX_INCLUDES     32
#define MAX_LINE         4096

/* Simple key-value store for parsed TOML */
typedef struct toml_kv {
    char *section;  /* e.g. "appearance.gaps" */
    char *key;
    char *value;
    struct toml_kv *next;
} toml_kv_t;

struct axctl_toml_config {
    toml_kv_t *head;
    toml_kv_t *tail;

    /* Parsed include paths */
    char *includes[MAX_INCLUDES];
    int   include_count;
};

struct axctl_config_watcher {
    int           inotify_fd;
    char         *config_path;
    axctl_config_reload_cb_t callback;
    void         *userdata;
    int           running;
    pthread_t     thread;

    /* Watched file descriptors */
    int           watch_fds[MAX_INCLUDES + 1];
    int           watch_count;
};

/* ------------------------------------------------------------------ */
/* TOML parser helpers                                                 */
/* ------------------------------------------------------------------ */

static char *trim(char *s)
{
    while (*s && isspace((unsigned char)*s)) s++;
    char *end = s + strlen(s) - 1;
    while (end > s && isspace((unsigned char)*end)) *end-- = 0;
    return s;
}

static char *unquote(const char *s)
{
    if (!s) return axctl_strdup("");
    size_t len = strlen(s);
    if (len >= 2 && s[0] == '"' && s[len-1] == '"') {
        char *r = axctl_strdup(s + 1);
        r[len - 2] = 0;
        return r;
    }
    return axctl_strdup(s);
}

static void kv_add(axctl_toml_config_t *cfg, const char *section,
                   const char *key, const char *value)
{
    toml_kv_t *kv = calloc(1, sizeof(toml_kv_t));
    kv->section = axctl_strdup(section ? section : "");
    kv->key = axctl_strdup(key);
    kv->value = unquote(value);
    kv->next = NULL;

    if (cfg->tail) {
        cfg->tail->next = kv;
        cfg->tail = kv;
    } else {
        cfg->head = cfg->tail = kv;
    }
}

static const char *kv_get(axctl_toml_config_t *cfg, const char *section,
                          const char *key)
{
    for (toml_kv_t *kv = cfg->head; kv; kv = kv->next) {
        if (strcmp(kv->section, section) == 0 && strcmp(kv->key, key) == 0)
            return kv->value;
    }
    return NULL;
}

static int kv_get_int(axctl_toml_config_t *cfg, const char *section,
                      const char *key, int def)
{
    const char *v = kv_get(cfg, section, key);
    return v ? atoi(v) : def;
}

static double kv_get_float(axctl_toml_config_t *cfg, const char *section,
                           const char *key, double def)
{
    const char *v = kv_get(cfg, section, key);
    return v ? atof(v) : def;
}

static int kv_get_bool(axctl_toml_config_t *cfg, const char *section,
                       const char *key, int def)
{
    const char *v = kv_get(cfg, section, key);
    if (!v) return def;
    return (strcmp(v, "true") == 0 || strcmp(v, "1") == 0 ||
            strcmp(v, "yes") == 0);
}

/* Parse a single TOML file into the config store */
static int parse_toml_file(axctl_toml_config_t *cfg, const char *path,
                           int depth)
{
    if (depth > MAX_IMPORT_DEPTH) {
        LOG_ERROR("Max import depth exceeded at %s", path);
        return -1;
    }

    FILE *fp = fopen(path, "r");
    if (!fp) {
        LOG_ERROR("Cannot open config: %s", path);
        return -1;
    }

    char line[MAX_LINE];
    char section[256] = "";
    int in_array_table = 0;
    int array_table_idx = 0;

    while (fgets(line, sizeof(line), fp)) {
        char *t = trim(line);

        /* Skip empty lines and comments */
        if (!*t || *t == '#') continue;

        /* Array of tables: [[section]] */
        if (t[0] == '[' && t[1] == '[') {
            char *end = strstr(t + 2, "]]");
            if (end) {
                *end = 0;
                char *sec_name = trim(t + 2);
                snprintf(section, sizeof(section), "%s.%d",
                         sec_name, array_table_idx++);
                in_array_table = 1;
                continue;
            }
        }

        /* Section header: [section] */
        if (t[0] == '[') {
            char *end = strchr(t + 1, ']');
            if (end) {
                *end = 0;
                char *sec_name = trim(t + 1);
                strncpy(section, sec_name, sizeof(section) - 1);
                in_array_table = 0;
                array_table_idx = 0;
                continue;
            }
        }

        /* Key = value */
        char *eq = strchr(t, '=');
        if (eq) {
            *eq = 0;
            char *key = trim(t);
            char *val = trim(eq + 1);

            /* Handle include directive */
            if (strcmp(key, "include") == 0 && section[0] == 0) {
                /* Parse array: ["file1.toml", "file2.toml"] */
                if (val[0] == '[') {
                    char *p = val + 1;
                    while (*p && *p != ']') {
                        while (*p && (*p == ' ' || *p == ',')) p++;
                        if (*p == '"') {
                            p++;
                            char *end_q = strchr(p, '"');
                            if (end_q) {
                                *end_q = 0;
                                if (cfg->include_count < MAX_INCLUDES) {
                                    /* Resolve relative to config dir */
                                    char *dir = axctl_strdup(path);
                                    char *slash = strrchr(dir, '/');
                                    if (slash) *slash = 0;
                                    char *inc_path = axctl_sprintf("%s/%s", dir, p);
                                    cfg->includes[cfg->include_count++] = inc_path;
                                    free(dir);
                                }
                                p = end_q + 1;
                            }
                        } else {
                            break;
                        }
                    }
                }
                continue;
            }

            kv_add(cfg, section, key, val);
        }
    }

    fclose(fp);

    /* Process includes */
    for (int i = 0; i < cfg->include_count; i++) {
        if (cfg->includes[i]) {
            parse_toml_file(cfg, cfg->includes[i], depth + 1);
        }
    }

    return 0;
}

/* ------------------------------------------------------------------ */
/* Public API: Config loading                                          */
/* ------------------------------------------------------------------ */

char *axctl_default_config_path(void)
{
    /* Check $XDG_CONFIG_HOME/axctl/config.toml */
    const char *xdg = getenv("XDG_CONFIG_HOME");
    char *primary;
    if (xdg && *xdg) {
        primary = axctl_sprintf("%s/axctl/config.toml", xdg);
    } else {
        const char *home = getenv("HOME");
        if (!home) home = "/root";
        primary = axctl_sprintf("%s/.config/axctl/config.toml", home);
    }

    struct stat st;
    if (stat(primary, &st) == 0) {
        return primary;
    }

    /* Fallback */
    free(primary);
    const char *home = getenv("HOME");
    if (!home) home = "/root";
    return axctl_sprintf("%s/.local/share/ambxst/axctl.toml", home);
}

axctl_toml_config_t *axctl_load_config(const char *path)
{
    axctl_toml_config_t *cfg = calloc(1, sizeof(*cfg));
    if (!cfg) return NULL;

    if (parse_toml_file(cfg, path, 0) != 0) {
        axctl_free_config(cfg);
        return NULL;
    }

    return cfg;
}

void axctl_free_config(axctl_toml_config_t *cfg)
{
    if (!cfg) return;
    toml_kv_t *kv = cfg->head;
    while (kv) {
        toml_kv_t *next = kv->next;
        free(kv->section);
        free(kv->key);
        free(kv->value);
        free(kv);
        kv = next;
    }
    for (int i = 0; i < cfg->include_count; i++)
        free(cfg->includes[i]);
    free(cfg);
}

/* ------------------------------------------------------------------ */
/* Convert TOML config to JSON for Config.Apply                        */
/* ------------------------------------------------------------------ */

char *axctl_config_to_json(axctl_toml_config_t *cfg)
{
    json_object *root = json_object_new_object();

    /* Appearance */
    json_object *appearance = json_object_new_object();

    const char *v;
    if ((v = kv_get(cfg, "appearance.gaps", "inner")))
        json_object_object_add(appearance, "gaps",
            ({json_object *g = json_object_new_object();
              json_object_object_add(g, "inner", json_object_new_int(atoi(v)));
              const char *vo = kv_get(cfg, "appearance.gaps", "outer");
              if (vo) json_object_object_add(g, "outer", json_object_new_int(atoi(vo)));
              g;}));

    if ((v = kv_get(cfg, "appearance.border", "width")) ||
        (v = kv_get(cfg, "appearance.border", "active_color"))) {
        json_object *b = json_object_new_object();
        const char *bw = kv_get(cfg, "appearance.border", "width");
        const char *ac = kv_get(cfg, "appearance.border", "active_color");
        const char *ic = kv_get(cfg, "appearance.border", "inactive_color");
        const char *br = kv_get(cfg, "appearance.border", "rounding");
        if (bw) json_object_object_add(b, "width", json_object_new_int(atoi(bw)));
        if (ac) json_object_object_add(b, "active_color", json_object_new_string(ac));
        if (ic) json_object_object_add(b, "inactive_color", json_object_new_string(ic));
        if (br) json_object_object_add(b, "rounding", json_object_new_int(atoi(br)));
        json_object_object_add(appearance, "border", b);
    }

    if ((v = kv_get(cfg, "appearance.blur", "enabled"))) {
        json_object *bl = json_object_new_object();
        json_object_object_add(bl, "enabled",
            json_object_new_boolean(strcmp(v, "true") == 0));
        const char *bs = kv_get(cfg, "appearance.blur", "size");
        const char *bp = kv_get(cfg, "appearance.blur", "passes");
        if (bs) json_object_object_add(bl, "size", json_object_new_int(atoi(bs)));
        if (bp) json_object_object_add(bl, "passes", json_object_new_int(atoi(bp)));
        json_object_object_add(appearance, "blur", bl);
    }

    json_object_object_add(root, "appearance", appearance);

    /* Keybinds -- collect from keybinds.N sections */
    json_object *kb_custom = json_object_new_array();
    for (toml_kv_t *kv = cfg->head; kv; kv = kv->next) {
        if (strncmp(kv->section, "keybinds.", 9) == 0 &&
            strcmp(kv->key, "key") == 0) {
            /* Found a keybind entry; gather its fields */
            json_object *bind = json_object_new_object();
            json_object_object_add(bind, "key",
                json_object_new_string(kv->value));

            const char *disp = kv_get(cfg, kv->section, "dispatcher");
            const char *arg  = kv_get(cfg, kv->section, "argument");
            int enabled = kv_get_bool(cfg, kv->section, "enabled", 1);

            if (disp) json_object_object_add(bind, "dispatcher",
                json_object_new_string(disp));
            if (arg) json_object_object_add(bind, "argument",
                json_object_new_string(arg));
            json_object_object_add(bind, "enabled",
                json_object_new_boolean(enabled));

            json_object_array_add(kb_custom, bind);
        }
    }
    if (json_object_array_length(kb_custom) > 0) {
        json_object *keybinds = json_object_new_object();
        json_object_object_add(keybinds, "custom", kb_custom);
        json_object_object_add(root, "keybinds", keybinds);
    } else {
        json_object_put(kb_custom);
    }

    const char *json_str = json_object_to_json_string_ext(root,
        JSON_C_TO_STRING_PRETTY);
    char *result = axctl_strdup(json_str);
    json_object_put(root);
    return result;
}

/* ------------------------------------------------------------------ */
/* Apply config to compositor                                          */
/* ------------------------------------------------------------------ */

int axctl_apply_config(axctl_toml_config_t *cfg, axctl_compositor_t *comp)
{
    /* Convert TOML config to JSON, then parse into universal config */
    char *json = axctl_config_to_json(cfg);
    if (!json) return -1;

    LOG_INFO("Generating static config file");

    axctl_config_universal_t ucfg = {0};
    int rc = axctl_config_universal_from_json(json, &ucfg);
    free(json);
    if (rc != 0) return rc;

    rc = axctl_config_handler_apply(comp, &ucfg, NULL);
    axctl_config_universal_free(&ucfg);

    /* Apply keyboard layouts if configured */
    const char *kb_layouts = kv_get(cfg, "input.keyboard", "layouts");
    const char *kb_variants = kv_get(cfg, "input.keyboard", "variants");
    if (kb_layouts && *kb_layouts && comp->set_keyboard_layouts) {
        comp->set_keyboard_layouts(comp->priv, kb_layouts,
                                   kb_variants ? kb_variants : "");
    }

    /* Apply layout if configured */
    const char *layout = kv_get(cfg, "general", "layout");
    if (layout && *layout && comp->set_layout) {
        comp->set_layout(comp->priv, layout);
    }

    return rc;
}

/* ------------------------------------------------------------------ */
/* Config watcher (inotify-based hot reload)                           */
/* ------------------------------------------------------------------ */

static void *watcher_loop(void *arg)
{
    axctl_config_watcher_t *w = (axctl_config_watcher_t *)arg;
    char buf[4096] __attribute__((aligned(__alignof__(struct inotify_event))));

    while (w->running) {
        ssize_t len = read(w->inotify_fd, buf, sizeof(buf));
        if (len < 0) {
            if (errno == EINTR) continue;
            break;
        }

        /* Debounce: wait a bit for atomic saves */
        usleep(200000); /* 200ms */

        /* Drain any additional events */
        fd_set fds;
        struct timeval tv = {0, 0};
        FD_ZERO(&fds);
        FD_SET(w->inotify_fd, &fds);
        while (select(w->inotify_fd + 1, &fds, NULL, NULL, &tv) > 0) {
            read(w->inotify_fd, buf, sizeof(buf));
            FD_ZERO(&fds);
            FD_SET(w->inotify_fd, &fds);
            tv.tv_sec = 0; tv.tv_usec = 0;
        }

        /* Reload config */
        LOG_INFO("Config file changed, reloading...");
        axctl_toml_config_t *new_cfg = axctl_load_config(w->config_path);
        if (new_cfg && w->callback) {
            w->callback(new_cfg, w->userdata);
            axctl_free_config(new_cfg);
        } else if (!new_cfg) {
            LOG_ERROR("Error reloading config from %s", w->config_path);
        }
    }

    return NULL;
}

axctl_config_watcher_t *axctl_config_watcher_create(void)
{
    int ifd = inotify_init1(IN_CLOEXEC);
    if (ifd < 0) {
        LOG_ERROR("Failed to create inotify: %s", strerror(errno));
        return NULL;
    }

    axctl_config_watcher_t *w = calloc(1, sizeof(*w));
    w->inotify_fd = ifd;
    return w;
}

int axctl_config_watcher_start(axctl_config_watcher_t *w,
                               const char *config_path,
                               axctl_config_reload_cb_t callback,
                               void *userdata)
{
    if (!w) return -1;
    w->config_path = axctl_strdup(config_path);
    w->callback = callback;
    w->userdata = userdata;
    w->running = 1;

    /* Watch the config file */
    int wd = inotify_add_watch(w->inotify_fd, config_path,
                                IN_MODIFY | IN_CREATE | IN_MOVED_TO);
    if (wd >= 0) {
        w->watch_fds[w->watch_count++] = wd;
    } else {
        LOG_WARN("Cannot watch %s: %s", config_path, strerror(errno));
    }

    /* Start watcher thread */
    pthread_create(&w->thread, NULL, watcher_loop, w);
    pthread_detach(w->thread);

    return 0;
}

void axctl_config_watcher_stop(axctl_config_watcher_t *w)
{
    if (!w) return;
    w->running = 0;
    close(w->inotify_fd);
    free(w->config_path);
    free(w);
}
