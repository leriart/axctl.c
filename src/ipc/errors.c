/*
 * axctl - IPC error handling
 */
#include "ipc/errors.h"
#include <stdio.h>
#include <stdarg.h>
#include <string.h>

static __thread char g_error_buf[1024] = {0};

void axctl_set_error(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vsnprintf(g_error_buf, sizeof(g_error_buf), fmt, args);
    va_end(args);
}

const char *axctl_get_error(void) {
    return g_error_buf;
}

void axctl_clear_error(void) {
    g_error_buf[0] = '\0';
}

const char *axctl_error_code_str(int code) {
    switch (code) {
    case AXCTL_OK:                     return "OK";
    case AXCTL_ERR_NOT_SUPPORTED:      return "FEATURE_NOT_SUPPORTED";
    case AXCTL_ERR_WINDOW_NOT_FOUND:   return "WINDOW_NOT_FOUND";
    case AXCTL_ERR_WORKSPACE_NOT_FOUND:return "WORKSPACE_NOT_FOUND";
    case AXCTL_ERR_COMPOSITOR_UNAVAIL: return "COMPOSITOR_NOT_AVAILABLE";
    case AXCTL_ERR_SUBSCRIPTION_FAILED:return "SUBSCRIPTION_FAILED";
    case AXCTL_ERR_OPERATION_FAILED:   return "OPERATION_FAILED";
    case AXCTL_ERR_INVALID_PARAMS:     return "INVALID_PARAMS";
    case AXCTL_ERR_CONNECT:            return "CONNECTION_ERROR";
    case AXCTL_ERR_IO:                 return "IO_ERROR";
    case AXCTL_ERR_PARSE:              return "PARSE_ERROR";
    case AXCTL_ERR_OOM:                return "OUT_OF_MEMORY";
    default:                           return "UNKNOWN_ERROR";
    }
}
