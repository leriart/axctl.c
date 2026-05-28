/*
 * axctl - Color format conversion utilities
 *
 * Different compositors use different color formats:
 *   Hyprland: rgba(RRGGBBAA)
 *   Niri:     #RRGGBBAA
 *   Mango:    0xRRGGBBAA
 */
#ifndef AXCTL_IPC_COLORS_H
#define AXCTL_IPC_COLORS_H

/* Extract the first color token from a space-separated string.
 * Caller must free the result. */
char *axctl_first_color(const char *color_str);

/* Convert any color format to Mango's 0xRRGGBBAA format.
 * Caller must free the result. */
char *axctl_mango_color(const char *color_str);

/* Convert a hex color to Hyprland rgba(RRGGBBAA) format.
 * Caller must free the result. */
char *axctl_hyprland_color(const char *hex_str);

/*
 * Parse a color string that may contain:
 *   - Multiple space-separated colors (gradient): "#ff3838 #222222 45deg"
 *   - Pre-formatted rgba()/rgb() values:         "rgba(ff3838ff)"
 *   - Raw hex:                                    "#ff3838"
 *
 * Returns the formatted color string (caller must free) via out_colors,
 * and the angle string (caller must free, or NULL) via out_angle.
 *
 * Matches Go's parseColorString() from generator.go.
 */
void axctl_parse_color_string(const char *str, char **out_colors, char **out_angle);

#endif /* AXCTL_IPC_COLORS_H */
