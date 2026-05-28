/*
 * Test suite for axctl_parse_color_string and color functions.
 * Verifies that color parsing matches Go's parseColorString().
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ipc/colors.h"
#include "utils/strutil.h"

static int tests_run = 0;
static int tests_passed = 0;

#define ASSERT_STR_EQ(got, expected, label) do { \
    tests_run++; \
    if ((got) == NULL && (expected) == NULL) { \
        tests_passed++; \
        printf("  ✓ %s: NULL == NULL\n", label); \
    } else if ((got) && (expected) && strcmp((got), (expected)) == 0) { \
        tests_passed++; \
        printf("  ✓ %s: \"%s\"\n", label, got); \
    } else { \
        printf("  ✗ %s: expected \"%s\", got \"%s\"\n", label, \
            (expected) ? (expected) : "(null)", (got) ? (got) : "(null)"); \
    } \
} while(0)

static void test_hyprland_color(void) {
    printf("\n=== axctl_hyprland_color ===\n");

    char *c;
    c = axctl_hyprland_color("#ff3838");
    ASSERT_STR_EQ(c, "rgba(ff3838ff)", "6-digit hex with #");
    free(c);

    c = axctl_hyprland_color("ff3838");
    ASSERT_STR_EQ(c, "rgba(ff3838ff)", "6-digit hex without #");
    free(c);

    c = axctl_hyprland_color("#ff383880");
    ASSERT_STR_EQ(c, "rgba(ff383880)", "8-digit hex with #");
    free(c);

    c = axctl_hyprland_color("");
    ASSERT_STR_EQ(c, "rgba(00000000)", "empty string");
    free(c);

    c = axctl_hyprland_color(NULL);
    ASSERT_STR_EQ(c, "rgba(00000000)", "NULL");
    free(c);
}

static void test_parse_color_string(void) {
    printf("\n=== axctl_parse_color_string ===\n");

    char *colors = NULL, *angle = NULL;

    /* Single hex color */
    axctl_parse_color_string("#ff3838", &colors, &angle);
    ASSERT_STR_EQ(colors, "rgba(ff3838ff)", "single hex → rgba");
    ASSERT_STR_EQ(angle, NULL, "single hex → no angle");
    free(colors); free(angle); colors = NULL; angle = NULL;

    /* Already rgba() → pass through */
    axctl_parse_color_string("rgba(ff3838ff)", &colors, &angle);
    ASSERT_STR_EQ(colors, "rgba(ff3838ff)", "passthrough rgba()");
    ASSERT_STR_EQ(angle, NULL, "passthrough → no angle");
    free(colors); free(angle); colors = NULL; angle = NULL;

    /* Already rgb() → pass through */
    axctl_parse_color_string("rgb(ff3838)", &colors, &angle);
    ASSERT_STR_EQ(colors, "rgb(ff3838)", "passthrough rgb()");
    free(colors); free(angle); colors = NULL; angle = NULL;

    /* Gradient: two colors + angle */
    axctl_parse_color_string("#ff3838 #222222 45deg", &colors, &angle);
    ASSERT_STR_EQ(colors, "rgba(ff3838ff) rgba(222222ff)", "gradient colors");
    ASSERT_STR_EQ(angle, "45deg", "gradient angle");
    free(colors); free(angle); colors = NULL; angle = NULL;

    /* Gradient: pre-formatted + angle */
    axctl_parse_color_string("rgba(ff3838ff) rgba(222222ff) 90deg", &colors, &angle);
    ASSERT_STR_EQ(colors, "rgba(ff3838ff) rgba(222222ff)", "gradient passthrough");
    ASSERT_STR_EQ(angle, "90deg", "gradient angle passthrough");
    free(colors); free(angle); colors = NULL; angle = NULL;

    /* Mixed: hex + rgba */
    axctl_parse_color_string("#ff3838 rgba(222222ff)", &colors, &angle);
    ASSERT_STR_EQ(colors, "rgba(ff3838ff) rgba(222222ff)", "mixed hex+rgba");
    free(colors); free(angle); colors = NULL; angle = NULL;

    /* Empty string */
    axctl_parse_color_string("", &colors, &angle);
    ASSERT_STR_EQ(colors, NULL, "empty → null colors");
    ASSERT_STR_EQ(angle, NULL, "empty → null angle");
    free(colors); free(angle); colors = NULL; angle = NULL;

    /* NULL */
    axctl_parse_color_string(NULL, &colors, &angle);
    ASSERT_STR_EQ(colors, NULL, "NULL → null colors");
    ASSERT_STR_EQ(angle, NULL, "NULL → null angle");

    /* BUG CASE: The screenshot error was caused by passing rgba() to
     * axctl_hyprland_color, producing rgba(rgba(...)). parseColorString
     * should pass rgba() through unchanged. */
    axctl_parse_color_string("rgba(ff3838ff)", &colors, &angle);
    ASSERT_STR_EQ(colors, "rgba(ff3838ff)", "BUG FIX: no double-wrapping");
    free(colors); free(angle); colors = NULL; angle = NULL;

    /* Raw 6-char hex without # */
    axctl_parse_color_string("ff3838", &colors, &angle);
    ASSERT_STR_EQ(colors, "rgba(ff3838ff)", "raw 6-char hex");
    free(colors); free(angle); colors = NULL; angle = NULL;

    /* Raw 8-char hex without # */
    axctl_parse_color_string("ff383880", &colors, &angle);
    ASSERT_STR_EQ(colors, "rgba(ff383880)", "raw 8-char hex");
    free(colors); free(angle);
}

int main(void) {
    test_hyprland_color();
    test_parse_color_string();

    printf("\n=== Results: %d/%d passed ===\n", tests_passed, tests_run);
    return tests_passed == tests_run ? 0 : 1;
}
