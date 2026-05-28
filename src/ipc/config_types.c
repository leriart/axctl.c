/*
 * axctl - Config types and universal config parsing
 */
#include "ipc/config_types.h"
#include "ipc/errors.h"
#include "utils/strutil.h"
#include "utils/json_helpers.h"
#include <stdlib.h>
#include <string.h>

opt_int_t *opt_int_new(int v) {
    opt_int_t *p = malloc(sizeof(opt_int_t));
    if (p) p->value = v;
    return p;
}
opt_double_t *opt_double_new(double v) {
    opt_double_t *p = malloc(sizeof(opt_double_t));
    if (p) p->value = v;
    return p;
}
opt_bool_t *opt_bool_new(bool v) {
    opt_bool_t *p = malloc(sizeof(opt_bool_t));
    if (p) p->value = v;
    return p;
}

void axctl_appearance_free(axctl_appearance_t *a) {
    if (!a) return;
    if (a->gaps) { free(a->gaps->inner); free(a->gaps->outer); free(a->gaps); }
    if (a->border) {
        free(a->border->width); free(a->border->active_color);
        free(a->border->inactive_color); free(a->border->rounding); free(a->border);
    }
    if (a->opacity) { free(a->opacity->active); free(a->opacity->inactive); free(a->opacity); }
    if (a->blur) { free(a->blur->enabled); free(a->blur->size); free(a->blur->passes); free(a->blur); }
    if (a->shadow) { free(a->shadow->enabled); free(a->shadow->size); free(a->shadow->color); free(a->shadow); }
    if (a->animations) { free(a->animations->enabled); free(a->animations); }
    free(a->layout);
    memset(a, 0, sizeof(*a));
}

void axctl_keybind_free(axctl_keybind_t *kb) {
    if (!kb) return;
    for (int i = 0; i < kb->modifier_count; i++) free(kb->modifiers[i]);
    free(kb->modifiers);
    free(kb->key); free(kb->dispatcher); free(kb->argument); free(kb->flags);
    memset(kb, 0, sizeof(*kb));
}

void axctl_window_rule_free(axctl_window_rule_t *r) {
    if (!r) return;
    free(r->match); free(r->rule); free(r->action); free(r->name);
    free(r->is_float); free(r->no_blur); free(r->no_shadow);
    free(r->rounding); free(r->border_size); free(r->pin);
    free(r->fullscreen); free(r->idle_inhibit); free(r->no_screen_share);
    free(r->move); free(r->size);
    memset(r, 0, sizeof(*r));
}

void axctl_layer_rule_free(axctl_layer_rule_t *r) {
    if (!r) return;
    free(r->no_anim); free(r->blur); free(r->blur_popups);
    free(r->ignore_alpha); free(r->no_shadow); free(r->ignore_zero_alpha);
    free(r->ignore_alpha_value); free(r->namespace_str);
    memset(r, 0, sizeof(*r));
}

void axctl_config_universal_free(axctl_config_universal_t *cfg) {
    if (!cfg) return;
    axctl_appearance_free(&cfg->appearance);
    for (int i = 0; i < cfg->custom_keybind_count; i++) axctl_keybind_free(&cfg->custom_keybinds[i]);
    free(cfg->custom_keybinds);
    for (int i = 0; i < cfg->ambxst_system_keybind_count; i++) axctl_keybind_free(&cfg->ambxst_system_keybinds[i]);
    free(cfg->ambxst_system_keybinds);
    for (int i = 0; i < cfg->ambxst_bind_count; i++) axctl_keybind_free(&cfg->ambxst_binds[i]);
    free(cfg->ambxst_binds);
    for (int i = 0; i < cfg->window_rule_count; i++) axctl_window_rule_free(&cfg->window_rules[i]);
    free(cfg->window_rules);
    for (int i = 0; i < cfg->layer_rule_count; i++) axctl_layer_rule_free(&cfg->layer_rules[i]);
    free(cfg->layer_rules);
    for (int i = 0; i < cfg->exec_count; i++) free(cfg->exec[i]);
    free(cfg->exec);
    for (int i = 0; i < cfg->exec_once_count; i++) free(cfg->exec_once[i]);
    free(cfg->exec_once);
    memset(cfg, 0, sizeof(*cfg));
}

