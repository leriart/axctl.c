/*
 * axctl - Universal IPC daemon for Wayland compositors
 * Logging utilities
 */
#ifndef AXCTL_LOG_H
#define AXCTL_LOG_H

#include <stdio.h>
#include <stdarg.h>

typedef enum {
    AXCTL_LOG_DEBUG = 0,
    AXCTL_LOG_INFO  = 1,
    AXCTL_LOG_WARN  = 2,
    AXCTL_LOG_ERROR = 3
} log_level_t;

/* Set the global minimum log level */
void log_set_level(log_level_t level);

/* Core logging functions */
void log_debug(const char *fmt, ...);
void log_info(const char *fmt, ...);
void log_warn(const char *fmt, ...);
void log_error(const char *fmt, ...);

/* Convenience macros for LOG_xxx style usage across codebase */
#define LOG_DEBUG(...)  log_debug(__VA_ARGS__)
#define LOG_INFO(...)   log_info(__VA_ARGS__)
#define LOG_WARN(...)   log_warn(__VA_ARGS__)
#define LOG_ERROR(...)  log_error(__VA_ARGS__)

#endif /* AXCTL_LOG_H */
