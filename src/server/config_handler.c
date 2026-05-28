/* src/server/config_handler.c -- Config handler
 *
 * Generates compositor-specific config files from a universal
 * axctl_config_universal_t, then triggers a reload.
 *
 * Supports Hyprland (.conf + .lua), Niri, and Mango generators.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "server/config_handler.h"
#include "ipc/compositor.h"
#include "ipc/config_types.h"
#include "ipc/hyprland/hyprland.h"
#include "ipc/niri/niri.h"
#include "ipc/mango/mango.h"
#include "ipc/errors.h"
#include "utils/log.h"
#include "utils/strutil.h"

/* ------------------------------------------------------------------ */
/* Internal types and helpers                                          */
/* ------------------------------------------------------------------ */

/* Function pointer types for config generators */
typedef char *(*gen_appearance_fn)(const axctl_appearance_t *);
typedef char *(*gen_keybinds_fn)(const axctl_keybind_t *, int count);
typedef char *(*gen_window_rules_fn)(const axctl_window_rule_t *, int);
typedef char *(*gen_layer_rules_fn)(const axctl_layer_rule_t *, int);
typedef char *(*gen_startup_fn)(char **, int, char **, int);

typedef struct {
    gen_appearance_fn    appearance;
    gen_keybinds_fn      keybinds;
    gen_window_rules_fn  window_rules;
    gen_layer_rules_fn   layer_rules;
    gen_startup_fn       startup;
    /* Lua generators (Hyprland only) */
    gen_appearance_fn    lua_appearance;
    gen_keybinds_fn      lua_keybinds;
    gen_window_rules_fn  lua_window_rules;
    gen_layer_rules_fn   lua_layer_rules;
    gen_startup_fn       lua_startup;
} generator_set_t;

char *axctl_config_default_output_path(void)
{
    const char *home = getenv("HOME");
    if (!home) home = "/root";
    return axctl_sprintf("%s/.local/share/axctl/generated.conf", home);
}

/* Ensure directory exists for a file path */
static void ensure_dir(const char *path)
{
    char *dir = axctl_strdup(path);
    char *slash = strrchr(dir, '/');
    if (slash) {
        *slash = '\0';
        char cmd[1024];
        snprintf(cmd, sizeof(cmd), "mkdir -p '%s'", dir);
        system(cmd);
    }
    free(dir);
}

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */

