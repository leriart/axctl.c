/*
 * axctl - String utility implementations
 */
#include "utils/strutil.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <ctype.h>

char *axctl_strdup(const char *s) {
    if (!s) return NULL;
    size_t len = strlen(s);
    char *dup = malloc(len + 1);
    if (!dup) return NULL;
    memcpy(dup, s, len + 1);
    return dup;
}

char *axctl_sprintf(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    int len = vsnprintf(NULL, 0, fmt, args);
    va_end(args);
    if (len < 0) return NULL;

    char *buf = malloc((size_t)len + 1);
    if (!buf) return NULL;

    va_start(args, fmt);
    vsnprintf(buf, (size_t)len + 1, fmt, args);
    va_end(args);
    return buf;
}

char *axctl_strcat(const char *a, const char *b) {
    if (!a && !b) return axctl_strdup("");
    if (!a) return axctl_strdup(b);
    if (!b) return axctl_strdup(a);

    size_t la = strlen(a), lb = strlen(b);
    char *out = malloc(la + lb + 1);
    if (!out) return NULL;
    memcpy(out, a, la);
    memcpy(out + la, b, lb + 1);
    return out;
}

void axctl_str_append(char **dst, const char *src) {
    if (!src) return;
    if (!*dst) {
        *dst = axctl_strdup(src);
        return;
    }
    char *old = *dst;
    *dst = axctl_strcat(old, src);
    free(old);
}

char *axctl_capitalize(const char *s) {
    if (!s || !*s) return axctl_strdup("");

    /* Split by '-' and capitalize each part */
    size_t len = strlen(s);
    char *result = malloc(len + 1);
    if (!result) return NULL;

    size_t ri = 0;
    bool next_upper = true;
    for (size_t i = 0; i < len; i++) {
        if (s[i] == '-') {
            next_upper = true;
            continue;
        }
        if (next_upper && s[i] >= 'a' && s[i] <= 'z') {
            result[ri++] = s[i] - 32;
        } else {
            result[ri++] = s[i];
        }
        next_upper = false;
    }
    result[ri] = '\0';
    return result;
}

bool axctl_str_icontains(const char *haystack, const char *needle) {
    if (!haystack || !needle) return false;
    size_t hlen = strlen(haystack), nlen = strlen(needle);
    if (nlen == 0) return true;
    if (nlen > hlen) return false;

    for (size_t i = 0; i <= hlen - nlen; i++) {
        bool match = true;
        for (size_t j = 0; j < nlen; j++) {
            if (tolower((unsigned char)haystack[i+j]) != tolower((unsigned char)needle[j])) {
                match = false;
                break;
            }
        }
        if (match) return true;
    }
    return false;
}

int axctl_strcasecmp(const char *a, const char *b) {
    if (!a && !b) return 0;
    if (!a) return -1;
    if (!b) return 1;
    while (*a && *b) {
        int ca = tolower((unsigned char)*a);
        int cb = tolower((unsigned char)*b);
        if (ca != cb) return ca - cb;
        a++; b++;
    }
    return tolower((unsigned char)*a) - tolower((unsigned char)*b);
}

char *axctl_strtrim(char *buf) {
    if (!buf) return NULL;
    while (*buf && isspace((unsigned char)*buf)) buf++;
    if (!*buf) return buf;
    char *end = buf + strlen(buf) - 1;
    while (end > buf && isspace((unsigned char)*end)) *end-- = '\0';
    return buf;
}

bool axctl_starts_with(const char *s, const char *prefix) {
    if (!s || !prefix) return false;
    return strncmp(s, prefix, strlen(prefix)) == 0;
}

bool axctl_ends_with(const char *s, const char *suffix) {
    if (!s || !suffix) return false;
    size_t sl = strlen(s), ul = strlen(suffix);
    if (ul > sl) return false;
    return strcmp(s + sl - ul, suffix) == 0;
}

int axctl_strsplit(const char *s, char delim, char ***out) {
    if (!s || !out) return 0;

    int count = 1;
    for (const char *p = s; *p; p++) {
        if (*p == delim) count++;
    }

    *out = calloc((size_t)count, sizeof(char *));
    if (!*out) return 0;

    int idx = 0;
    const char *start = s;
    for (const char *p = s; ; p++) {
        if (*p == delim || *p == '\0') {
            size_t len = (size_t)(p - start);
            (*out)[idx] = malloc(len + 1);
            if ((*out)[idx]) {
                memcpy((*out)[idx], start, len);
                (*out)[idx][len] = '\0';
            }
            idx++;
            if (!*p) break;
            start = p + 1;
        }
    }
    return count;
}

void axctl_strsplit_free(char **arr, int count) {
    if (!arr) return;
    for (int i = 0; i < count; i++) free(arr[i]);
    free(arr);
}

char *axctl_str_replace(const char *s, const char *old_str, const char *new_str) {
    if (!s || !old_str) return axctl_strdup(s);
    const char *pos = strstr(s, old_str);
    if (!pos) return axctl_strdup(s);

    size_t prefix_len = (size_t)(pos - s);
    size_t old_len = strlen(old_str);
    size_t new_len = new_str ? strlen(new_str) : 0;
    size_t result_len = strlen(s) - old_len + new_len;

    char *result = malloc(result_len + 1);
    if (!result) return NULL;

    memcpy(result, s, prefix_len);
    if (new_str) memcpy(result + prefix_len, new_str, new_len);
    strcpy(result + prefix_len + new_len, pos + old_len);
    return result;
}
