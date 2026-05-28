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

#endif /* AXCTL_IPC_COLORS_H */
