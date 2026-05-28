/*
 * axctl - Universal configuration types
 *
 * These structures represent the compositor-agnostic configuration format.
 * Each compositor backend has a config generator that converts these
 * universal types into compositor-specific syntax.
 */
#ifndef AXCTL_IPC_CONFIG_TYPES_H
#define AXCTL_IPC_CONFIG_TYPES_H

#include <stdbool.h>
#include <stddef.h>
#include <json-c/json.h>

/* Optional integer/float/bool/string wrappers (NULL pointer means "not set") */
typedef struct { int value; }     opt_int_t;
typedef struct { double value; }  opt_double_t;
typedef struct { bool value; }    opt_bool_t;

/* ── Gaps config ────────────────────────────────────────────────────── */
typedef struct {
    opt_int_t *inner;
    opt_int_t *outer;
} axctl_gaps_t;

/* ── Border config ──────────────────────────────────────────────────── */
typedef struct {
    opt_int_t *width;
    char *active_color;     /* NULL if not set */
    char *inactive_color;   /* NULL if not set */
    opt_int_t *rounding;
} axctl_border_t;

/* ── Opacity config ─────────────────────────────────────────────────── */
typedef struct {
    opt_double_t *active;
    opt_double_t *inactive;
} axctl_opacity_t;

/* ── Blur config ────────────────────────────────────────────────────── */
typedef struct {
    opt_bool_t *enabled;
    opt_int_t *size;
    opt_int_t *passes;
} axctl_blur_t;

/* ── Shadow config ──────────────────────────────────────────────────── */
typedef struct {
    opt_bool_t *enabled;
    opt_int_t *size;
    char *color;            /* NULL if not set */
} axctl_shadow_t;

/* ── Animations config ──────────────────────────────────────────────── */
typedef struct {
    opt_bool_t *enabled;
} axctl_animations_t;

/* ── Appearance config ──────────────────────────────────────────────── */
typedef struct {
    axctl_gaps_t *gaps;
    axctl_border_t *border;
    axctl_opacity_t *opacity;
    axctl_blur_t *blur;
    axctl_shadow_t *shadow;
    axctl_animations_t *animations;
    char *layout;           /* NULL if not set */
} axctl_appearance_t;

/* ── Keybind ────────────────────────────────────────────────────────── */
typedef struct {
    char **modifiers;
    int modifier_count;
    char *key;
    char *dispatcher;
    char *argument;
    char *flags;
    bool enabled;
} axctl_keybind_t;

/* ── Keybind target (for unbinding) ─────────────────────────────────── */
typedef struct {
    char **modifiers;
    int modifier_count;
    char *key;
} axctl_keybind_target_t;

/* ── Window rule ────────────────────────────────────────────────────── */
typedef struct {
    /* Legacy single-line syntax */
    char *match;
    char *rule;
    char *action;
    char *name;

    /* Block syntax fields */
    opt_bool_t *is_float;
    opt_bool_t *no_blur;
    opt_bool_t *no_shadow;
    opt_int_t  *rounding;
    opt_int_t  *border_size;
    opt_bool_t *pin;
    opt_bool_t *fullscreen;
    opt_bool_t *idle_inhibit;
    opt_bool_t *no_screen_share;
    char *move;
    char *size;
} axctl_window_rule_t;

/* ── Layer rule ─────────────────────────────────────────────────────── */
typedef struct {
    opt_bool_t   *no_anim;
    opt_bool_t   *blur;
    opt_bool_t   *blur_popups;
    opt_bool_t   *ignore_alpha;
    opt_bool_t   *no_shadow;
    opt_bool_t   *ignore_zero_alpha;
    opt_double_t *ignore_alpha_value;
    char *namespace_str;
} axctl_layer_rule_t;

/* ── ConfigUniversal ────────────────────────────────────────────────── */
typedef struct {
    axctl_appearance_t appearance;

    /* Keybinds */
    axctl_keybind_t *custom_keybinds;
    int custom_keybind_count;

    /* Window rules */
    axctl_window_rule_t *window_rules;
    int window_rule_count;

    /* Layer rules */
    axctl_layer_rule_t *layer_rules;
    int layer_rule_count;

    /* Startup commands */
    char **exec;
    int exec_count;
    char **exec_once;
    int exec_once_count;
} axctl_config_universal_t;

/* ── Config Generator vtable ────────────────────────────────────────── */
typedef struct {
    char *(*generate_appearance)(const axctl_appearance_t *config);
    char *(*generate_keybinds)(const axctl_keybind_t *binds, int count);
    char *(*generate_window_rules)(const axctl_window_rule_t *rules, int count);
    char *(*generate_layer_rules)(const axctl_layer_rule_t *rules, int count);
    char *(*generate_startup)(char **exec, int exec_count,
                              char **exec_once, int exec_once_count);
} config_generator_vtable_t;

/* ── Lua config generator vtable (Hyprland-specific) ────────────────── */
typedef struct {
    char *(*generate_appearance_lua)(const axctl_appearance_t *config);
    char *(*generate_keybinds_lua)(const axctl_keybind_t *binds, int count);
    char *(*generate_window_rules_lua)(const axctl_window_rule_t *rules, int count);
    char *(*generate_layer_rules_lua)(const axctl_layer_rule_t *rules, int count);
    char *(*generate_startup_lua)(char **exec, int exec_count,
                                  char **exec_once, int exec_once_count);
} lua_config_generator_vtable_t;

/* Free functions */
void axctl_appearance_free(axctl_appearance_t *a);
void axctl_config_universal_free(axctl_config_universal_t *cfg);
void axctl_keybind_free(axctl_keybind_t *kb);
void axctl_window_rule_free(axctl_window_rule_t *r);
void axctl_layer_rule_free(axctl_layer_rule_t *r);

/* Parse universal config from JSON */
int axctl_config_universal_from_json(const char *json_str, axctl_config_universal_t *out);

/* Helper to create optional values */
opt_int_t *opt_int_new(int v);
opt_double_t *opt_double_new(double v);
opt_bool_t *opt_bool_new(bool v);

#endif /* AXCTL_IPC_CONFIG_TYPES_H */