/* Parse appearance from JSON, handling both nested and flat dot-notation keys */
static void parse_appearance(struct json_object *obj, axctl_appearance_t *app) {
    struct json_object *gaps_obj = json_get_object(obj, "gaps");
    if (gaps_obj) {
        app->gaps = calloc(1, sizeof(axctl_gaps_t));
        struct json_object *v;
        if (json_object_object_get_ex(gaps_obj, "inner", &v))
            app->gaps->inner = opt_int_new(json_object_get_int(v));
        if (json_object_object_get_ex(gaps_obj, "outer", &v))
            app->gaps->outer = opt_int_new(json_object_get_int(v));
    }
    struct json_object *border_obj = json_get_object(obj, "border");
    if (border_obj) {
        app->border = calloc(1, sizeof(axctl_border_t));
        struct json_object *v;
        if (json_object_object_get_ex(border_obj, "width", &v))
            app->border->width = opt_int_new(json_object_get_int(v));
        if (json_object_object_get_ex(border_obj, "active_color", &v))
            app->border->active_color = axctl_strdup(json_object_get_string(v));
        if (json_object_object_get_ex(border_obj, "inactive_color", &v))
            app->border->inactive_color = axctl_strdup(json_object_get_string(v));
        if (json_object_object_get_ex(border_obj, "rounding", &v))
            app->border->rounding = opt_int_new(json_object_get_int(v));
    }
    struct json_object *opacity_obj = json_get_object(obj, "opacity");
    if (opacity_obj) {
        app->opacity = calloc(1, sizeof(axctl_opacity_t));
        struct json_object *v;
        if (json_object_object_get_ex(opacity_obj, "active", &v))
            app->opacity->active = opt_double_new(json_object_get_double(v));
        if (json_object_object_get_ex(opacity_obj, "inactive", &v))
            app->opacity->inactive = opt_double_new(json_object_get_double(v));
    }
    struct json_object *blur_obj = json_get_object(obj, "blur");
    if (blur_obj) {
        app->blur = calloc(1, sizeof(axctl_blur_t));
        struct json_object *v;
        if (json_object_object_get_ex(blur_obj, "enabled", &v))
            app->blur->enabled = opt_bool_new(json_object_get_boolean(v));
        if (json_object_object_get_ex(blur_obj, "size", &v))
            app->blur->size = opt_int_new(json_object_get_int(v));
        if (json_object_object_get_ex(blur_obj, "passes", &v))
            app->blur->passes = opt_int_new(json_object_get_int(v));
    }
    struct json_object *shadow_obj = json_get_object(obj, "shadow");
    if (shadow_obj) {
        app->shadow = calloc(1, sizeof(axctl_shadow_t));
        struct json_object *v;
        if (json_object_object_get_ex(shadow_obj, "enabled", &v))
            app->shadow->enabled = opt_bool_new(json_object_get_boolean(v));
        if (json_object_object_get_ex(shadow_obj, "size", &v))
            app->shadow->size = opt_int_new(json_object_get_int(v));
        if (json_object_object_get_ex(shadow_obj, "color", &v))
            app->shadow->color = axctl_strdup(json_object_get_string(v));
    }
    struct json_object *anim_obj = json_get_object(obj, "animations");
    if (anim_obj) {
        app->animations = calloc(1, sizeof(axctl_animations_t));
        struct json_object *v;
        if (json_object_object_get_ex(anim_obj, "enabled", &v))
            app->animations->enabled = opt_bool_new(json_object_get_boolean(v));
    }
    const char *layout = json_get_string(obj, "layout");
    if (layout && *layout) app->layout = axctl_strdup(layout);

    /* Handle flat dot-notation keys (e.g. "gaps.inner") */
    json_object_object_foreach(obj, key, val) {
        if (!strchr(key, '.')) continue;
        char **parts; int n = axctl_strsplit(key, '.', &parts);
        if (n == 2) {
            if (strcmp(parts[0], "gaps") == 0) {
                if (!app->gaps) app->gaps = calloc(1, sizeof(axctl_gaps_t));
                if (strcmp(parts[1], "inner") == 0 && json_object_get_type(val) == json_type_int)
                    { free(app->gaps->inner); app->gaps->inner = opt_int_new(json_object_get_int(val)); }
                else if (strcmp(parts[1], "outer") == 0 && json_object_get_type(val) == json_type_int)
                    { free(app->gaps->outer); app->gaps->outer = opt_int_new(json_object_get_int(val)); }
            } else if (strcmp(parts[0], "border") == 0) {
                if (!app->border) app->border = calloc(1, sizeof(axctl_border_t));
                if (strcmp(parts[1], "width") == 0 && json_object_get_type(val) == json_type_int)
                    { free(app->border->width); app->border->width = opt_int_new(json_object_get_int(val)); }
                else if (strcmp(parts[1], "active_color") == 0 && json_object_get_type(val) == json_type_string)
                    { free(app->border->active_color); app->border->active_color = axctl_strdup(json_object_get_string(val)); }
                else if (strcmp(parts[1], "inactive_color") == 0 && json_object_get_type(val) == json_type_string)
                    { free(app->border->inactive_color); app->border->inactive_color = axctl_strdup(json_object_get_string(val)); }
            } else if (strcmp(parts[0], "opacity") == 0) {
                if (!app->opacity) app->opacity = calloc(1, sizeof(axctl_opacity_t));
                if (strcmp(parts[1], "active") == 0)
                    { free(app->opacity->active); app->opacity->active = opt_double_new(json_object_get_double(val)); }
                else if (strcmp(parts[1], "inactive") == 0)
                    { free(app->opacity->inactive); app->opacity->inactive = opt_double_new(json_object_get_double(val)); }
            }
        }
        axctl_strsplit_free(parts, n);
    }
}

