/*
 * axctl - Logging implementation
 */
#include "utils/log.h"
#include <stdio.h>
#include <stdarg.h>
#include <time.h>
#include <string.h>

static log_level_t g_log_level = AXCTL_LOG_INFO;

static const char *level_names[] = {"DEBUG", "INFO", "WARN", "ERROR"};
static const char *level_colors[] = {"\033[36m", "\033[32m", "\033[33m", "\033[31m"};

void log_set_level(log_level_t level) {
    g_log_level = level;
}

static void log_msg(log_level_t level, const char *fmt, va_list args) {
    if (level < g_log_level) return;

    time_t now = time(NULL);
    struct tm *tm_info = localtime(&now);
    char time_buf[32];
    strftime(time_buf, sizeof(time_buf), "%H:%M:%S", tm_info);

    fprintf(stderr, "%s[%s %s]\033[0m ", level_colors[level], time_buf, level_names[level]);
    vfprintf(stderr, fmt, args);
    fprintf(stderr, "\n");
    fflush(stderr);
}

void log_debug(const char *fmt, ...) {
    va_list args; va_start(args, fmt); log_msg(AXCTL_LOG_DEBUG, fmt, args); va_end(args);
}

void log_info(const char *fmt, ...) {
    va_list args; va_start(args, fmt); log_msg(AXCTL_LOG_INFO, fmt, args); va_end(args);
}

void log_warn(const char *fmt, ...) {
    va_list args; va_start(args, fmt); log_msg(AXCTL_LOG_WARN, fmt, args); va_end(args);
}

void log_error(const char *fmt, ...) {
    va_list args; va_start(args, fmt); log_msg(AXCTL_LOG_ERROR, fmt, args); va_end(args);
}