int axctl_config_handler_apply(axctl_compositor_t *comp,
                               const axctl_config_universal_t *cfg,
                               const char *output_path)
{
    if (!comp || !cfg) {
        axctl_set_error("invalid arguments");
        return -1;
    }

    /* Determine which generators to use based on compositor type */
    generator_set_t gen = {0};
    const char *comp_name = comp->name ? comp->name : "";

    if (strcmp(comp_name, "Hyprland") == 0) {
        gen.appearance   = hyprland_generate_appearance;
        gen.keybinds     = hyprland_generate_keybinds;
        gen.window_rules = hyprland_generate_window_rules;
        gen.layer_rules  = hyprland_generate_layer_rules;
        gen.startup      = hyprland_generate_startup;
        gen.lua_appearance   = hyprland_generate_appearance_lua;
        gen.lua_keybinds     = hyprland_generate_keybinds_lua;
        gen.lua_window_rules = hyprland_generate_window_rules_lua;
        gen.lua_layer_rules  = hyprland_generate_layer_rules_lua;
        gen.lua_startup      = hyprland_generate_startup_lua;
    } else if (strcmp(comp_name, "Niri") == 0) {
        gen.appearance   = niri_generate_appearance;
        gen.keybinds     = niri_generate_keybinds;
        gen.window_rules = niri_generate_window_rules;
        gen.layer_rules  = niri_generate_layer_rules;
        gen.startup      = niri_generate_startup;
    } else if (strcmp(comp_name, "Mango") == 0) {
        gen.appearance   = mango_generate_appearance;
        gen.keybinds     = mango_generate_keybinds;
        gen.window_rules = mango_generate_window_rules;
        gen.layer_rules  = mango_generate_layer_rules;
        gen.startup      = mango_generate_startup;
    } else {
        axctl_set_error("config generator not supported for compositor: %s", comp_name);
        return -1;
    }

    /* Generate config sections.
     * NOTE: Keybinds are NOT generated from the TOML config.
     * The QML handles keybinds exclusively via axctl config keybinds-batch,
     * which uses hyprctl keyword to apply/remove keybinds at runtime.
     * If we also wrote keybinds to the config file, Hyprland would load
     * them on every reload, causing duplicate binds that fire 2x-4x per
     * keypress (the keybinds-batch would add runtime copies on top of
     * file copies, and each reload would compound them). */
    char *s_app     = gen.appearance   ? gen.appearance(&cfg->appearance) : axctl_strdup("");
    char *s_bind    = axctl_strdup("");  /* keybinds skipped — handled by keybinds-batch */
    char *s_rules   = gen.window_rules ? gen.window_rules(cfg->window_rules, cfg->window_rule_count) : axctl_strdup("");
    char *s_layers  = gen.layer_rules  ? gen.layer_rules(cfg->layer_rules, cfg->layer_rule_count) : axctl_strdup("");
    char *s_startup = gen.startup      ? gen.startup(cfg->exec, cfg->exec_count, cfg->exec_once, cfg->exec_once_count) : axctl_strdup("");

    /* Determine output path */
    char *config_path = output_path ? axctl_strdup(output_path) : axctl_config_default_output_path();
    ensure_dir(config_path);

    /* Write combined config */
    FILE *fp = fopen(config_path, "w");
    if (!fp) {
        axctl_set_error("failed to write config to %s", config_path);
        free(config_path);
        free(s_app); free(s_bind); free(s_rules); free(s_layers); free(s_startup);
        return -1;
    }

    fprintf(fp, "# Generated by axctl - do not edit manually\n\n");
    if (s_startup && *s_startup) fprintf(fp, "%s\n", s_startup);
    if (s_app     && *s_app)     fprintf(fp, "%s\n", s_app);
    if (s_bind    && *s_bind)    fprintf(fp, "%s\n", s_bind);
    if (s_rules   && *s_rules)   fprintf(fp, "%s\n", s_rules);
    if (s_layers  && *s_layers)  fprintf(fp, "%s\n", s_layers);
    fclose(fp);

    LOG_INFO("Config written to: %s", config_path);

    /* Generate Lua config if available (Hyprland only) */
    if (gen.lua_appearance) {
        char *lua_path = axctl_strdup(config_path);
        char *dot = strrchr(lua_path, '.');
        if (dot) *dot = '\0';
        char *lua_full = axctl_sprintf("%s.lua", lua_path);
        free(lua_path);

        char *l_app     = gen.lua_appearance(&cfg->appearance);
        char *l_bind    = axctl_strdup("");  /* keybinds skipped — handled by keybinds-batch */
        char *l_rules   = gen.lua_window_rules ? gen.lua_window_rules(cfg->window_rules, cfg->window_rule_count) : axctl_strdup("");
        char *l_layers  = gen.lua_layer_rules  ? gen.lua_layer_rules(cfg->layer_rules, cfg->layer_rule_count) : axctl_strdup("");
        char *l_startup = gen.lua_startup      ? gen.lua_startup(cfg->exec, cfg->exec_count, cfg->exec_once, cfg->exec_once_count) : axctl_strdup("");

        fp = fopen(lua_full, "w");
        if (fp) {
            fprintf(fp, "-- Generated by axctl - do not edit manually\n\n");
            if (l_startup && *l_startup) fprintf(fp, "%s\n", l_startup);
            if (l_app     && *l_app)     fprintf(fp, "%s\n", l_app);
            if (l_bind    && *l_bind)    fprintf(fp, "%s\n", l_bind);
            if (l_rules   && *l_rules)   fprintf(fp, "%s\n", l_rules);
            if (l_layers  && *l_layers)  fprintf(fp, "%s\n", l_layers);
            fclose(fp);
            LOG_INFO("Lua config written to: %s", lua_full);
        }

        free(lua_full);
        free(l_app); free(l_bind); free(l_rules); free(l_layers); free(l_startup);
    }

    /* Cleanup */
    free(config_path);
    free(s_app); free(s_bind); free(s_rules); free(s_layers); free(s_startup);

    /* Reload is SKIPPED — the config watcher auto-triggers this on TOML
     * changes, but hyprctl reload would wipe ALL runtime keybinds
     * applied by keybinds-batch. The QML calls axctl config reload
     * explicitly when needed. */
    // reload skipped: would wipe runtime keybinds from keybinds-batch
    // QML handles explicit reloads via axctl config reload

    return AXCTL_OK;
}