/* Parse a single keybind from a JSON object into an axctl_keybind_t */
static void parse_keybind(struct json_object *kb, axctl_keybind_t *k) {
    k->key = axctl_strdup(json_get_string(kb, "key"));
    k->dispatcher = axctl_strdup(json_get_string(kb, "dispatcher"));
    k->argument = axctl_strdup(json_get_string(kb, "argument"));
    k->flags = axctl_strdup(json_get_string(kb, "flags"));
    k->enabled = json_get_bool(kb, "enabled", true);
    struct json_object *mods = json_get_array(kb, "modifiers");
    if (mods) {
        int mlen = json_object_array_length(mods);
        k->modifiers = calloc(mlen + 1, sizeof(char *));
        k->modifier_count = mlen;
        for (int j = 0; j < mlen; j++)
            k->modifiers[j] = axctl_strdup(json_object_get_string(
                json_object_array_get_idx(mods, j)));
    }
}

int axctl_config_universal_from_json(const char *json_str, axctl_config_universal_t *out) {
    memset(out, 0, sizeof(*out));
    struct json_object *root = json_tokener_parse(json_str);
    if (!root) return AXCTL_ERR_PARSE;

    struct json_object *app_obj = json_get_object(root, "appearance");
    if (app_obj) parse_appearance(app_obj, &out->appearance);

    /* Parse keybinds from JSON.
     * Go structure: { "keybinds": { "ambxst": { "system": { ... }, ... }, "custom": [...] } }
     * "ambxst.system" is a map of name→Keybind (system keybinds).
     * Other keys under "ambxst" (excluding "system") are also name→Keybind (named binds).
     * "custom" is an array of Keybind objects. */
    struct json_object *kb_obj = json_get_object(root, "keybinds");
    if (kb_obj) {
        /* Parse keybinds.ambxst */
        struct json_object *ambxst_obj = json_get_object(kb_obj, "ambxst");
        if (ambxst_obj) {
            /* Parse keybinds.ambxst.system (map of name→Keybind) */
            struct json_object *system_obj = json_get_object(ambxst_obj, "system");
            if (system_obj) {
                int sys_count = json_object_object_length(system_obj);
                if (sys_count > 0) {
                    out->ambxst_system_keybinds = calloc(sys_count, sizeof(axctl_keybind_t));
                    json_object_object_foreach(system_obj, sys_key, sys_val) {
                        (void)sys_key;
                        if (json_object_get_type(sys_val) == json_type_object) {
                            parse_keybind(sys_val,
                                &out->ambxst_system_keybinds[out->ambxst_system_keybind_count++]);
                        }
                    }
                }
            }

            /* Parse other keys under ambxst (excluding "system") as named binds */
            int other_count = 0;
            json_object_object_foreach(ambxst_obj, ak, av) {
                if (strcmp(ak, "system") != 0 && json_object_get_type(av) == json_type_object)
                    other_count++;
            }
            if (other_count > 0) {
                out->ambxst_binds = calloc(other_count, sizeof(axctl_keybind_t));
                json_object_object_foreach(ambxst_obj, bk, bv) {
                    if (strcmp(bk, "system") != 0 && json_object_get_type(bv) == json_type_object) {
                        parse_keybind(bv,
                            &out->ambxst_binds[out->ambxst_bind_count++]);
                    }
                }
            }
        }

        /* Parse keybinds.custom (array of Keybind) */
        struct json_object *kb_custom = json_get_array(kb_obj, "custom");
        if (kb_custom) {
            int count = json_object_array_length(kb_custom);
            if (count > 0) {
                out->custom_keybinds = calloc(count, sizeof(axctl_keybind_t));
                for (int i = 0; i < count; i++) {
                    struct json_object *kb = json_object_array_get_idx(kb_custom, i);
                    parse_keybind(kb, &out->custom_keybinds[out->custom_keybind_count++]);
                }
            }
        }
    }

    json_object_put(root);
    return AXCTL_OK;
}
