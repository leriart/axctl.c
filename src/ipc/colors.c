/*
 * axctl - Color format conversion
 */
#include "ipc/colors.h"
#include "utils/strutil.h"
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdio.h>

char *axctl_first_color(const char *color_str) {
    if (!color_str || !*color_str) return axctl_strdup("");
    /* Return the first space-delimited token */
    const char *end = color_str;
    while (*end && !isspace((unsigned char)*end)) end++;
    size_t len = (size_t)(end - color_str);
    char *out = malloc(len + 1);
    if (!out) return NULL;
    memcpy(out, color_str, len);
    out[len] = '\0';
    return out;
}

char *axctl_mango_color(const char *color_str) {
    if (!color_str || !*color_str) return axctl_strdup("");

    /* Get first token */
    char *token = axctl_first_color(color_str);
    if (!token || !*token) { free(token); return axctl_strdup(""); }

    char *c = token;
    /* Remove rgba()/rgb() wrapper - extract hex inside */
    if (axctl_starts_with(c, "rgba(") || axctl_starts_with(c, "RGBA(") ||
        axctl_starts_with(c, "rgb(") || axctl_starts_with(c, "RGB(")) {
        char *paren = strchr(c, '(');
        if (paren) {
            c = paren + 1;
            char *end = strchr(c, ')');
            if (end) *end = '\0';
        }
    }

    /* Strip # */
    if (*c == '#') c++;

    /* Strip 0x */
    if (c[0] == '0' && (c[1] == 'x' || c[1] == 'X')) c += 2;

    size_t len = strlen(c);
    char hex[16] = {0};

    if (len == 3) {
        /* RGB shorthand -> RRGGBBAA */
        snprintf(hex, sizeof(hex), "%c%c%c%c%c%cff",
                 c[0], c[0], c[1], c[1], c[2], c[2]);
    } else if (len == 4) {
        /* RGBA shorthand -> RRGGBBAA */
        snprintf(hex, sizeof(hex), "%c%c%c%c%c%c%c%c",
                 c[0], c[0], c[1], c[1], c[2], c[2], c[3], c[3]);
    } else if (len == 6) {
        /* RRGGBB -> RRGGBBAA (append opaque alpha) */
        snprintf(hex, sizeof(hex), "%sff", c);
    } else if (len >= 8) {
        /* Already RRGGBBAA */
        strncpy(hex, c, 8);
        hex[8] = '\0';
    } else {
        strncpy(hex, c, sizeof(hex) - 1);
    }

    free(token);
    return axctl_sprintf("0x%s", hex);
}

char *axctl_hyprland_color(const char *hex_str) {
    if (!hex_str || !*hex_str) return axctl_strdup("rgba(00000000)");

    const char *c = hex_str;
    if (*c == '#') c++;

    char r[3] = "00", g[3] = "00", b[3] = "00", a[3] = "ff";
    size_t len = strlen(c);
    if (len >= 6) {
        r[0] = c[0]; r[1] = c[1];
        g[0] = c[2]; g[1] = c[3];
        b[0] = c[4]; b[1] = c[5];
    }
    if (len >= 8) {
        a[0] = c[6]; a[1] = c[7];
    }
    return axctl_sprintf("rgba(%s%s%s%s)", r, g, b, a);
}

/*
 * axctl_parse_color_string — port of Go's parseColorString()
 *
 * Splits a space-delimited color string into individual tokens:
 *   - If token is already rgb()/rgba() → pass through unchanged
 *   - If token starts with # or is 6/8 hex chars → convert via axctl_hyprland_color
 *   - If token ends with "deg" → treat as gradient angle
 *
 * out_colors receives the joined color tokens (caller frees).
 * out_angle  receives the angle token or NULL   (caller frees if non-NULL).
 */
void axctl_parse_color_string(const char *str, char **out_colors, char **out_angle) {
    *out_colors = NULL;
    *out_angle  = NULL;
    if (!str || !*str) return;

    /* Work on a mutable copy for strtok */
    char *buf = axctl_strdup(str);
    char *colors = axctl_strdup("");
    char *angle  = NULL;
    int color_count = 0;

    char *saveptr = NULL;
    char *tok = strtok_r(buf, " \t", &saveptr);
    while (tok) {
        size_t tlen = strlen(tok);

        /* Angle token: ends with "deg" */
        if (tlen > 3 && strcmp(tok + tlen - 3, "deg") == 0) {
            free(angle);
            angle = axctl_strdup(tok);
            tok = strtok_r(NULL, " \t", &saveptr);
            continue;
        }

        /* Already wrapped in rgb()/rgba() → pass through */
        if (strncmp(tok, "rgb(", 4) == 0 || strncmp(tok, "rgba(", 5) == 0) {
            if (color_count > 0) axctl_str_append(&colors, " ");
            axctl_str_append(&colors, tok);
            color_count++;
            tok = strtok_r(NULL, " \t", &saveptr);
            continue;
        }

        /* Hex color: starts with # or is exactly 6/8 hex chars */
        int is_hex = 0;
        if (tok[0] == '#') {
            is_hex = 1;
        } else if (tlen == 6 || tlen == 8) {
            is_hex = 1;
            for (size_t i = 0; i < tlen; i++) {
                if (!isxdigit((unsigned char)tok[i])) { is_hex = 0; break; }
            }
        }

        if (is_hex) {
            char *formatted = axctl_hyprland_color(tok);
            if (color_count > 0) axctl_str_append(&colors, " ");
            axctl_str_append(&colors, formatted);
            free(formatted);
            color_count++;
        }
        /* else: unknown token, skip (matches Go behaviour) */

        tok = strtok_r(NULL, " \t", &saveptr);
    }

    free(buf);

    if (color_count > 0) {
        *out_colors = colors;
    } else {
        free(colors);
    }
    *out_angle = angle;
}
