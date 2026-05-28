/*
 * axctl - String utility functions
 */
#ifndef AXCTL_STRUTIL_H
#define AXCTL_STRUTIL_H

#include <stdbool.h>
#include <stddef.h>

/* Duplicate a string (caller must free) */
char *axctl_strdup(const char *s);

/* Format a string (caller must free) */
char *axctl_sprintf(const char *fmt, ...);

/* Concatenate strings (caller must free the result) */
char *axctl_strcat(const char *a, const char *b);

/* Append src onto *dst, reallocating *dst. *dst may be NULL initially. */
void axctl_str_append(char **dst, const char *src);

/* Convert kebab-case to PascalCase: "focus-dir" -> "FocusDir" */
char *axctl_capitalize(const char *s);

/* Case-insensitive string contains */
bool axctl_str_icontains(const char *haystack, const char *needle);

/* Case-insensitive comparison */
int axctl_strcasecmp(const char *a, const char *b);

/* Trim whitespace from both ends, returns pointer into buf (modifies buf) */
char *axctl_strtrim(char *buf);

/* Check if string starts with prefix */
bool axctl_starts_with(const char *s, const char *prefix);

/* Check if string ends with suffix */
bool axctl_ends_with(const char *s, const char *suffix);

/* Split string by delimiter into an array. Returns count. Caller frees array and elements. */
int axctl_strsplit(const char *s, char delim, char ***out);

/* Free a string array returned by axctl_strsplit */
void axctl_strsplit_free(char **arr, int count);

/* Replace first occurrence of old with new in string. Caller frees result. */
char *axctl_str_replace(const char *s, const char *old_str, const char *new_str);

#endif /* AXCTL_STRUTIL_H */
