/* test_user_binds.c - Validates the user's binds.json through C keybind parsing
 *
 * Parses the user's actual binds.json (real Ambxst format with keys[] and actions[]),
 * checks for dangerous or malformed binds.
 *
 * Build: gcc -o test_user_binds test_user_binds.c -ljson-c
 * Usage: ./test_user_binds /path/to/binds.json
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <json-c/json.h>
#include <ctype.h>

static bool is_safe_without_mod(const char *key) {
    if (!key) return false;
    if (strncmp(key, "XF86", 4) == 0) return true;
    if (strncmp(key, "switch:", 7) == 0) return true;
    if (strcmp(key, "Print") == 0) return true;
    return false;
}

static bool is_mouse_key(const char *key) {
    return key && strncasecmp(key, "mouse:", 6) == 0;
}

int main(int argc, char **argv) {
    const char *path = (argc > 1) ? argv[1] : "binds.json";

    FILE *fp = fopen(path, "r");
    if (!fp) { fprintf(stderr, "Cannot open %s\n", path); return 1; }
    fseek(fp, 0, SEEK_END);
    long len = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    char *buf = malloc(len + 1);
    fread(buf, 1, len, fp);
    buf[len] = '\0';
    fclose(fp);

    struct json_object *root = json_tokener_parse(buf);
    free(buf);
    if (!root) { fprintf(stderr, "Failed to parse JSON\n"); return 1; }

    struct json_object *custom;
    if (!json_object_object_get_ex(root, "custom", &custom)) {
        fprintf(stderr, "No 'custom' key found\n");
        json_object_put(root);
        return 1;
    }

    int total = json_object_array_length(custom);
    int passed = 0, warnings = 0, errors = 0, skipped = 0;

    printf("=== User Binds Validation ===\n");
    printf("File: %s\n", path);
    printf("Total custom bind entries: %d\n\n", total);

    for (int i = 0; i < total; i++) {
        struct json_object *bind = json_object_array_get_idx(custom, i);

        /* Get enabled */
        struct json_object *j_enabled, *j_name;
        json_object_object_get_ex(bind, "enabled", &j_enabled);
        json_object_object_get_ex(bind, "name", &j_name);
        bool enabled = j_enabled ? json_object_get_boolean(j_enabled) : true;
        const char *name = j_name ? json_object_get_string(j_name) : "(unnamed)";

        if (!enabled) { skipped++; continue; }

        /* Get keys[] array */
        struct json_object *j_keys;
        json_object_object_get_ex(bind, "keys", &j_keys);
        if (!j_keys || json_object_array_length(j_keys) == 0) {
            printf("[WARN]  Bind %d '%s': no keys defined\n", i, name);
            warnings++;
            continue;
        }

        /* Get actions[] array */
        struct json_object *j_actions;
        json_object_object_get_ex(bind, "actions", &j_actions);
        if (!j_actions || json_object_array_length(j_actions) == 0) {
            printf("[WARN]  Bind %d '%s': no actions defined\n", i, name);
            warnings++;
            continue;
        }

        /* Check each key entry */
        int n_keys = json_object_array_length(j_keys);
        for (int k = 0; k < n_keys; k++) {
            struct json_object *key_obj = json_object_array_get_idx(j_keys, k);
            struct json_object *j_key, *j_mods;
            json_object_object_get_ex(key_obj, "key", &j_key);
            json_object_object_get_ex(key_obj, "modifiers", &j_mods);

            const char *key = j_key ? json_object_get_string(j_key) : "";
            int mod_count = j_mods ? json_object_array_length(j_mods) : 0;

            if (!key || !*key) {
                printf("[WARN]  Bind %d '%s' key[%d]: empty key\n", i, name, k);
                warnings++;
                continue;
            }

            /* Build mod string */
            char mods_str[256] = "";
            for (int j = 0; j < mod_count; j++) {
                if (j > 0) strcat(mods_str, " ");
                const char *m = json_object_get_string(json_object_array_get_idx(j_mods, j));
                strncat(mods_str, m, sizeof(mods_str) - strlen(mods_str) - 1);
            }

            /* Check: mouse without modifier */
            if (is_mouse_key(key) && mod_count == 0) {
                printf("[ERROR] Bind %d '%s': mouse key '%s' has NO modifiers!\n", i, name, key);
                errors++;
                continue;
            }

            /* Check: normal key without modifier */
            if (mod_count == 0 && !is_safe_without_mod(key)) {
                printf("[ERROR] Bind %d '%s': key='%s' has NO modifiers!\n", i, name, key);
                errors++;
                continue;
            }

            /* Check action IDs */
            int n_actions = json_object_array_length(j_actions);
            for (int a = 0; a < n_actions; a++) {
                struct json_object *act = json_object_array_get_idx(j_actions, a);
                struct json_object *j_id;
                json_object_object_get_ex(act, "id", &j_id);
                const char *aid = j_id ? json_object_get_string(j_id) : "";
                /* Just log what we'd generate */
                printf("[OK]    Bind %d '%s': [%s] %s → action '%s'\n",
                       i, name, mods_str, key, aid);
            }
            passed++;
        }
    }

    printf("\n=== Results ===\n");
    printf("Passed:   %d key-action combos\n", passed);
    printf("Warnings: %d\n", warnings);
    printf("Errors:   %d\n", errors);
    printf("Skipped:  %d (disabled)\n", skipped);
    printf("Total entries: %d\n", total);

    if (errors == 0) {
        printf("\n✅ ALL BINDS VALID\n");
    } else {
        printf("\n❌ %d ERROR(S) FOUND\n", errors);
    }

    json_object_put(root);
    return errors > 0 ? 1 : 0;
}
